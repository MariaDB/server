/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef WIN32
# define NGX_GRN_SUPPORT_STOP_BY_COMMAND
#endif

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <groonga.h>

#include <sys/stat.h>

#ifdef NGX_GRN_SUPPORT_STOP_BY_COMMAND
# include <sys/types.h>
# include <unistd.h>
#endif

#define GRN_NO_FLAGS 0

typedef struct {
  ngx_flag_t enabled;
  ngx_str_t database_path;
  char *database_path_cstr;
  ngx_flag_t database_auto_create;
  ngx_str_t base_path;
  ngx_str_t log_path;
  ngx_open_file_t *log_file;
  grn_log_level log_level;
  ngx_str_t query_log_path;
  ngx_open_file_t *query_log_file;
  size_t cache_limit;
  char *config_file;
  int config_line;
  char *name;
  grn_ctx context;
  grn_cache *cache;
} ngx_http_groonga_loc_conf_t;

typedef struct {
  ngx_log_t *log;
  ngx_pool_t *pool;
  ngx_int_t rc;
} ngx_http_groonga_database_callback_data_t;

typedef struct {
  grn_bool initialized;
  grn_ctx context;
  struct {
    grn_bool processed;
    grn_bool header_sent;
    ngx_http_request_t *r;
    ngx_int_t rc;
    ngx_chain_t *free_chain;
    ngx_chain_t *busy_chain;
  } raw;
  struct {
    grn_obj head;
    grn_obj body;
    grn_obj foot;
  } typed;
} ngx_http_groonga_handler_data_t;

typedef struct {
  ngx_pool_t *pool;
  ngx_open_file_t *file;
} ngx_http_groonga_logger_data_t;

typedef struct {
  ngx_pool_t *pool;
  ngx_open_file_t *file;
  ngx_str_t *path;
} ngx_http_groonga_query_logger_data_t;

typedef void (*ngx_http_groonga_loc_conf_callback_pt)(ngx_http_groonga_loc_conf_t *conf, void *user_data);

ngx_module_t ngx_http_groonga_module;

static char *
ngx_str_null_terminate(ngx_pool_t *pool, const ngx_str_t *string)
{
  char *null_terminated_c_string;

  null_terminated_c_string = ngx_pnalloc(pool, string->len + 1);
  if (!null_terminated_c_string) {
    return NULL;
  }

  memcpy(null_terminated_c_string, string->data, string->len);
  null_terminated_c_string[string->len] = '\0';

  return null_terminated_c_string;
}

static grn_bool
ngx_str_equal_c_string(ngx_str_t *string, const char *c_string)
{
  if (string->len != strlen(c_string)) {
    return GRN_FALSE;
  }

  return memcmp(c_string, string->data, string->len) == 0;
}

static grn_bool
ngx_str_is_custom_path(ngx_str_t *string)
{
  if (string->len == 0) {
    return GRN_FALSE;
  }

  if (strncmp((const char *)(string->data), "off", string->len) == 0) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static void
ngx_http_groonga_logger_log(grn_ctx *ctx, grn_log_level level,
                            const char *timestamp, const char *title,
                            const char *message, const char *location,
                            void *user_data)
{
  ngx_http_groonga_logger_data_t *logger_data = user_data;
  const char level_marks[] = " EACewnid-";
  u_char buffer[NGX_MAX_ERROR_STR];
  u_char *last;
  size_t prefix_size;
  size_t message_size;
  size_t location_size;
  size_t postfix_size;
  size_t log_message_size;

#define LOG_PREFIX_FORMAT "%s|%c|%s "
  prefix_size =
    strlen(timestamp) +
    1 /* | */ +
    1 /* %c */ +
    1 /* | */ +
    strlen(title) +
    1 /* a space */;
  message_size = strlen(message);
  if (location && *location) {
    location_size = 1 /* a space */ + strlen(location);
  } else {
    location_size = 0;
  }
  postfix_size = 1 /* \n */;
  log_message_size = prefix_size + message_size + location_size + postfix_size;

  if (log_message_size > NGX_MAX_ERROR_STR) {
    last = ngx_slprintf(buffer, buffer + NGX_MAX_ERROR_STR,
                        LOG_PREFIX_FORMAT,
                        timestamp, *(level_marks + level), title);
    ngx_write_fd(logger_data->file->fd, buffer, last - buffer);
    ngx_write_fd(logger_data->file->fd, (void *)message, message_size);
    if (location_size > 0) {
      ngx_write_fd(logger_data->file->fd, " ", 1);
      ngx_write_fd(logger_data->file->fd, (void *)location, location_size);
    }
    ngx_write_fd(logger_data->file->fd, "\n", 1);
  } else {
    if (location && *location) {
      last = ngx_slprintf(buffer, buffer + NGX_MAX_ERROR_STR,
                          LOG_PREFIX_FORMAT " %s %s\n",
                          timestamp, *(level_marks + level), title, message,
                          location);
    } else {
      last = ngx_slprintf(buffer, buffer + NGX_MAX_ERROR_STR,
                          LOG_PREFIX_FORMAT " %s\n",
                          timestamp, *(level_marks + level), title, message);
    }
    ngx_write_fd(logger_data->file->fd, buffer, last - buffer);
  }
#undef LOG_PREFIX_FORMAT
}

static void
ngx_http_groonga_logger_reopen(grn_ctx *ctx, void *user_data)
{
  GRN_LOG(ctx, GRN_LOG_NOTICE, "log will be closed.");
  ngx_reopen_files((ngx_cycle_t *)ngx_cycle, -1);
  GRN_LOG(ctx, GRN_LOG_NOTICE, "log opened.");
}

static void
ngx_http_groonga_logger_fin(grn_ctx *ctx, void *user_data)
{
  ngx_http_groonga_logger_data_t *logger_data = user_data;

  ngx_pfree(logger_data->pool, logger_data);
}

static grn_logger ngx_http_groonga_logger = {
  GRN_LOG_DEFAULT_LEVEL,
  GRN_LOG_TIME | GRN_LOG_MESSAGE,
  NULL,
  ngx_http_groonga_logger_log,
  ngx_http_groonga_logger_reopen,
  ngx_http_groonga_logger_fin
};

static ngx_int_t
ngx_http_groonga_context_init_logger(grn_ctx *context,
                                     ngx_http_groonga_loc_conf_t *location_conf,
                                     ngx_pool_t *pool,
                                     ngx_log_t *log)
{
  ngx_http_groonga_logger_data_t *logger_data;

  if (!location_conf->log_file) {
    return NGX_OK;
  }

  logger_data = ngx_pcalloc(pool, sizeof(ngx_http_groonga_logger_data_t));
  if (!logger_data) {
    ngx_log_error(NGX_LOG_ERR, log, 0,
                  "http_groonga: failed to allocate memory for logger");
    return NGX_ERROR;
  }

  logger_data->pool = pool;
  logger_data->file = location_conf->log_file;
  ngx_http_groonga_logger.max_level = location_conf->log_level;
  ngx_http_groonga_logger.user_data = logger_data;
  grn_logger_set(context, &ngx_http_groonga_logger);

  return NGX_OK;
}

static void
ngx_http_groonga_query_logger_log(grn_ctx *ctx, unsigned int flag,
                                  const char *timestamp, const char *info,
                                  const char *message, void *user_data)
{
  ngx_http_groonga_query_logger_data_t *data = user_data;
  u_char buffer[NGX_MAX_ERROR_STR];
  u_char *last;

  last = ngx_slprintf(buffer, buffer + NGX_MAX_ERROR_STR,
                      "%s|%s%s\n",
                      timestamp, info, message);
  ngx_write_fd(data->file->fd, buffer, last - buffer);
}

static void
ngx_http_groonga_query_logger_reopen(grn_ctx *ctx, void *user_data)
{
  ngx_http_groonga_query_logger_data_t *data = user_data;

  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_DESTINATION, " ",
                "query log will be closed: <%.*s>",
                (int)(data->path->len), data->path->data);
  ngx_reopen_files((ngx_cycle_t *)ngx_cycle, -1);
  GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_DESTINATION, " ",
                "query log is opened: <%.*s>",
                (int)(data->path->len), data->path->data);
}

static void
ngx_http_groonga_query_logger_fin(grn_ctx *ctx, void *user_data)
{
  ngx_http_groonga_query_logger_data_t *data = user_data;

  ngx_pfree(data->pool, data);
}

static grn_query_logger ngx_http_groonga_query_logger = {
  GRN_QUERY_LOG_DEFAULT,
  NULL,
  ngx_http_groonga_query_logger_log,
  ngx_http_groonga_query_logger_reopen,
  ngx_http_groonga_query_logger_fin
};

static ngx_int_t
ngx_http_groonga_context_init_query_logger(grn_ctx *context,
                                           ngx_http_groonga_loc_conf_t *location_conf,
                                           ngx_pool_t *pool,
                                           ngx_log_t *log)
{
  ngx_http_groonga_query_logger_data_t *query_logger_data;

  if (!location_conf->query_log_file) {
    return NGX_OK;
  }

  query_logger_data = ngx_pcalloc(pool,
                                  sizeof(ngx_http_groonga_query_logger_data_t));
  if (!query_logger_data) {
    ngx_log_error(NGX_LOG_ERR, log, 0,
                  "http_groonga: failed to allocate memory for query logger");
    return NGX_ERROR;
  }

  query_logger_data->pool = pool;
  query_logger_data->file = location_conf->query_log_file;
  query_logger_data->path = &(location_conf->query_log_path);
  ngx_http_groonga_query_logger.user_data = query_logger_data;
  grn_query_logger_set(context, &ngx_http_groonga_query_logger);

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_context_init(grn_ctx *context,
                              ngx_http_groonga_loc_conf_t *location_conf,
                              ngx_pool_t *pool,
                              ngx_log_t *log)
{
  ngx_int_t status;

  grn_ctx_init(context, GRN_NO_FLAGS);

  status = ngx_http_groonga_context_init_logger(context,
                                                location_conf,
                                                pool,
                                                log);
  if (status == NGX_ERROR) {
    grn_ctx_fin(context);
    return status;
  }

  status = ngx_http_groonga_context_init_query_logger(context,
                                                      location_conf,
                                                      pool,
                                                      log);
  if (status == NGX_ERROR) {
    grn_ctx_fin(context);
    return status;
  }

  if (location_conf->cache) {
    grn_cache_current_set(context, location_conf->cache);
  }

  return status;
}

static void
ngx_http_groonga_context_log_error(ngx_log_t *log, grn_ctx *context)
{
  if (context->rc == GRN_SUCCESS) {
    return;
  }

  ngx_log_error(NGX_LOG_ERR, log, 0, "%s", context->errbuf);
}

static ngx_int_t
ngx_http_groonga_context_check_error(ngx_log_t *log, grn_ctx *context)
{
  if (context->rc == GRN_SUCCESS) {
    return NGX_OK;
  } else {
    ngx_http_groonga_context_log_error(log, context);
    return NGX_HTTP_BAD_REQUEST;
  }
}

static ngx_buf_t *
ngx_http_groonga_grn_obj_to_ngx_buf(ngx_pool_t *pool, grn_obj *object)
{
  ngx_buf_t *buffer;
  buffer = ngx_pcalloc(pool, sizeof(ngx_buf_t));
  if (buffer == NULL) {
    return NULL;
  }

  /* adjust the pointers of the buffer */
  buffer->pos = (u_char *)GRN_TEXT_VALUE(object);
  buffer->last = (u_char *)GRN_TEXT_VALUE(object) + GRN_TEXT_LEN(object);
  buffer->memory = 1;    /* this buffer is in memory */
  buffer->in_file = 0;

  return buffer;
}

static void
ngx_http_groonga_handler_cleanup(void *user_data)
{
  ngx_http_groonga_handler_data_t *data = user_data;
  grn_ctx *context;

  if (!data->initialized) {
    return;
  }

  context = &(data->context);
  GRN_OBJ_FIN(context, &(data->typed.head));
  GRN_OBJ_FIN(context, &(data->typed.body));
  GRN_OBJ_FIN(context, &(data->typed.foot));
  grn_logger_set(context, NULL);
  grn_query_logger_set(context, NULL);
  grn_ctx_fin(context);
}

static void
ngx_http_groonga_handler_set_content_type(ngx_http_request_t *r,
                                          const char *content_type)
{
  r->headers_out.content_type.len = strlen(content_type);
  r->headers_out.content_type.data = (u_char *)content_type;
  r->headers_out.content_type_len = r->headers_out.content_type.len;
}

static void
ngx_http_groonga_context_receive_handler_raw(grn_ctx *context,
                                             int flags,
                                             ngx_http_groonga_handler_data_t *data)
{
  char *chunk = NULL;
  unsigned int chunk_size = 0;
  int recv_flags;
  ngx_http_request_t *r;
  ngx_log_t *log;
  grn_bool is_last_chunk;

  grn_ctx_recv(context, &chunk, &chunk_size, &recv_flags);
  data->raw.processed = GRN_TRUE;

  if (data->raw.rc != NGX_OK) {
    return;
  }

  r = data->raw.r;
  log = r->connection->log;
  is_last_chunk = (flags & GRN_CTX_TAIL);

  if (!data->raw.header_sent) {
    ngx_http_groonga_handler_set_content_type(r, grn_ctx_get_mime_type(context));
    r->headers_out.status = NGX_HTTP_OK;
    if (is_last_chunk) {
      r->headers_out.content_length_n = chunk_size;
      if (chunk_size == 0) {
        r->header_only = 1;
      }
    } else {
      r->headers_out.content_length_n = -1;
    }
    data->raw.rc = ngx_http_send_header(r);
    data->raw.header_sent = GRN_TRUE;

    if (data->raw.rc != NGX_OK) {
      return;
    }
  }

  if (chunk_size > 0 || is_last_chunk) {
    ngx_chain_t *chain;

    chain = ngx_chain_get_free_buf(r->pool, &(data->raw.free_chain));
    if (!chain) {
      ngx_log_error(NGX_LOG_ERR, log, 0,
                    "http_groonga: failed to allocate memory for chunked body");
      data->raw.rc = NGX_ERROR;
      return;
    }
    if (chunk_size == 0) {
      chain->buf->pos = NULL;
      chain->buf->last = NULL;
      chain->buf->memory = 0;
    } else {
      chain->buf->pos = (u_char *)chunk;
      chain->buf->last = (u_char *)chunk + chunk_size;
      chain->buf->memory = 1;
    }
    chain->buf->tag = (ngx_buf_tag_t)&ngx_http_groonga_module;
    chain->buf->flush = 1;
    chain->buf->temporary = 0;
    chain->buf->in_file = 0;
    if (is_last_chunk) {
      chain->buf->last_buf = 1;
    } else {
      chain->buf->last_buf = 0;
    }
    chain->next = NULL;

    data->raw.rc = ngx_http_output_filter(r, chain);
    ngx_chain_update_chains(r->pool,
                            &(data->raw.free_chain),
                            &(data->raw.busy_chain),
                            &chain,
                            (ngx_buf_tag_t)&ngx_http_groonga_module);
  }
}

static void
ngx_http_groonga_context_receive_handler_typed(grn_ctx *context,
                                               int flags,
                                               ngx_http_groonga_handler_data_t *data)
{
  char *result = NULL;
  unsigned int result_size = 0;
  int recv_flags;

  if (!(flags & GRN_CTX_TAIL)) {
    return;
  }

  grn_ctx_recv(context, &result, &result_size, &recv_flags);

#ifdef NGX_GRN_SUPPORT_STOP_BY_COMMAND
  if (recv_flags == GRN_CTX_QUIT) {
    ngx_int_t ngx_rc;
    ngx_int_t ngx_pid;

    if (ngx_process == NGX_PROCESS_SINGLE) {
      ngx_pid = getpid();
    } else {
      ngx_pid = getppid();
    }

    ngx_rc = ngx_os_signal_process((ngx_cycle_t*)ngx_cycle,
                                   "stop",
                                   ngx_pid);
    if (ngx_rc == NGX_OK) {
      context->stat &= ~GRN_CTX_QUIT;
      grn_ctx_recv(context, &result, &result_size, &recv_flags);
      context->stat |= GRN_CTX_QUIT;
    } else {
      context->rc = GRN_OPERATION_NOT_PERMITTED;
      GRN_TEXT_PUTS(context, &(data->typed.body), "false");
      context->stat &= ~GRN_CTX_QUIT;
    }
  }
#endif

  if (result_size > 0 ||
      GRN_TEXT_LEN(&(data->typed.body)) > 0 ||
      context->rc != GRN_SUCCESS) {
    if (result_size > 0) {
      GRN_TEXT_PUT(context, &(data->typed.body), result, result_size);
    }

    grn_output_envelope(context,
                        context->rc,
                        &(data->typed.head),
                        &(data->typed.body),
                        &(data->typed.foot),
                        NULL,
                        0);
  }
}

static void
ngx_http_groonga_context_receive_handler(grn_ctx *context,
                                         int flags,
                                         void *callback_data)
{
  ngx_http_groonga_handler_data_t *data = callback_data;

  if (grn_ctx_get_output_type(context) == GRN_CONTENT_NONE) {
    ngx_http_groonga_context_receive_handler_raw(context, flags, data);
  } else {
    ngx_http_groonga_context_receive_handler_typed(context, flags, data);
  }
}

static ngx_int_t
ngx_http_groonga_extract_command_path(ngx_http_request_t *r,
                                      ngx_str_t *command_path)
{
  size_t base_path_length;

  ngx_http_core_loc_conf_t *http_location_conf;
  ngx_http_groonga_loc_conf_t *location_conf;

  http_location_conf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
  location_conf = ngx_http_get_module_loc_conf(r, ngx_http_groonga_module);

  command_path->data = r->unparsed_uri.data;
  command_path->len = r->unparsed_uri.len;
  base_path_length = http_location_conf->name.len;
  if (location_conf->base_path.len > 0) {
    if (command_path->len < location_conf->base_path.len) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "requested URI is shorter than groonga_base_path: "
                    "URI: <%V>, groonga_base_path: <%V>",
                    &(r->unparsed_uri), &(location_conf->base_path));
    } else if (strncmp((const char *)command_path->data,
                       (const char *)(location_conf->base_path.data),
                       location_conf->base_path.len) < 0) {
      ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                    "groonga_base_path doesn't match requested URI: "
                    "URI: <%V>, groonga_base_path: <%V>",
                    &(r->unparsed_uri), &(location_conf->base_path));
    } else {
      base_path_length = location_conf->base_path.len;
    }
  }
  command_path->data += base_path_length;
  command_path->len -= base_path_length;
  if (command_path->len > 0 && command_path->data[0] == '/') {
    command_path->data += 1;
    command_path->len -= 1;
  }
  if (command_path->len == 0) {
    return NGX_HTTP_BAD_REQUEST;
  }

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_handler_create_data(ngx_http_request_t *r,
                                     ngx_http_groonga_handler_data_t **data_return)
{
  ngx_int_t rc;

  ngx_http_groonga_loc_conf_t *location_conf;

  ngx_http_cleanup_t *cleanup;
  ngx_http_groonga_handler_data_t *data;

  grn_ctx *context;

  location_conf = ngx_http_get_module_loc_conf(r, ngx_http_groonga_module);

  cleanup = ngx_http_cleanup_add(r, sizeof(ngx_http_groonga_handler_data_t));
  cleanup->handler = ngx_http_groonga_handler_cleanup;
  data = cleanup->data;
  *data_return = data;

  context = &(data->context);
  rc = ngx_http_groonga_context_init(context, location_conf,
                                     r->pool, r->connection->log);
  if (rc != NGX_OK) {
    return rc;
  }

  data->initialized = GRN_TRUE;

  data->raw.processed = GRN_FALSE;
  data->raw.header_sent = GRN_FALSE;
  data->raw.r = r;
  data->raw.rc = NGX_OK;
  data->raw.free_chain = NULL;
  data->raw.busy_chain = NULL;

  GRN_TEXT_INIT(&(data->typed.head), GRN_NO_FLAGS);
  GRN_TEXT_INIT(&(data->typed.body), GRN_NO_FLAGS);
  GRN_TEXT_INIT(&(data->typed.foot), GRN_NO_FLAGS);

  grn_ctx_use(context, grn_ctx_db(&(location_conf->context)));
  rc = ngx_http_groonga_context_check_error(r->connection->log, context);
  if (rc != NGX_OK) {
    return rc;
  }

  grn_ctx_recv_handler_set(context,
                           ngx_http_groonga_context_receive_handler,
                           data);

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_handler_process_command_path(ngx_http_request_t *r,
                                              ngx_str_t *command_path,
                                              ngx_http_groonga_handler_data_t *data)
{
  grn_ctx *context;
  grn_obj uri;

  context = &(data->context);
  GRN_TEXT_INIT(&uri, 0);
  GRN_TEXT_PUTS(context, &uri, "/d/");
  GRN_TEXT_PUT(context, &uri, command_path->data, command_path->len);
  grn_ctx_send(context, GRN_TEXT_VALUE(&uri), GRN_TEXT_LEN(&uri),
               GRN_NO_FLAGS);
  ngx_http_groonga_context_log_error(r->connection->log, context);
  GRN_OBJ_FIN(context, &uri);

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_handler_validate_post_command(ngx_http_request_t *r,
                                               ngx_str_t *command_path,
                                               ngx_http_groonga_handler_data_t *data)
{
  grn_ctx *context;
  ngx_str_t command;

  command.data = command_path->data;
  if (r->args.len == 0) {
    command.len = command_path->len;
  } else {
    command.len = command_path->len - r->args.len - strlen("?");
  }
  if (ngx_str_equal_c_string(&command, "load")) {
    return NGX_OK;
  }

  context = &(data->context);
  ngx_http_groonga_handler_set_content_type(r, "text/plain");
  GRN_TEXT_PUTS(context, &(data->typed.body),
                "command for POST must be <load>: <");
  GRN_TEXT_PUT(context, &(data->typed.body), command.data, command.len);
  GRN_TEXT_PUTS(context, &(data->typed.body), ">");

  return NGX_HTTP_BAD_REQUEST;
}

static ngx_int_t
ngx_http_groonga_send_lines(grn_ctx *context,
                            ngx_http_request_t *r,
                            u_char *current,
                            u_char *last)
{
  ngx_int_t rc;

  u_char *line_start;

  for (line_start = current; current < last; current++) {
    if (*current != '\n') {
      continue;
    }

    grn_ctx_send(context, (const char *)line_start, current - line_start,
                 GRN_NO_FLAGS);
    rc = ngx_http_groonga_context_check_error(r->connection->log, context);
    if (rc != NGX_OK) {
      return rc;
    }
    line_start = current + 1;
  }
  if (line_start < current) {
    grn_ctx_send(context, (const char *)line_start, current - line_start,
                 GRN_NO_FLAGS);
    rc = ngx_http_groonga_context_check_error(r->connection->log, context);
    if (rc != NGX_OK) {
      return rc;
    }
  }

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_join_request_body_chain(ngx_http_request_t *r,
                                         ngx_chain_t *chain,
                                         u_char **out_start,
                                         u_char **out_end)
{
  ngx_int_t rc;

  ngx_log_t *log = r->connection->log;

  ngx_chain_t *current;
  u_char *out;
  size_t out_size;

  u_char *out_cursor;
  ngx_buf_t *buffer;
  size_t buffer_size;

  out_size = 0;
  for (current = chain; current; current = current->next) {
    out_size += ngx_buf_size(current->buf);
  }
  out = ngx_palloc(r->pool, out_size);
  if (!out) {
    ngx_log_error(NGX_LOG_ERR, log, 0,
                  "http_groonga: failed to allocate memory for request body");
    return NGX_ERROR;
  }

  out_cursor = out;
  for (current = chain; current; current = current->next) {
    buffer = current->buf;
    buffer_size = ngx_buf_size(current->buf);

    if (buffer->file) {
      rc = ngx_read_file(buffer->file, out_cursor, buffer_size, 0);
      if (rc < 0) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "http_groonga: failed to read a request body stored in a file");
        return rc;
      }
    } else {
      ngx_memcpy(out_cursor, buffer->pos, buffer_size);
    }
    out_cursor += buffer_size;
  }

  *out_start = out;
  *out_end = out + out_size;

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_handler_process_body(ngx_http_request_t *r,
                                      ngx_http_groonga_handler_data_t *data)
{
  ngx_int_t rc;

  grn_ctx *context;

  ngx_buf_t *body;
  u_char *body_data;
  u_char *body_data_end;

  context = &(data->context);

  body = r->request_body->bufs->buf;
  if (!body) {
    ngx_http_groonga_handler_set_content_type(r, "text/plain");
    GRN_TEXT_PUTS(context, &(data->typed.body), "must send load data as body");
    return NGX_HTTP_BAD_REQUEST;
  }

  rc = ngx_http_groonga_join_request_body_chain(r,
                                                r->request_body->bufs,
                                                &body_data,
                                                &body_data_end);
  if (rc != NGX_OK) {
    return rc;
  }

  rc = ngx_http_groonga_send_lines(context, r, body_data, body_data_end);
  ngx_pfree(r->pool, body_data);

  return rc;
}


static ngx_int_t
ngx_http_groonga_handler_process_load(ngx_http_request_t *r,
                                      ngx_str_t *command_path,
                                      ngx_http_groonga_handler_data_t *data)
{
  ngx_int_t rc;

  rc = ngx_http_groonga_handler_validate_post_command(r, command_path, data);
  if (rc != NGX_OK) {
    return rc;
  }

  rc = ngx_http_groonga_handler_process_command_path(r, command_path, data);
  if (rc != NGX_OK) {
    return rc;
  }

  rc = ngx_http_groonga_handler_process_body(r, data);
  if (rc != NGX_OK) {
    return rc;
  }

  return NGX_OK;
}

static ngx_chain_t *
ngx_http_groonga_attach_chain(ngx_chain_t *chain, ngx_chain_t *new_chain)
{
  ngx_chain_t *last_chain;

  if (new_chain->buf->last == new_chain->buf->pos) {
    return chain;
  }

  new_chain->buf->last_buf = 1;
  new_chain->next = NULL;
  if (!chain) {
    return new_chain;
  }

  chain->buf->last_buf = 0;
  last_chain = chain;
  while (last_chain->next) {
    last_chain = last_chain->next;
  }
  last_chain->next = new_chain;
  return chain;
}

static ngx_int_t
ngx_http_groonga_handler_send_response(ngx_http_request_t *r,
                                       ngx_http_groonga_handler_data_t *data)
{
  ngx_int_t rc;
  grn_ctx *context;
  const char *content_type;
  ngx_buf_t *head_buf, *body_buf, *foot_buf;
  ngx_chain_t head_chain, body_chain, foot_chain;
  ngx_chain_t *output_chain = NULL;

  if (data->raw.processed) {
    return data->raw.rc;
  }

  context = &(data->context);

  /* set the 'Content-type' header */
  if (r->headers_out.content_type.len == 0) {
    content_type = grn_ctx_get_mime_type(context);
    ngx_http_groonga_handler_set_content_type(r, content_type);
  }

  /* allocate buffers for a response body */
  head_buf = ngx_http_groonga_grn_obj_to_ngx_buf(r->pool, &(data->typed.head));
  if (!head_buf) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  body_buf = ngx_http_groonga_grn_obj_to_ngx_buf(r->pool, &(data->typed.body));
  if (!body_buf) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  foot_buf = ngx_http_groonga_grn_obj_to_ngx_buf(r->pool, &(data->typed.foot));
  if (!foot_buf) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  /* attach buffers to the buffer chain */
  head_chain.buf = head_buf;
  output_chain = ngx_http_groonga_attach_chain(output_chain, &head_chain);
  body_chain.buf = body_buf;
  output_chain = ngx_http_groonga_attach_chain(output_chain, &body_chain);
  foot_chain.buf = foot_buf;
  output_chain = ngx_http_groonga_attach_chain(output_chain, &foot_chain);

  /* set the status line */
  r->headers_out.status = NGX_HTTP_OK;
  r->headers_out.content_length_n = GRN_TEXT_LEN(&(data->typed.head)) +
                                    GRN_TEXT_LEN(&(data->typed.body)) +
                                    GRN_TEXT_LEN(&(data->typed.foot));
  if (r->headers_out.content_length_n == 0) {
    r->header_only = 1;
  }

  /* send the headers of your response */
  rc = ngx_http_send_header(r);

  if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
    return rc;
  }

  /* send the buffer chain of your response */
  rc = ngx_http_output_filter(r, output_chain);

  return rc;
}

static ngx_int_t
ngx_http_groonga_handler_get(ngx_http_request_t *r)
{
  ngx_int_t rc;
  ngx_str_t command_path;
  ngx_http_groonga_handler_data_t *data;

  rc = ngx_http_groonga_extract_command_path(r, &command_path);
  if (rc != NGX_OK) {
    return rc;
  }

  rc = ngx_http_groonga_handler_create_data(r, &data);
  if (rc != NGX_OK) {
    return rc;
  }

  rc = ngx_http_groonga_handler_process_command_path(r, &command_path, data);
  if (rc != NGX_OK) {
    return rc;
  }

  /* discard request body, since we don't need it here */
  rc = ngx_http_discard_request_body(r);
  if (rc != NGX_OK) {
    return rc;
  }

  rc = ngx_http_groonga_handler_send_response(r, data);

  return rc;
}

static void
ngx_http_groonga_handler_post_send_error_response(ngx_http_request_t *r,
                                                  ngx_int_t rc)
{
  r->headers_out.status = rc;
  r->headers_out.content_length_n = 0;
  r->header_only = 1;
  rc = ngx_http_send_header(r);
  ngx_http_finalize_request(r, rc);
}

static void
ngx_http_groonga_handler_post(ngx_http_request_t *r)
{
  ngx_int_t rc;
  ngx_str_t command_path;
  ngx_http_groonga_handler_data_t *data = NULL;

  rc = ngx_http_groonga_extract_command_path(r, &command_path);
  if (rc != NGX_OK) {
    ngx_http_groonga_handler_post_send_error_response(r, rc);
    return;
  }

  rc = ngx_http_groonga_handler_create_data(r, &data);
  if (rc != NGX_OK) {
    ngx_http_groonga_handler_post_send_error_response(r, rc);
    return;
  }

  rc = ngx_http_groonga_handler_process_load(r, &command_path, data);
  if (rc != NGX_OK) {
    ngx_http_groonga_handler_post_send_error_response(r, rc);
    return;
  }

  ngx_http_groonga_handler_send_response(r, data);
  ngx_http_finalize_request(r, rc);
}

static ngx_int_t
ngx_http_groonga_handler(ngx_http_request_t *r)
{
  ngx_int_t rc;

  switch (r->method) {
  case NGX_HTTP_GET:
  case NGX_HTTP_HEAD:
    rc = ngx_http_groonga_handler_get(r);
    break;
  case NGX_HTTP_POST:
    rc = ngx_http_read_client_request_body(r, ngx_http_groonga_handler_post);
    if (rc < NGX_HTTP_SPECIAL_RESPONSE) {
      rc = NGX_DONE;
    }
    break;
  default:
    rc = NGX_HTTP_NOT_ALLOWED;
    break;
  }

  return rc;
}

static char *
ngx_http_groonga_conf_set_groonga_slot(ngx_conf_t *cf, ngx_command_t *cmd,
                                       void *conf)
{
  char *status;
  ngx_http_core_loc_conf_t *location_conf;
  ngx_http_groonga_loc_conf_t *groonga_location_conf = conf;

  status = ngx_conf_set_flag_slot(cf, cmd, conf);
  if (status != NGX_CONF_OK) {
    return status;
  }

  location_conf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
  if (groonga_location_conf->enabled) {
    location_conf->handler = ngx_http_groonga_handler;
    groonga_location_conf->name =
      ngx_str_null_terminate(cf->pool, &(location_conf->name));
    groonga_location_conf->config_file =
      ngx_str_null_terminate(cf->pool, &(cf->conf_file->file.name));
    groonga_location_conf->config_line = cf->conf_file->line;
  } else {
    location_conf->handler = NULL;
  }

  return NGX_CONF_OK;
}

static char *
ngx_http_groonga_conf_set_log_path_slot(ngx_conf_t *cf, ngx_command_t *cmd,
                                        void *conf)
{
  char *status;
  ngx_http_groonga_loc_conf_t *groonga_location_conf = conf;

  status = ngx_conf_set_str_slot(cf, cmd, conf);
  if (status != NGX_CONF_OK) {
    return status;
  }

  if (!groonga_location_conf->log_path.data) {
    return NGX_CONF_OK;
  }

  if (!ngx_str_is_custom_path(&(groonga_location_conf->log_path))) {
    return NGX_CONF_OK;
  }

  groonga_location_conf->log_file =
    ngx_conf_open_file(cf->cycle, &(groonga_location_conf->log_path));
  if (!groonga_location_conf->log_file) {
    ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                  "http_groonga: failed to open groonga log file: <%V>",
                  &(groonga_location_conf->log_path));
    return NGX_CONF_ERROR;
  }

  return NGX_CONF_OK;
}

static char *
ngx_http_groonga_conf_set_log_level_slot(ngx_conf_t *cf, ngx_command_t *cmd,
                                         void *conf)
{
  char *status = NGX_CONF_OK;
  ngx_http_groonga_loc_conf_t *groonga_location_conf = conf;
  char *value;

  value = ngx_str_null_terminate(cf->cycle->pool,
                                 ((ngx_str_t *)cf->args->elts) + 1);
  if (strcasecmp(value, "none") == 0) {
    groonga_location_conf->log_level = GRN_LOG_NONE;
  } else if (strcasecmp(value, "emergency") == 0) {
    groonga_location_conf->log_level = GRN_LOG_EMERG;
  } else if (strcasecmp(value, "alert") == 0) {
    groonga_location_conf->log_level = GRN_LOG_ALERT;
  } else if (strcasecmp(value, "critical") == 0) {
    groonga_location_conf->log_level = GRN_LOG_CRIT;
  } else if (strcasecmp(value, "error") == 0) {
    groonga_location_conf->log_level = GRN_LOG_ERROR;
  } else if (strcasecmp(value, "warning") == 0) {
    groonga_location_conf->log_level = GRN_LOG_WARNING;
  } else if (strcasecmp(value, "notice") == 0) {
    groonga_location_conf->log_level = GRN_LOG_NOTICE;
  } else if (strcasecmp(value, "info") == 0) {
    groonga_location_conf->log_level = GRN_LOG_INFO;
  } else if (strcasecmp(value, "debug") == 0) {
    groonga_location_conf->log_level = GRN_LOG_DEBUG;
  } else if (strcasecmp(value, "dump") == 0) {
    groonga_location_conf->log_level = GRN_LOG_DUMP;
  } else {
    status = "must be one of 'none', 'emergency', 'alert', "
      "'ciritical', 'error', 'warning', 'notice', 'info', 'debug' and 'dump'";
  }
  ngx_pfree(cf->cycle->pool, value);

  return status;
}

static char *
ngx_http_groonga_conf_set_query_log_path_slot(ngx_conf_t *cf,
                                              ngx_command_t *cmd,
                                              void *conf)
{
  char *status;
  ngx_http_groonga_loc_conf_t *groonga_location_conf = conf;

  status = ngx_conf_set_str_slot(cf, cmd, conf);
  if (status != NGX_CONF_OK) {
    return status;
  }

  if (!groonga_location_conf->query_log_path.data) {
    return NGX_CONF_OK;
  }

  if (!ngx_str_is_custom_path(&(groonga_location_conf->query_log_path))) {
    return NGX_CONF_OK;
  }

  groonga_location_conf->query_log_file =
    ngx_conf_open_file(cf->cycle, &(groonga_location_conf->query_log_path));
  if (!groonga_location_conf->query_log_file) {
    ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                  "http_groonga: failed to open groonga query log file: <%V>",
                  &(groonga_location_conf->query_log_path));
    return NGX_CONF_ERROR;
  }

  return NGX_CONF_OK;
}

static void *
ngx_http_groonga_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_groonga_loc_conf_t *conf;
  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_groonga_loc_conf_t));
  if (conf == NULL) {
    return NGX_CONF_ERROR;
  }

  conf->enabled = NGX_CONF_UNSET;
  conf->database_path.data = NULL;
  conf->database_path.len = 0;
  conf->database_path_cstr = NULL;
  conf->database_auto_create = NGX_CONF_UNSET;
  conf->base_path.data = NULL;
  conf->base_path.len = 0;
  conf->log_path.data = NULL;
  conf->log_path.len = 0;
  conf->log_file = NULL;
  conf->log_level = GRN_LOG_DEFAULT_LEVEL;
  conf->query_log_path.data = NULL;
  conf->query_log_path.len = 0;
  conf->query_log_file = NULL;
  conf->cache_limit = NGX_CONF_UNSET_SIZE;
  conf->config_file = NULL;
  conf->config_line = 0;
  conf->cache = NULL;

  return conf;
}

static char *
ngx_http_groonga_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_groonga_loc_conf_t *prev = parent;
  ngx_http_groonga_loc_conf_t *conf = child;

  ngx_conf_merge_str_value(conf->database_path, prev->database_path, NULL);
  ngx_conf_merge_value(conf->database_auto_create,
                       prev->database_auto_create,
                       GRN_TRUE);
  ngx_conf_merge_size_value(conf->cache_limit, prev->cache_limit,
                            GRN_CACHE_DEFAULT_MAX_N_ENTRIES);

#ifdef NGX_HTTP_GROONGA_LOG_PATH
  if (!conf->log_file) {
    ngx_str_t default_log_path;
    default_log_path.data = (u_char *)NGX_HTTP_GROONGA_LOG_PATH;
    default_log_path.len = strlen(NGX_HTTP_GROONGA_LOG_PATH);
    conf->log_file = ngx_conf_open_file(cf->cycle, &default_log_path);
    if (!conf->log_file) {
      ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                    "http_groonga: "
                    "failed to open the default groonga log file: <%V>",
                    &default_log_path);
      return NGX_CONF_ERROR;
    }
  }
#endif

  ngx_conf_merge_str_value(conf->query_log_path, prev->query_log_path,
                           NGX_HTTP_GROONGA_QUERY_LOG_PATH);
  if (!conf->query_log_file &&
      ngx_str_is_custom_path(&(conf->query_log_path)) &&
      conf->enabled) {
    conf->query_log_file = ngx_conf_open_file(cf->cycle,
                                              &(conf->query_log_path));
    if (!conf->query_log_file) {
      ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                    "http_groonga: "
                    "failed to open the default groonga query log file: <%V>",
                    &(conf->query_log_path));
      return NGX_CONF_ERROR;
    }
  }

  return NGX_CONF_OK;
}

static void
ngx_http_groonga_each_loc_conf_in_tree(ngx_http_location_tree_node_t *node,
                                       ngx_http_groonga_loc_conf_callback_pt callback,
                                       void *user_data)
{
  if (!node) {
    return;
  }

  if (node->exact && node->exact->handler == ngx_http_groonga_handler) {
    callback(node->exact->loc_conf[ngx_http_groonga_module.ctx_index],
             user_data);
  }

  if (node->inclusive && node->inclusive->handler == ngx_http_groonga_handler) {
    callback(node->inclusive->loc_conf[ngx_http_groonga_module.ctx_index],
             user_data);
  }

  ngx_http_groonga_each_loc_conf_in_tree(node->left, callback, user_data);
  ngx_http_groonga_each_loc_conf_in_tree(node->right, callback, user_data);
  ngx_http_groonga_each_loc_conf_in_tree(node->tree, callback, user_data);
}

static void
ngx_http_groonga_each_loc_conf(ngx_http_conf_ctx_t *http_conf,
                               ngx_http_groonga_loc_conf_callback_pt callback,
                               void *user_data)
{
  ngx_http_core_main_conf_t *main_conf;
  ngx_http_core_srv_conf_t **server_confs;
  ngx_uint_t i;

  if (!http_conf) {
    return;
  }

  main_conf = http_conf->main_conf[ngx_http_core_module.ctx_index];
  server_confs = main_conf->servers.elts;
  for (i = 0; i < main_conf->servers.nelts; i++) {
    ngx_http_core_srv_conf_t *server_conf;
    ngx_http_core_loc_conf_t *location_conf;

    server_conf = server_confs[i];
    location_conf = server_conf->ctx->loc_conf[ngx_http_core_module.ctx_index];
    ngx_http_groonga_each_loc_conf_in_tree(location_conf->static_locations,
                                           callback,
                                           user_data);
  }
}

static ngx_int_t
ngx_http_groonga_mkdir_p(ngx_log_t *log, const char *dir_name)
{
  char sub_path[PATH_MAX];
  size_t i, dir_name_length;

  dir_name_length = strlen(dir_name);
  sub_path[0] = dir_name[0];
  for (i = 1; i < dir_name_length + 1; i++) {
    if (dir_name[i] == '/' || dir_name[i] == '\0') {
      struct stat stat_buffer;
      sub_path[i] = '\0';
      if (stat(sub_path, &stat_buffer) == -1) {
        if (ngx_create_dir(sub_path, 0700) == -1) {
          ngx_log_error(NGX_LOG_EMERG, log, 0,
                        "failed to create directory: %s (%s): %s",
                        sub_path, dir_name,
                        strerror(errno));
          return NGX_ERROR;
        }
      }
    }
    sub_path[i] = dir_name[i];
  }

  return NGX_OK;
}

static void
ngx_http_groonga_create_database(ngx_http_groonga_loc_conf_t *location_conf,
                                 ngx_http_groonga_database_callback_data_t *data)
{
  const char *database_base_name;
  grn_ctx *context;

  database_base_name = strrchr(location_conf->database_path_cstr, '/');
  if (database_base_name) {
    char database_dir[PATH_MAX];
    database_dir[0] = '\0';
    strncat(database_dir,
            location_conf->database_path_cstr,
            database_base_name - location_conf->database_path_cstr);
    data->rc = ngx_http_groonga_mkdir_p(data->log, database_dir);
    if (data->rc != NGX_OK) {
      return;
    }
  }

  context = &(location_conf->context);
  grn_db_create(context, location_conf->database_path_cstr, NULL);
  if (context->rc == GRN_SUCCESS) {
    return;
  }

  ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                "failed to create groonga database: %s",
                context->errbuf);
  data->rc = NGX_ERROR;
}

static void
ngx_http_groonga_open_database_callback(ngx_http_groonga_loc_conf_t *location_conf,
                                        void *user_data)
{
  ngx_http_groonga_database_callback_data_t *data = user_data;
  grn_ctx *context;

  context = &(location_conf->context);
  data->rc = ngx_http_groonga_context_init(context, location_conf,
                                           data->pool, data->log);
  if (data->rc != NGX_OK) {
    return;
  }

  if (!location_conf->database_path.data) {
    ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                  "%s: \"groonga_database\" must be specified in block at %s:%d",
                  location_conf->name,
                  location_conf->config_file,
                  location_conf->config_line);
    data->rc = NGX_ERROR;
    return;
  }

  if (!location_conf->database_path_cstr) {
    location_conf->database_path_cstr =
      ngx_str_null_terminate(data->pool, &(location_conf->database_path));
  }

  grn_db_open(context, location_conf->database_path_cstr);
  if (context->rc != GRN_SUCCESS) {
    if (location_conf->database_auto_create) {
      ngx_http_groonga_create_database(location_conf, data);
    } else {
      ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                    "failed to open groonga database: %s",
                    context->errbuf);
      data->rc = NGX_ERROR;
      return;
    }
  }

  location_conf->cache = grn_cache_open(context);
  if (!location_conf->cache) {
    ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                  "failed to open groonga cache: %s",
                  context->errbuf);
    data->rc = NGX_ERROR;
    return;
  }
  if (location_conf->cache_limit != NGX_CONF_UNSET_SIZE) {
    grn_cache_set_max_n_entries(context,
                                location_conf->cache,
                                location_conf->cache_limit);
  }
}

static void
ngx_http_groonga_close_database_callback(ngx_http_groonga_loc_conf_t *location_conf,
                                         void *user_data)
{
  ngx_http_groonga_database_callback_data_t *data = user_data;
  grn_ctx *context;

  context = &(location_conf->context);
  ngx_http_groonga_context_init_logger(context,
                                       location_conf,
                                       data->pool,
                                       data->log);
  ngx_http_groonga_context_init_query_logger(context,
                                             location_conf,
                                             data->pool,
                                             data->log);
  grn_cache_current_set(context, location_conf->cache);

  grn_obj_close(context, grn_ctx_db(context));
  ngx_http_groonga_context_log_error(data->log, context);

  grn_cache_current_set(context, NULL);
  grn_cache_close(context, location_conf->cache);

  grn_ctx_fin(context);
}

static ngx_int_t
ngx_http_groonga_init_process(ngx_cycle_t *cycle)
{
  grn_rc rc;
  ngx_http_conf_ctx_t *http_conf;
  ngx_http_groonga_database_callback_data_t data;

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    return NGX_ERROR;
  }

  grn_set_segv_handler();

  http_conf =
    (ngx_http_conf_ctx_t *)ngx_get_conf(cycle->conf_ctx, ngx_http_module);

  data.log = cycle->log;
  data.pool = cycle->pool;
  data.rc = NGX_OK;
  ngx_http_groonga_each_loc_conf(http_conf,
                                 ngx_http_groonga_open_database_callback,
                                 &data);

  return data.rc;
}

static void
ngx_http_groonga_exit_process(ngx_cycle_t *cycle)
{
  ngx_http_conf_ctx_t *http_conf;
  ngx_http_groonga_database_callback_data_t data;

  http_conf =
    (ngx_http_conf_ctx_t *)ngx_get_conf(cycle->conf_ctx, ngx_http_module);
  data.log = cycle->log;
  data.pool = cycle->pool;
  ngx_http_groonga_each_loc_conf(http_conf,
                                 ngx_http_groonga_close_database_callback,
                                 &data);

  grn_fin();

  return;
}

/* entry point */
static ngx_command_t ngx_http_groonga_commands[] = {
  { ngx_string("groonga"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_http_groonga_conf_set_groonga_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, enabled),
    NULL },

  { ngx_string("groonga_database"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, database_path),
    NULL },

  { ngx_string("groonga_database_auto_create"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, database_auto_create),
    NULL },

  { ngx_string("groonga_base_path"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, base_path),
    NULL },

  { ngx_string("groonga_log_path"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_http_groonga_conf_set_log_path_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, log_path),
    NULL },

  { ngx_string("groonga_log_level"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_http_groonga_conf_set_log_level_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL },

  { ngx_string("groonga_query_log_path"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_http_groonga_conf_set_query_log_path_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, query_log_path),
    NULL },

  { ngx_string("groonga_cache_limit"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_size_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, cache_limit),
    NULL },

  ngx_null_command
};

static ngx_http_module_t ngx_http_groonga_module_ctx = {
  NULL, /* preconfiguration */
  NULL, /* postconfiguration */

  NULL, /* create main configuration */
  NULL, /* init main configuration */

  NULL, /* create server configuration */
  NULL, /* merge server configuration */

  ngx_http_groonga_create_loc_conf, /* create location configuration */
  ngx_http_groonga_merge_loc_conf, /* merge location configuration */
};

ngx_module_t ngx_http_groonga_module = {
  NGX_MODULE_V1,
  &ngx_http_groonga_module_ctx, /* module context */
  ngx_http_groonga_commands, /* module directives */
  NGX_HTTP_MODULE, /* module type */
  NULL, /* init master */
  NULL, /* init module */
  ngx_http_groonga_init_process, /* init process */
  NULL, /* init thread */
  NULL, /* exit thread */
  ngx_http_groonga_exit_process, /* exit process */
  NULL, /* exit master */
  NGX_MODULE_V1_PADDING
};
