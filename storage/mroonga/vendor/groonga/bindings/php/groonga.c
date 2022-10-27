
#include "php_groonga.h"

#if HAVE_GROONGA

int le_grn_ctx;
void grn_ctx_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
  grn_ctx *ctx = (grn_ctx *)(rsrc->ptr);
  grn_ctx_close(ctx);
}

zend_function_entry groonga_functions[] = {
  PHP_FE(grn_ctx_init        , grn_ctx_init_arg_info)
  PHP_FE(grn_ctx_close       , grn_ctx_close_arg_info)
  PHP_FE(grn_ctx_connect      , grn_ctx_connect_arg_info)
  PHP_FE(grn_ctx_send         , grn_ctx_send_arg_info)
  PHP_FE(grn_ctx_recv         , grn_ctx_recv_arg_info)
  { NULL, NULL, NULL }
};


zend_module_entry groonga_module_entry = {
  STANDARD_MODULE_HEADER,
  "groonga",
  groonga_functions,
  PHP_MINIT(groonga),
  PHP_MSHUTDOWN(groonga),
  PHP_RINIT(groonga),
  PHP_RSHUTDOWN(groonga),
  PHP_MINFO(groonga),
  "0.1",
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_GROONGA
ZEND_GET_MODULE(groonga)
#endif


PHP_MINIT_FUNCTION(groonga)
{
  REGISTER_LONG_CONSTANT("GRN_CTX_USE_QL", GRN_CTX_USE_QL, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_BATCH_MODE", GRN_CTX_BATCH_MODE, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_DEFAULT", GRN_ENC_DEFAULT, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_NONE", GRN_ENC_NONE, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_EUC_JP", GRN_ENC_EUC_JP, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_UTF8", GRN_ENC_UTF8, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_SJIS", GRN_ENC_SJIS, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_LATIN1", GRN_ENC_LATIN1, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_ENC_KOI8R", GRN_ENC_KOI8R, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_MORE", GRN_CTX_MORE, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_TAIL", GRN_CTX_TAIL, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_HEAD", GRN_CTX_HEAD, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_QUIET", GRN_CTX_QUIET, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_QUIT", GRN_CTX_QUIT, CONST_PERSISTENT | CONST_CS);
  REGISTER_LONG_CONSTANT("GRN_CTX_FIN", GRN_CTX_FIN, CONST_PERSISTENT | CONST_CS);
  le_grn_ctx = zend_register_list_destructors_ex(
               grn_ctx_dtor, NULL, "grn_ctx", module_number);

  grn_init();

  return SUCCESS;
}


PHP_MSHUTDOWN_FUNCTION(groonga)
{
  grn_fin();
  return SUCCESS;
}


PHP_RINIT_FUNCTION(groonga)
{
  return SUCCESS;
}


PHP_RSHUTDOWN_FUNCTION(groonga)
{
  return SUCCESS;
}


PHP_MINFO_FUNCTION(groonga)
{
  php_info_print_box_start(0);
  php_printf("<p>Groonga</p>\n");
  php_printf("<p>Version 0.2 (ctx)</p>\n");
  php_printf("<p><b>Authors:</b></p>\n");
  php_printf("<p>yu &lt;yu@irx.jp&gt; (lead)</p>\n");
  php_info_print_box_end();
}


PHP_FUNCTION(grn_ctx_init)
{
  grn_ctx *ctx = (grn_ctx *) malloc(sizeof(grn_ctx));
  long res_id = -1;
  long flags = 0;
  grn_rc rc;

  if (ctx == NULL) {
    RETURN_FALSE; // unable to allocate memory for ctx
  }

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &flags) == FAILURE) {
    return;
  }

  if ((rc = grn_ctx_init(ctx, flags)) != GRN_SUCCESS) {
    RETURN_FALSE;
  }

  res_id = ZEND_REGISTER_RESOURCE(return_value, ctx, le_grn_ctx);
  RETURN_RESOURCE(res_id);
}


PHP_FUNCTION(grn_ctx_close)
{
  zval *res = NULL;
  int res_id = -1;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &res) == FAILURE) {
    return;
  }

  zend_list_delete(Z_LVAL_P(res)); // call grn_ctx_dtor
  RETURN_TRUE;
}


PHP_FUNCTION(grn_ctx_connect)
{
  zval *res = NULL;
  int res_id = -1;

  grn_rc rc;
  grn_ctx *ctx;
  char  *host = "localhost";
  int host_len = 0;
  long port = 10041;
  long flags = 0;


  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|ll", &res, &host, &host_len, &port, &flags) == FAILURE) {
    return;
  }

  ZEND_FETCH_RESOURCE(ctx, grn_ctx *, &res, res_id, "grn_ctx", le_grn_ctx);

  if ((rc = grn_ctx_connect(ctx, host, port, flags)) != GRN_SUCCESS) {
    RETURN_FALSE;
  }

  RETURN_TRUE;
}


PHP_FUNCTION(grn_ctx_send)
{
  zval *res = NULL;
  int res_id = -1;

  grn_ctx *ctx;
  char *query = NULL;
  unsigned int query_len, qid;
  long flags = 0;


  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs|l", &res, &query, &query_len, &flags) == FAILURE) {
    return;
  }

  ZEND_FETCH_RESOURCE(ctx, grn_ctx *, &res, res_id, "grn_ctx", le_grn_ctx);
  qid = grn_ctx_send(ctx, query, query_len, flags);
  if (ctx->rc != GRN_SUCCESS) {
    RETURN_FALSE;
  }

  RETURN_LONG(qid)

}


PHP_FUNCTION(grn_ctx_recv)
{
  zval *res,*ret = NULL;
  int res_id = -1;
  grn_ctx *ctx;

  char *str;
  int flags;
  unsigned int str_len, qid;


  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &res) == FAILURE) {
    return;
  }

  ZEND_FETCH_RESOURCE(ctx, grn_ctx *, &res, res_id, "grn_ctx", le_grn_ctx);

  qid = grn_ctx_recv(ctx, &str, &str_len, &flags);

  if (ctx->rc != GRN_SUCCESS) {
    RETURN_FALSE;
  }

  MAKE_STD_ZVAL(ret);
  array_init(ret);
  array_init(return_value);

  add_next_index_long(ret, flags);
  add_next_index_stringl(ret, str, str_len, 1);

  add_index_zval(return_value, qid, ret);
}

#endif /* HAVE_GROONGA */
