/*
  +----------------------------------------------------------------------+
  | Copyright (c) 2009 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Andrei Zmievski <andrei@php.net>                            |
  +----------------------------------------------------------------------+
*/

/* $ Id: $ */

/* TODO
 * parse client Id in constructor
 * add version to MINFO
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>

#ifdef ZTS
#include "TSRM.h"
#endif

#include <php_ini.h>
#include <SAPI.h>
#include <ext/standard/info.h>
#include <zend_extensions.h>
#include <ext/standard/php_smart_str.h>

#include "php_zookeeper.h"

#define DEFAULT_RECV_TIMEOUT 10000

/****************************************
  Helper macros
****************************************/
#define ZK_METHOD_INIT_VARS                \
    zval*             object  = getThis(); \
    php_zk_t*         i_obj   = NULL;      \

#define ZK_METHOD_FETCH_OBJECT                                                 \
    i_obj = (php_zk_t *) zend_object_store_get_object( object TSRMLS_CC );   \
	if (!i_obj->zk) {	\
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Zookeeper connect was not called");	\
		return;	\
	} \

/****************************************
  Structures and definitions
****************************************/
typedef struct {
	zend_fcall_info fci;
	zend_fcall_info_cache fcc;
	zend_bool oneshot;
	ulong h;
} php_cb_data_t;

typedef struct {
	zend_object    zo;
	zhandle_t     *zk;
	php_cb_data_t *cb_data;
} php_zk_t;

static zend_class_entry *zookeeper_ce = NULL;

#if (PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION < 3)
const zend_fcall_info empty_fcall_info = { 0, NULL, NULL, NULL, NULL, 0, NULL, NULL, 0 };
#undef ZEND_BEGIN_ARG_INFO_EX
#define ZEND_BEGIN_ARG_INFO_EX(name, pass_rest_by_reference, return_reference, required_num_args)   \
    static zend_arg_info name[] = {                                                                       \
        { NULL, 0, NULL, 0, 0, 0, pass_rest_by_reference, return_reference, required_num_args },
#endif

ZEND_DECLARE_MODULE_GLOBALS(php_zookeeper)

#ifdef COMPILE_DL_ZOOKEEPER
ZEND_GET_MODULE(zookeeper)
#endif

/****************************************
  Forward declarations
****************************************/
static php_cb_data_t* php_cb_data_new(zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_bool oneshot TSRMLS_DC);
static void php_cb_data_destroy(php_cb_data_t **entry);
static void php_zk_watcher_marshal(zhandle_t *zk, int type, int state, const char *path, void *context);
static void php_zk_completion_marshal(int rc, const void *context);
static void php_parse_acl_list(zval *z_acl, struct ACL_vector *aclv);
static void php_aclv_destroy(struct ACL_vector *aclv);
static void php_stat_to_array(const struct Stat *stat, zval *array);
static void php_aclv_to_array(const struct ACL_vector *aclv, zval *array);

/****************************************
  Method implementations
****************************************/

static void php_zookeeper_connect_impl(INTERNAL_FUNCTION_PARAMETERS, char *host, zend_fcall_info *fci, zend_fcall_info_cache *fcc, long recv_timeout)
{
	zval *object = getThis();
	php_zk_t *i_obj;
	zhandle_t *zk = NULL;
	php_cb_data_t *cb_data = NULL;

	if (recv_timeout <= 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "recv_timeout parameter has to be greater than 0");
		ZVAL_NULL(object);
		return;
	}

	i_obj = (php_zk_t *) zend_object_store_get_object(object TSRMLS_CC);

	if (fci->size != 0) {
		cb_data = php_cb_data_new(fci, fcc, 0);
	}
	zk = zookeeper_init(host, (fci->size != 0) ? php_zk_watcher_marshal : NULL,
						recv_timeout, 0, cb_data, 0);

	if (zk == NULL) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "could not init zookeeper instance");
		/* not reached */
	}

	i_obj->zk = zk;
	i_obj->cb_data = cb_data;
}

/* {{{ Zookeeper::connect ( .. )
   Connects to a zookeeper host */
static PHP_METHOD(Zookeeper, connect)
{
	int host_len;
	char *host;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	long recv_timeout = DEFAULT_RECV_TIMEOUT;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f!l", &host, &host_len, &fci, &fcc, &recv_timeout) == FAILURE) {
		return;
	}

	php_zookeeper_connect_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, host, &fci, &fcc, recv_timeout);
}
/* }}} */


/* {{{ Zookeeper::__construct ( .. )
   Creates a Zookeeper object and optionally connects */
static PHP_METHOD(Zookeeper, __construct)
{
	zval *object = getThis();
	int host_len = 0;
	char *host = NULL;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	long recv_timeout = DEFAULT_RECV_TIMEOUT;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sf!l", &host, &host_len, &fci, &fcc, &recv_timeout) == FAILURE) {
		ZVAL_NULL(object);
		return;
	}

	if (host_len > 0)
	{
		php_zookeeper_connect_impl(INTERNAL_FUNCTION_PARAM_PASSTHRU, host, &fci, &fcc, recv_timeout);
	}

}
/* }}} */


/* {{{ Zookeeper::create( .. )
   */
static PHP_METHOD(Zookeeper, create)
{
	char *path, *value;
	int path_len, value_len;
	zval *acl_info = NULL;
	long flags = 0;
	char realpath[256];
	int realpath_max = 256;
	struct ACL_vector aclv = { 0, };
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ssa!|l", &path, &path_len,
							  &value, &value_len, &acl_info, &flags) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	php_parse_acl_list(acl_info, &aclv);
	status = zoo_create(i_obj->zk, path, value, value_len, (acl_info ? &aclv : 0), flags,
						realpath, realpath_max);
	if (status != ZOK) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	RETURN_STRING(realpath, 1);
}
/* }}} */

/* {{{ Zookeeper::delete( .. )
   */
static PHP_METHOD(Zookeeper, delete)
{
	char *path;
	int path_len;
	long version = -1;
	int status = ZOK;

	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &path, &path_len,
							  &version) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	status = zoo_delete(i_obj->zk, path, version);
	if (status != ZOK) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::getChildren( .. )
   */
static PHP_METHOD(Zookeeper, getChildren)
{
	char *path;
	int path_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	php_cb_data_t *cb_data = NULL;
	struct String_vector strings;
	int i, status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f", &path, &path_len, &fci,
							  &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&fci, &fcc, 1);
	}
	status = zoo_wget_children(i_obj->zk, path,
							   (fci.size != 0) ? php_zk_watcher_marshal : NULL,
							   cb_data, &strings);
	if (status != ZOK) {
		php_cb_data_destroy(&cb_data);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	array_init(return_value);
	for (i = 0; i < strings.count; i++) {
		add_next_index_string(return_value, strings.data[i], 1);
	}
}
/* }}} */

/* {{{ Zookeeper::get( .. )
   */
static PHP_METHOD(Zookeeper, get)
{
	char *path;
	int path_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	zval *stat_info = NULL;
	php_cb_data_t *cb_data = NULL;
	char buffer[512];
	int buffer_len = 512;
	struct Stat stat;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f!z", &path, &path_len, &fci,
							  &fcc, &stat_info) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&fci, &fcc, 1);
	}
	status = zoo_wget(i_obj->zk, path, (fci.size != 0) ? php_zk_watcher_marshal : NULL,
					  cb_data, buffer, &buffer_len, &stat);
	if (status != ZOK) {
		php_cb_data_destroy(&cb_data);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	if (stat_info) {
		zval_dtor(stat_info);
		php_stat_to_array(&stat, stat_info);
	}
	RETURN_STRINGL(buffer, buffer_len, 1);
}
/* }}} */

/* {{{ Zookeeper::exists( .. )
   */
static PHP_METHOD(Zookeeper, exists)
{
	char *path;
	int path_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	php_cb_data_t *cb_data = NULL;
	struct Stat stat;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|f", &path, &path_len, &fci,
							  &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&fci, &fcc, 1);
	}
	status = zoo_wexists(i_obj->zk, path, (fci.size != 0) ? php_zk_watcher_marshal : NULL,
						 cb_data, &stat);
	if (status != ZOK && status != ZNONODE) {
		php_cb_data_destroy(&cb_data);
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	if (status == ZOK) {
		php_stat_to_array(&stat, return_value);
		return;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ Zookeeper::set( .. )
   */
static PHP_METHOD(Zookeeper, set)
{
	char *path, *value;
	int path_len, value_len;
	long version = -1;
	zval *stat_info = NULL;
	struct Stat stat, *stat_ptr = NULL;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|lz", &path, &path_len,
							  &value, &value_len, &version, &stat_info) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (stat_info) {
		stat_ptr = &stat;
	}
	status = zoo_set2(i_obj->zk, path, value, value_len, version, stat_ptr);
	if (status != ZOK) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	if (stat_info) {
		php_stat_to_array(stat_ptr, stat_info);
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::getClientId( .. )
   */
static PHP_METHOD(Zookeeper, getClientId)
{
	const clientid_t *cid;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	cid = zoo_client_id(i_obj->zk);
	array_init(return_value);
	add_next_index_long(return_value, cid->client_id);
	add_next_index_string(return_value, (char *)cid->passwd, 1);
}
/* }}} */

/* {{{ Zookeeper::getAcl( .. )
   */
static PHP_METHOD(Zookeeper, getAcl)
{
	char *path;
	int path_len;
	int status = ZOK;
	struct ACL_vector aclv;
	struct Stat stat;
	zval *stat_info, *acl_info;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &path, &path_len) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	status = zoo_get_acl(i_obj->zk, path, &aclv, &stat);
	if (status != ZOK) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	MAKE_STD_ZVAL(acl_info);
	MAKE_STD_ZVAL(stat_info);
	php_aclv_to_array(&aclv, acl_info);
	php_stat_to_array(&stat, stat_info);
	array_init(return_value);
	add_next_index_zval(return_value, stat_info);
	add_next_index_zval(return_value, acl_info);
}
/* }}} */

/* {{{ Zookeeper::setAcl( .. )
   */
static PHP_METHOD(Zookeeper, setAcl)
{
	char *path;
	int path_len;
	long version;
	zval *acl_info;
	struct ACL_vector aclv;
	int status = ZOK;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sla", &path, &path_len,
							  &version, &acl_info) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	php_parse_acl_list(acl_info, &aclv);
	status = zoo_set_acl(i_obj->zk, path, version, &aclv);
	php_aclv_destroy(&aclv);
	if (status != ZOK) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::getState( .. )
   */
static PHP_METHOD(Zookeeper, getState)
{
	int state;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	state = zoo_state(i_obj->zk);
	RETURN_LONG(state);
}

/* {{{ Zookeeper::getRecvTimeout( .. )
   */
static PHP_METHOD(Zookeeper, getRecvTimeout)
{
	int recv_timeout;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	recv_timeout = zoo_recv_timeout(i_obj->zk);
	RETURN_LONG(recv_timeout);
}

/* {{{ Zookeeper::isRecoverable( .. )
   */
static PHP_METHOD(Zookeeper, isRecoverable)
{
	int result;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	result = is_unrecoverable(i_obj->zk);
	RETURN_BOOL(!result);
}

/* {{{ Zookeeper::setDebugLevel( .. )
   */
static PHP_METHOD(Zookeeper, setDebugLevel)
{
	int level;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &level) == FAILURE) {
		return;
	}

	zoo_set_debug_level((ZooLogLevel)level);
	RETURN_TRUE;
}

/* {{{ Zookeeper::setDeterministicConnOrder( .. )
   */
static PHP_METHOD(Zookeeper, setDeterministicConnOrder)
{
	zend_bool value;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "b", &value) == FAILURE) {
		return;
	}

	zoo_deterministic_conn_order(value);
	RETURN_TRUE;
}

/* {{{ Zookeeper::addAuth( .. )
   */
static PHP_METHOD(Zookeeper, addAuth)
{
	char *scheme, *cert;
	int scheme_len, cert_len;
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	int status = ZOK;
	php_cb_data_t *cb_data = NULL;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|f", &scheme, &scheme_len, &cert,
							  &cert_len, &fci, &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (fci.size != 0) {
		cb_data = php_cb_data_new(&fci, &fcc, 0);
	}
	status = zoo_add_auth(i_obj->zk, scheme, cert, cert_len,
						  (fci.size != 0) ? php_zk_completion_marshal : NULL, cb_data);
	if (status != ZOK) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "error: %s", zerror(status));
		return;
	}

	RETURN_TRUE;
}

/* }}} */

/* {{{ Zookeeper::setWatcher( .. )
   */
static PHP_METHOD(Zookeeper, setWatcher)
{
	zend_fcall_info fci = empty_fcall_info;
	zend_fcall_info_cache fcc = empty_fcall_info_cache;
	php_cb_data_t *cb_data = NULL;
	ZK_METHOD_INIT_VARS;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "f", &fci, &fcc) == FAILURE) {
		return;
	}

	ZK_METHOD_FETCH_OBJECT;

	if (i_obj->cb_data) {
		zend_hash_index_del(&ZK_G(callbacks), i_obj->cb_data->h);
	}
	cb_data = php_cb_data_new(&fci, &fcc, 0);
	zoo_set_watcher(i_obj->zk, php_zk_watcher_marshal);
	i_obj->cb_data = cb_data;

	RETURN_TRUE;
}
/* }}} */

/* {{{ Zookeeper::setLogStream( .. )
   */
static PHP_METHOD(Zookeeper, setLogStream)
{
	zval *zstream;
	php_stream *stream;
	FILE *fp;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zstream) == FAILURE) {
		return;
	}

	if (Z_TYPE_P(zstream) == IS_RESOURCE) {
		php_stream_from_zval(stream, &zstream);
	} else {
		convert_to_string_ex(&zstream);
		stream = php_stream_open_wrapper(Z_STRVAL_P(zstream), "w", ENFORCE_SAFE_MODE|REPORT_ERRORS, NULL);
	}
	if (stream == NULL) {
		RETURN_FALSE;
	}

	if (FAILURE == php_stream_cast(stream, PHP_STREAM_AS_STDIO, (void *) &fp, REPORT_ERRORS)) {
		RETURN_FALSE;
	}

	zoo_set_log_stream(fp);

	if (Z_TYPE_P(zstream) == IS_RESOURCE) {
		php_stream_free(stream, PHP_STREAM_FREE_CLOSE_CASTED);
	}

	RETURN_TRUE;
}
/* }}} */

/****************************************
  Internal support code
****************************************/

/* {{{ constructor/destructor */
static void php_zk_destroy(php_zk_t *i_obj TSRMLS_DC)
{
	if (i_obj->cb_data) {
		zend_hash_index_del(&ZK_G(callbacks), i_obj->cb_data->h);
	}
	if (i_obj->zk) {
		zookeeper_close(i_obj->zk);
	}

	efree(i_obj);
}

static void php_zk_free_storage(php_zk_t *i_obj TSRMLS_DC)
{
	zend_object_std_dtor(&i_obj->zo TSRMLS_CC);
	php_zk_destroy(i_obj TSRMLS_CC);
}

zend_object_value php_zk_new(zend_class_entry *ce TSRMLS_DC)
{
    zend_object_value retval;
    php_zk_t *i_obj;
    zval *tmp;

    i_obj = ecalloc(1, sizeof(*i_obj));
	zend_object_std_init( &i_obj->zo, ce TSRMLS_CC );
    zend_hash_copy(i_obj->zo.properties, &ce->default_properties, (copy_ctor_func_t) zval_add_ref, (void *) &tmp, sizeof(zval *));

    retval.handle = zend_objects_store_put(i_obj, (zend_objects_store_dtor_t)zend_objects_destroy_object, (zend_objects_free_object_storage_t)php_zk_free_storage, NULL TSRMLS_CC);
    retval.handlers = zend_get_std_object_handlers();

    return retval;
}

static php_cb_data_t* php_cb_data_new(zend_fcall_info *fci, zend_fcall_info_cache *fcc, zend_bool oneshot TSRMLS_DC)
{
	php_cb_data_t *cbd = emalloc(sizeof(php_cb_data_t));
	cbd->fci = *fci;
	cbd->fcc = *fcc;
	cbd->oneshot = oneshot;
	zend_hash_next_index_insert(&ZK_G(callbacks), (void*)&cbd, sizeof(php_cb_data_t *), NULL);
	cbd->h = zend_hash_num_elements(&ZK_G(callbacks))-1;
	return cbd;
}

static void php_cb_data_destroy(php_cb_data_t **entry)
{
	php_cb_data_t *cbd = *(php_cb_data_t **)entry;
	efree(cbd);
}


static void php_zk_watcher_marshal(zhandle_t *zk, int type, int state, const char *path, void *context)
{
	TSRMLS_FETCH();

	zval **params[3];
	zval *retval;
	zval *z_type;
	zval *z_state;
	zval *z_path;
	php_cb_data_t *cb_data = (php_cb_data_t *)context;

	MAKE_STD_ZVAL(z_type);
	MAKE_STD_ZVAL(z_state);
	MAKE_STD_ZVAL(z_path);

	ZVAL_LONG(z_type, type);
	ZVAL_LONG(z_state, state);
	ZVAL_STRING(z_path, (char *)path, 1);

	params[0] = &z_type;
	params[1] = &z_state;
	params[2] = &z_path;

	cb_data->fci.retval_ptr_ptr = &retval;
	cb_data->fci.params = params;
	cb_data->fci.param_count = 3;

	if (zend_call_function(&cb_data->fci, &cb_data->fcc TSRMLS_CC) == SUCCESS) {
		zval_ptr_dtor(&retval);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not invoke watcher callback");
	}

	zval_ptr_dtor(&z_type);
	zval_ptr_dtor(&z_state);
	zval_ptr_dtor(&z_path);

	if (cb_data->oneshot) {
		zend_hash_index_del(&ZK_G(callbacks), cb_data->h);
	}
}

static void php_zk_completion_marshal(int rc, const void *context)
{
	TSRMLS_FETCH();

	zval **params[1];
	zval *retval;
	zval *z_rc;
	php_cb_data_t *cb_data = (php_cb_data_t *)context;

	MAKE_STD_ZVAL(z_rc);

	ZVAL_LONG(z_rc, rc);

	params[0] = &z_rc;

	cb_data->fci.retval_ptr_ptr = &retval;
	cb_data->fci.params = params;
	cb_data->fci.param_count = 1;

	if (zend_call_function(&cb_data->fci, &cb_data->fcc TSRMLS_CC) == SUCCESS) {
		zval_ptr_dtor(&retval);
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "could not invoke completion callback");
	}

	zval_ptr_dtor(&z_rc);

	if (cb_data->oneshot) {
		zend_hash_index_del(&ZK_G(callbacks), cb_data->h);
	}
}

static void php_parse_acl_list(zval *z_acl, struct ACL_vector *aclv)
{
	zval **entry;
	zval **perms, **scheme, **id;
	int size = 0;
	int i = 0;

	if (!z_acl || (size = zend_hash_num_elements(Z_ARRVAL_P(z_acl))) == 0) {
		return;
	}

	aclv->data = (struct ACL *)calloc(size, sizeof(struct ACL));

	for (zend_hash_internal_pointer_reset(Z_ARRVAL_P(z_acl));
		 zend_hash_get_current_data(Z_ARRVAL_P(z_acl), (void**)&entry) == SUCCESS;
		 zend_hash_move_forward(Z_ARRVAL_P(z_acl))) {

		if (Z_TYPE_PP(entry) != IS_ARRAY) {
			continue;
		}

		perms = scheme = id = NULL;
		zend_hash_find(Z_ARRVAL_PP(entry), ZEND_STRS("perms"), (void**)&perms);
		zend_hash_find(Z_ARRVAL_PP(entry), ZEND_STRS("scheme"), (void**)&scheme);
		zend_hash_find(Z_ARRVAL_PP(entry), ZEND_STRS("id"), (void**)&id);
		if (perms == NULL || scheme == NULL || id == NULL) {
			continue;
		}

		convert_to_long_ex(perms);
		convert_to_string_ex(scheme);
		convert_to_string_ex(id);

		aclv->data[i].perms = (int32_t)Z_LVAL_PP(perms);
		aclv->data[i].id.id = strdup(Z_STRVAL_PP(id));
		aclv->data[i].id.scheme = strdup(Z_STRVAL_PP(scheme));

		i++;
	}

	aclv->count = i;
}

static void php_aclv_destroy(struct ACL_vector *aclv)
{
	int i;
	for (i=0;i<aclv->count;++i)
	{
		free(aclv->data[i].id.id);
		free(aclv->data[i].id.scheme);
	}
	free(aclv->data);
}

static void php_stat_to_array(const struct Stat *stat, zval *array)
{
	array_init(array);

	add_assoc_double_ex(array, ZEND_STRS("czxid"), stat->czxid);
	add_assoc_double_ex(array, ZEND_STRS("mzxid"), stat->mzxid);
	add_assoc_double_ex(array, ZEND_STRS("ctime"), stat->ctime);
	add_assoc_double_ex(array, ZEND_STRS("mtime"), stat->mtime);
	add_assoc_long_ex(array, ZEND_STRS("version"), stat->version);
	add_assoc_long_ex(array, ZEND_STRS("cversion"), stat->cversion);
	add_assoc_long_ex(array, ZEND_STRS("aversion"), stat->aversion);
	add_assoc_double_ex(array, ZEND_STRS("ephemeralOwner"), stat->ephemeralOwner);
	add_assoc_long_ex(array, ZEND_STRS("dataLength"), stat->dataLength);
	add_assoc_long_ex(array, ZEND_STRS("numChildren"), stat->numChildren);
	add_assoc_double_ex(array, ZEND_STRS("pzxid"), stat->pzxid);
}

static void php_aclv_to_array(const struct ACL_vector *aclv, zval *array)
{
	zval *entry;
	int i;

	array_init(array);
	for (i = 0; i < aclv->count; i++) {
		MAKE_STD_ZVAL(entry);
		array_init(entry);
		add_assoc_long_ex(entry, ZEND_STRS("perms"), aclv->data[i].perms);
		add_assoc_string_ex(entry, ZEND_STRS("scheme"), aclv->data[i].id.scheme, 1);
		add_assoc_string_ex(entry, ZEND_STRS("id"), aclv->data[i].id.id, 1);
		add_next_index_zval(array, entry);
	}
}
/* }}} */

/* {{{ internal API functions */

static void php_zk_init_globals(zend_php_zookeeper_globals *php_zookeeper_globals_p TSRMLS_DC)
{
	zend_hash_init_ex(&ZK_G(callbacks), 5, NULL, (dtor_func_t)php_cb_data_destroy, 1, 0);
}

static void php_zk_destroy_globals(zend_php_zookeeper_globals *php_zookeeper_globals_p TSRMLS_DC)
{
	zend_hash_destroy(&ZK_G(callbacks));
}

PHP_ZOOKEEPER_API
zend_class_entry *php_zk_get_ce(void)
{
	return zookeeper_ce;
}

/* }}} */

/* {{{ methods arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo___construct, 0, 0, 1)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, watcher_cb)
	ZEND_ARG_INFO(0, recv_timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_connect , 0, 0, 1)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, watcher_cb)
	ZEND_ARG_INFO(0, recv_timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_create, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_ARRAY_INFO(0, acl, 0)
	ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_delete, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, version)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_getChildren, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, watcher_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_get, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, watcher_cb)
	ZEND_ARG_INFO(1, stat_info)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_exists, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, watcher_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_set, 0, 0, 1)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, value)
	ZEND_ARG_INFO(0, version)
	ZEND_ARG_INFO(1, stat_info)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getClientId, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getAcl, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setAcl, 0)
	ZEND_ARG_INFO(0, path)
	ZEND_ARG_INFO(0, version)
	ZEND_ARG_INFO(0, acl)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getState, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_getRecvTimeout, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_isRecoverable, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setDebugLevel, 0)
	ZEND_ARG_INFO(0, level)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setDeterministicConnOrder, 0)
	ZEND_ARG_INFO(0, trueOrFalse)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_addAuth, 0, 0, 2)
	ZEND_ARG_INFO(0, scheme)
	ZEND_ARG_INFO(0, cert)
	ZEND_ARG_INFO(0, completion_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setWatcher, 0)
	ZEND_ARG_INFO(0, watcher_cb)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_setLogStream, 0)
	ZEND_ARG_INFO(0, stream)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ zookeeper_class_methods */
#define ZK_ME(name, args) PHP_ME(Zookeeper, name, args, ZEND_ACC_PUBLIC)
#define ZK_ME_STATIC(name, args) PHP_ME(Zookeeper, name, args, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
static zend_function_entry zookeeper_class_methods[] = {
    ZK_ME(__construct,        arginfo___construct)
    ZK_ME(connect,            arginfo_connect )

	ZK_ME(create,             arginfo_create)
	ZK_ME(delete,             arginfo_delete)
	ZK_ME(get,                arginfo_get)
	ZK_ME(getChildren,        arginfo_getChildren)
	ZK_ME(exists,             arginfo_exists)
	ZK_ME(set,                arginfo_set)

	ZK_ME(getAcl,             arginfo_getAcl)
	ZK_ME(setAcl,             arginfo_setAcl)

	ZK_ME(getClientId,        arginfo_getClientId)
	ZK_ME(getState,           arginfo_getState)
	ZK_ME(getRecvTimeout,     arginfo_getRecvTimeout)
	ZK_ME(isRecoverable,      arginfo_isRecoverable)

	ZK_ME_STATIC(setDebugLevel,      arginfo_setDebugLevel)
	ZK_ME_STATIC(setDeterministicConnOrder, arginfo_setDeterministicConnOrder)

	ZK_ME(addAuth,            arginfo_addAuth)

	ZK_ME(setWatcher,         arginfo_setWatcher)
	ZK_ME(setLogStream,       arginfo_setLogStream)

    { NULL, NULL, NULL }
};
#undef ZK_ME
/* }}} */

/* {{{ zookeeper_module_entry
 */

zend_module_entry zookeeper_module_entry = {
#if ZEND_MODULE_API_NO >= 20050922
    STANDARD_MODULE_HEADER_EX,
	NULL,
	NULL,
#else
    STANDARD_MODULE_HEADER,
#endif
	"zookeeper",
	NULL,
	PHP_MINIT(zookeeper),
	PHP_MSHUTDOWN(zookeeper),
	NULL,
	PHP_RSHUTDOWN(zookeeper),
	PHP_MINFO(zookeeper),
	PHP_ZOOKEEPER_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

/* {{{ php_zk_register_constants */
static void php_zk_register_constants(INIT_FUNC_ARGS)
{
	#define ZK_CLASS_CONST_LONG(name) zend_declare_class_constant_long(php_zk_get_ce() , ZEND_STRS( #name ) - 1, ZOO_##name TSRMLS_CC)
	#define ZK_CLASS_CONST_LONG2(name) zend_declare_class_constant_long(php_zk_get_ce() , ZEND_STRS( #name ) - 1, Z##name TSRMLS_CC)

	ZK_CLASS_CONST_LONG(PERM_READ);
	ZK_CLASS_CONST_LONG(PERM_WRITE);
	ZK_CLASS_CONST_LONG(PERM_CREATE);
	ZK_CLASS_CONST_LONG(PERM_DELETE);
	ZK_CLASS_CONST_LONG(PERM_ALL);
	ZK_CLASS_CONST_LONG(PERM_ADMIN);

	ZK_CLASS_CONST_LONG(EPHEMERAL);
	ZK_CLASS_CONST_LONG(SEQUENCE);

	ZK_CLASS_CONST_LONG(EXPIRED_SESSION_STATE);
	ZK_CLASS_CONST_LONG(AUTH_FAILED_STATE);
	ZK_CLASS_CONST_LONG(CONNECTING_STATE);
	ZK_CLASS_CONST_LONG(ASSOCIATING_STATE);
	ZK_CLASS_CONST_LONG(CONNECTED_STATE);

	ZK_CLASS_CONST_LONG(CREATED_EVENT);
	ZK_CLASS_CONST_LONG(DELETED_EVENT);
	ZK_CLASS_CONST_LONG(CHANGED_EVENT);
	ZK_CLASS_CONST_LONG(CHILD_EVENT);
	ZK_CLASS_CONST_LONG(SESSION_EVENT);
	ZK_CLASS_CONST_LONG(NOTWATCHING_EVENT);

	ZK_CLASS_CONST_LONG(LOG_LEVEL_ERROR);
	ZK_CLASS_CONST_LONG(LOG_LEVEL_WARN);
	ZK_CLASS_CONST_LONG(LOG_LEVEL_INFO);
	ZK_CLASS_CONST_LONG(LOG_LEVEL_DEBUG);

	ZK_CLASS_CONST_LONG2(SYSTEMERROR);
	ZK_CLASS_CONST_LONG2(RUNTIMEINCONSISTENCY);
	ZK_CLASS_CONST_LONG2(DATAINCONSISTENCY);
	ZK_CLASS_CONST_LONG2(CONNECTIONLOSS);
	ZK_CLASS_CONST_LONG2(MARSHALLINGERROR);
	ZK_CLASS_CONST_LONG2(UNIMPLEMENTED);
	ZK_CLASS_CONST_LONG2(OPERATIONTIMEOUT);
	ZK_CLASS_CONST_LONG2(BADARGUMENTS);
	ZK_CLASS_CONST_LONG2(INVALIDSTATE);

	ZK_CLASS_CONST_LONG2(OK);
	ZK_CLASS_CONST_LONG2(APIERROR);
	ZK_CLASS_CONST_LONG2(NONODE);
	ZK_CLASS_CONST_LONG2(NOAUTH);
	ZK_CLASS_CONST_LONG2(BADVERSION);
	ZK_CLASS_CONST_LONG2(NOCHILDRENFOREPHEMERALS);
	ZK_CLASS_CONST_LONG2(NODEEXISTS);
	ZK_CLASS_CONST_LONG2(NOTEMPTY);
	ZK_CLASS_CONST_LONG2(SESSIONEXPIRED);
	ZK_CLASS_CONST_LONG2(INVALIDCALLBACK);
	ZK_CLASS_CONST_LONG2(INVALIDACL);
	ZK_CLASS_CONST_LONG2(AUTHFAILED);
	ZK_CLASS_CONST_LONG2(CLOSING);
	ZK_CLASS_CONST_LONG2(NOTHING);

	#undef  ZK_CLASS_CONST_LONG
	#undef  ZK_CLASS_CONST_LONG2
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(zookeeper)
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Zookeeper", zookeeper_class_methods);
	zookeeper_ce = zend_register_internal_class(&ce TSRMLS_CC);
	zookeeper_ce->create_object = php_zk_new;

	/* set debug level to warning by default */
	zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);

	php_zk_register_constants(INIT_FUNC_ARGS_PASSTHRU);

#ifdef ZTS
	ts_allocate_id(&php_zookeeper_globals_id, sizeof(zend_php_zookeeper_globals),
				   (ts_allocate_ctor) php_zk_init_globals, (ts_allocate_dtor) php_zk_destroy_globals);
#else
	php_zk_init_globals(&php_zookeeper_globals TSRMLS_CC);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(zookeeper)
{
#ifdef ZTS
    ts_free_id(php_zookeeper_globals_id);
#else
    php_zk_destroy_globals(&php_zookeeper_globals TSRMLS_CC);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RSHUTDOWN_FUNCTION */
PHP_RSHUTDOWN_FUNCTION(zookeeper)
{
	zend_hash_clean(&ZK_G(callbacks));

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(zookeeper)
{
	char buf[32];

	php_info_print_table_start();

	php_info_print_table_header(2, "zookeeper support", "enabled");
	php_info_print_table_row(2, "version", PHP_ZOOKEEPER_VERSION);

	snprintf(buf, sizeof(buf), "%ld.%ld.%ld", ZOO_MAJOR_VERSION, ZOO_MINOR_VERSION, ZOO_PATCH_VERSION);
	php_info_print_table_row(2, "libzookeeper version", buf);

	php_info_print_table_end();
}
/* }}} */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim: noet sw=4 ts=4 fdm=marker:
 */
