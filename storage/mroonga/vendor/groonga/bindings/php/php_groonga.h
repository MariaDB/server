/*
   +----------------------------------------------------------------------+
   | unknown license:                                                     |
   +----------------------------------------------------------------------+
   | Authors: yu <yu@irx.jp>                                              |
   +----------------------------------------------------------------------+
*/

#ifndef PHP_GROONGA_H
#define PHP_GROONGA_H

#ifdef  __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <php.h>

#ifdef HAVE_GROONGA

#include <php_ini.h>
#include <SAPI.h>
#include <ext/standard/info.h>
#include <Zend/zend_extensions.h>
#ifdef  __cplusplus
} // extern "C" 
#endif
#include <groonga.h>
#ifdef  __cplusplus
extern "C" {
#endif

extern zend_module_entry groonga_module_entry;
#define phpext_groonga_ptr &groonga_module_entry

#ifdef PHP_WIN32
#define PHP_GROONGA_API __declspec(dllexport)
#else
#define PHP_GROONGA_API
#endif

PHP_MINIT_FUNCTION(groonga);
PHP_MSHUTDOWN_FUNCTION(groonga);
PHP_RINIT_FUNCTION(groonga);
PHP_RSHUTDOWN_FUNCTION(groonga);
PHP_MINFO_FUNCTION(groonga);

#ifdef ZTS
#include "TSRM.h"
#endif

#define FREE_RESOURCE(resource) zend_list_delete(Z_LVAL_P(resource))

#define PROP_GET_LONG(name)    Z_LVAL_P(zend_read_property(_this_ce, _this_zval, #name, strlen(#name), 1 TSRMLS_CC))
#define PROP_SET_LONG(name, l) zend_update_property_long(_this_ce, _this_zval, #name, strlen(#name), l TSRMLS_CC)

#define PROP_GET_DOUBLE(name)    Z_DVAL_P(zend_read_property(_this_ce, _this_zval, #name, strlen(#name), 1 TSRMLS_CC))
#define PROP_SET_DOUBLE(name, d) zend_update_property_double(_this_ce, _this_zval, #name, strlen(#name), d TSRMLS_CC)

#define PROP_GET_STRING(name)    Z_STRVAL_P(zend_read_property(_this_ce, _this_zval, #name, strlen(#name), 1 TSRMLS_CC))
#define PROP_GET_STRLEN(name)    Z_STRLEN_P(zend_read_property(_this_ce, _this_zval, #name, strlen(#name), 1 TSRMLS_CC))
#define PROP_SET_STRING(name, s) zend_update_property_string(_this_ce, _this_zval, #name, strlen(#name), s TSRMLS_CC)
#define PROP_SET_STRINGL(name, s, l) zend_update_property_stringl(_this_ce, _this_zval, #name, strlen(#name), s, l TSRMLS_CC)


PHP_FUNCTION(grn_ctx_init);
#if (PHP_MAJOR_VERSION >= 5)
ZEND_BEGIN_ARG_INFO_EX(grn_ctx_init_arg_info, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
  ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()
#else /* PHP 4.x */
#define grn_ctx_init_arg_info NULL
#endif

PHP_FUNCTION(grn_ctx_close);
#if (PHP_MAJOR_VERSION >= 5)
ZEND_BEGIN_ARG_INFO_EX(grn_ctx_close_arg_info, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
  ZEND_ARG_INFO(0, res)
ZEND_END_ARG_INFO()
#else /* PHP 4.x */
#define grn_ctx_close_arg_info NULL
#endif

PHP_FUNCTION(grn_ctx_connect);
#if (PHP_MAJOR_VERSION >= 5)
ZEND_BEGIN_ARG_INFO_EX(grn_ctx_connect_arg_info, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 4)
  ZEND_ARG_INFO(0, res)
  ZEND_ARG_INFO(0, host)
  ZEND_ARG_INFO(0, port)
  ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()
#else /* PHP 4.x */
#define grn_ctx_connect_arg_info NULL
#endif

PHP_FUNCTION(grn_ctx_send);
#if (PHP_MAJOR_VERSION >= 5)
ZEND_BEGIN_ARG_INFO_EX(grn_ctx_send_arg_info, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 3)
  ZEND_ARG_INFO(0, res)
  ZEND_ARG_INFO(0, query)
  ZEND_ARG_INFO(0, flags)
ZEND_END_ARG_INFO()
#else /* PHP 4.x */
#define grn_ctx_send_arg_info NULL
#endif

PHP_FUNCTION(grn_ctx_recv);
#if (PHP_MAJOR_VERSION >= 5)
ZEND_BEGIN_ARG_INFO_EX(grn_ctx_recv_arg_info, ZEND_SEND_BY_VAL, ZEND_RETURN_VALUE, 1)
  ZEND_ARG_INFO(0, res)
ZEND_END_ARG_INFO()
#else /* PHP 4.x */
#define grn_ctx_recv_arg_info NULL
#endif

#ifdef  __cplusplus
} // extern "C" 
#endif

#endif /* PHP_HAVE_GROONGA */

#endif /* PHP_GROONGA_H */
