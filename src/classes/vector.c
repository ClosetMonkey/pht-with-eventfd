/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Thomas Punt <tpunt@php.net>                                  |
  +----------------------------------------------------------------------+
*/

#include <Zend/zend_API.h>
#include <Zend/zend_exceptions.h>

#include "php_pht.h"
#include "src/pht_entry.h"
#include "src/classes/vector.h"

zend_object_handlers vector_handlers;
zend_class_entry *Vector_ce;

void free_vector_internal(vector_obj_internal_t *voi)
{
    pthread_mutex_destroy(&voi->lock);

    for (int i = 0; i < voi->vector.used; ++i) {
        // @todo check if object is either another MQ or a HT (its refcount will
        // need to be decremented if so).
        // This should go into a specific vector_destroy method.
        pht_entry_delete(voi->vector.values[i]);
    }

    free(voi->vector.values);
    free(voi);
}

static zend_object *vector_ctor(zend_class_entry *entry)
{
    vector_obj_t *vo = ecalloc(1, sizeof(vector_obj_t) + zend_object_properties_size(entry));

    zend_object_std_init(&vo->obj, entry);
    object_properties_init(&vo->obj, entry);

    vo->obj.handlers = &vector_handlers;
    vo->vn = 0;

    if (!PHT_ZG(skip_voi_creation)) {
        vector_obj_internal_t *voi = calloc(1, sizeof(vector_obj_internal_t));

        pht_vector_init(&voi->vector, 2);
        pthread_mutex_init(&voi->lock, NULL);
        voi->refcount = 1;
        voi->vn = 0;

        vo->voi = voi;
    }

    return &vo->obj;
}

void vo_dtor_obj(zend_object *obj)
{
    zend_object_std_dtor(obj);
}

void vo_free_obj(zend_object *obj)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)obj - obj->handlers->offset);

    pthread_mutex_lock(&vo->voi->lock);
    --vo->voi->refcount;
    pthread_mutex_unlock(&vo->voi->lock);

    if (!vo->voi->refcount) {
        free_vector_internal(vo->voi);
    }
}

zval *vo_read_dimension(zval *zobj, zval *offset, int type, zval *rv)
{
    if (offset == NULL) {
        zend_throw_error(NULL, "Cannot read an empty offset"); // $v[] = 1 ?
        return NULL;
    }

    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ_P(zobj) - Z_OBJ_P(zobj)->handlers->offset);
    pht_entry_t *e = NULL;

    switch (Z_TYPE_P(offset)) {
        case IS_LONG:
            e = pht_vector_fetch_at(&vo->voi->vector, Z_LVAL_P(offset));
            break;
        default:
            // @todo cast IS_STRING or IS_DOUBLE to int? cater for Object::__toString()?
            zend_throw_error(NULL, "Invalid offset type");
            return NULL;
    }

    if (!e) {
        if (type != BP_VAR_IS) {
            zend_throw_error(NULL, "Undefined offset");
        }
        return NULL; // correct?
    }

    pht_convert_entry_to_zval(rv, e);

    return rv;
}

void vo_write_dimension(zval *zobj, zval *offset, zval *value)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ_P(zobj) - Z_OBJ_P(zobj)->handlers->offset);
    pht_entry_t *entry = create_new_entry(value);

    if (!entry) {
        zend_throw_error(NULL, "Failed to serialise the value");
        return;
    }

    if (!offset) {
        pht_vector_push(&vo->voi->vector, entry);
        ++vo->voi->vn;
        return;
    }

    switch (Z_TYPE_P(offset)) {
        case IS_LONG:
            {
                if (!pht_vector_update_at(&vo->voi->vector, entry, Z_LVAL_P(offset))) {
                    zend_throw_error(NULL, "Invalid index - the index must be within the array size");
                    return;
                }

                ++vo->voi->vn;
            }
            break;
        default:
            // @todo cast IS_STRING or IS_DOUBLE to int? cater for Object::__toString()?
            zend_throw_error(NULL, "Invalid offset type");
            return;
    }
}

int vo_has_dimension(zval *zobj, zval *offset, int check_empty)
{
    zend_object *obj = Z_OBJ_P(zobj);
    vector_obj_t *vo = (vector_obj_t *)((char *)obj - obj->handlers->offset);
    pht_entry_t *entry = NULL;

    switch (Z_TYPE_P(offset)) {
        case IS_LONG:
            entry = pht_vector_fetch_at(&vo->voi->vector, Z_LVAL_P(offset));
            break;
        default:
            // @todo cast IS_STRING or IS_DOUBLE to int? cater for Object::__toString()?
            zend_throw_error(NULL, "Invalid offset type");
            return 0;
    }

    if (!entry) {
        return 0;
    }

    if (!check_empty) {
        return PHT_ENTRY_TYPE(entry) != IS_NULL;
    }

    zval value;
    int result;

    pht_convert_entry_to_zval(&value, entry);
    result = i_zend_is_true(&value);
    zval_ptr_dtor(&value);

    return result;
}

void vo_unset_dimension(zval *zobj, zval *offset)
{
    zend_object *obj = Z_OBJ_P(zobj);
    vector_obj_t *vo = (vector_obj_t *)((char *)obj - obj->handlers->offset);

    switch (Z_TYPE_P(offset)) {
        case IS_LONG:
            if (!pht_vector_delete_at(&vo->voi->vector, Z_LVAL_P(offset))) {
                zend_throw_error(NULL, "Invalid index - the index must be within the array size");
                return;
            }
            ++vo->voi->vn;
            break;
        default:
            zend_throw_error(NULL, "Invalid offset type"); // @todo cater for Object::__toString()?
    }
}

HashTable *vo_get_properties(zval *zobj)
{
    zend_object *obj = Z_OBJ_P(zobj);
    vector_obj_t *vo = (vector_obj_t *)((char *)obj - obj->handlers->offset);

    if (obj->properties && vo->vn == vo->voi->vn) {
        return obj->properties;
    }

    HashTable *zht = emalloc(sizeof(HashTable));

    zend_hash_init(zht, pht_vector_size(&vo->voi->vector), NULL, ZVAL_PTR_DTOR, 0);
    pht_vector_to_zend_hashtable(zht, &vo->voi->vector);

    if (obj->properties) {
        // @todo safe? Perhaps just wipe HT and insert into it instead?
        GC_REFCOUNT(obj->properties) = 0;
        zend_array_destroy(obj->properties);
    }

    obj->properties = zht;
    vo->vn = vo->voi->vn;

    return zht;
}

ZEND_BEGIN_ARG_INFO_EX(Vector_push_arginfo, 0, 0, 1)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, push)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);
    zval *value;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    pht_entry_t *entry = create_new_entry(value);

    if (!entry) {
        zend_throw_error(NULL, "Failed to serialise the value");
        return;
    }

    pht_vector_push(&vo->voi->vector, entry);
    ++vo->voi->vn;
}

ZEND_BEGIN_ARG_INFO_EX(Vector_pop_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, pop)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pht_entry_t *entry = pht_vector_pop(&vo->voi->vector);

    if (!entry) {
        zend_throw_error(NULL, "Attempted to pop an element from an empty vector");
        return;
    }

    pht_convert_entry_to_zval(return_value, entry);
    pht_entry_delete(entry);
    ++vo->voi->vn;
}

ZEND_BEGIN_ARG_INFO_EX(Vector_shift_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, shift)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pht_entry_t *entry = pht_vector_shift(&vo->voi->vector);

    if (!entry) {
        zend_throw_error(NULL, "Attempted to shift an element from an empty vector");
        return;
    }

    pht_convert_entry_to_zval(return_value, entry);
    pht_entry_delete(entry);
    ++vo->voi->vn;
}

ZEND_BEGIN_ARG_INFO_EX(Vector_unshift_arginfo, 0, 0, 1)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, unshift)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);
    zval *value;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(value)
    ZEND_PARSE_PARAMETERS_END();

    pht_entry_t *entry = create_new_entry(value);

    if (!entry) {
        zend_throw_error(NULL, "Failed to serialise the value");
        return;
    }

    pht_vector_unshift(&vo->voi->vector, entry);
    ++vo->voi->vn;
}

ZEND_BEGIN_ARG_INFO_EX(Vector_delete_arginfo, 0, 0, 1)
    ZEND_ARG_INFO(0, index)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, delete)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);
    zend_long index;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(index)
    ZEND_PARSE_PARAMETERS_END();

    if (!pht_vector_delete_at(&vo->voi->vector, index)) {
        zend_throw_error(NULL, "Attempted to delete an element from an out-of-bounds index");
        return;
    }

    ++vo->voi->vn;
}

ZEND_BEGIN_ARG_INFO_EX(Vector_lock_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, lock)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pthread_mutex_lock(&vo->voi->lock);
}

ZEND_BEGIN_ARG_INFO_EX(Vector_unlock_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, unlock)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    pthread_mutex_unlock(&vo->voi->lock);
}

ZEND_BEGIN_ARG_INFO_EX(Vector_size_arginfo, 0, 0, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(Vector, size)
{
    vector_obj_t *vo = (vector_obj_t *)((char *)Z_OBJ(EX(This)) - Z_OBJ(EX(This))->handlers->offset);

    if (zend_parse_parameters_none() != SUCCESS) {
        return;
    }

    RETVAL_LONG(pht_vector_size(&vo->voi->vector));
}

zend_function_entry Vector_methods[] = {
    PHP_ME(Vector, push, Vector_push_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, pop, Vector_pop_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, shift, Vector_shift_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, unshift, Vector_unshift_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, delete, Vector_delete_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, lock, Vector_lock_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, unlock, Vector_unlock_arginfo, ZEND_ACC_PUBLIC)
    PHP_ME(Vector, size, Vector_size_arginfo, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

zval *vo_read_property(zval *object, zval *member, int type, void **cache, zval *rv)
{
    zend_throw_exception(zend_ce_exception, "Properties on Vector objects are not enabled", 0);

    return &EG(uninitialized_zval);
}

void vo_write_property(zval *object, zval *member, zval *value, void **cache_slot)
{
    zend_throw_exception(zend_ce_exception, "Properties on Vector objects are not enabled", 0);
}

void vector_ce_init(void)
{
    zend_class_entry ce;
    zend_object_handlers *zh = zend_get_std_object_handlers();

    INIT_CLASS_ENTRY(ce, "Vector", Vector_methods);
    Vector_ce = zend_register_internal_class(&ce);
    Vector_ce->create_object = vector_ctor;

    memcpy(&vector_handlers, zh, sizeof(zend_object_handlers));

    vector_handlers.offset = XtOffsetOf(vector_obj_t, obj);
    vector_handlers.dtor_obj = vo_dtor_obj;
    vector_handlers.free_obj = vo_free_obj;
    vector_handlers.read_property = vo_read_property;
    vector_handlers.write_property = vo_write_property;
    vector_handlers.read_dimension = vo_read_dimension;
    vector_handlers.write_dimension = vo_write_dimension;
    vector_handlers.has_dimension = vo_has_dimension;
    vector_handlers.unset_dimension = vo_unset_dimension;
    vector_handlers.get_properties = vo_get_properties;
}