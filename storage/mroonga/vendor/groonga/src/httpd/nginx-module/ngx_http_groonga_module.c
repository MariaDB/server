/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012-2017 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef WIN32
# define NGX_GRN_SUPPORT_STOP_BY_COMMAND
#endif

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <groonga.h>
#include <groonga/plugin.h>

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
  ngx_msec_t default_request_timeout_msec;
  char *config_file;
  int config_line;
  char *name;
  grn_obj *database;
  grn_cache *cache;
  ngx_str_t cache_base_path;
} ngx_http_groonga_loc_conf_t;

typedef struct {
  ngx_log_t *log;
  ngx_pool_t *pool;
  ngx_int_t rc;
} ngx_http_groonga_database_callback_data_t;

typedef struct {
  grn_bool initialized;
  grn_rc rc;
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

typedef void (*ngx_http_groonga_loc_conf_callback_pt)(ngx_http_groonga_loc_conf_t *conf, void *user_data);

ngx_module_t ngx_http_groonga_module;

static grn_ctx ngx_http_groonga_context;
static grn_ctx *context = &ngx_http_groonga_context;
static ngx_http_groonga_loc_conf_t *ngx_http_groonga_current_location_conf = NULL;

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

static uint32_t
ngx_http_groonga_get_thread_limit(void *data)
{
  return 1;
}

static ngx_int_t
ngx_http_groonga_grn_rc_to_http_status(grn_rc rc)
{
  switch (rc) {
  case GRN_SUCCESS :
    return NGX_HTTP_OK;
  case GRN_INVALID_ARGUMENT :
  case GRN_FUNCTION_NOT_IMPLEMENTED :
  case GRN_SYNTAX_ERROR :
    return NGX_HTTP_BAD_REQUEST;
  case GRN_CANCEL :
    return NGX_HTTP_REQUEST_TIME_OUT;
  default :
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
}

static void
ngx_http_groonga_write_fd(ngx_fd_t fd,
                          u_char *buffer, size_t buffer_size,
                          const char *message, size_t message_size)
{
  size_t rest_message_size = message_size;
  const char *current_message = message;

  while (rest_message_size > 0) {
    size_t current_message_size;

    if (rest_message_size > NGX_MAX_ERROR_STR) {
      current_message_size = NGX_MAX_ERROR_STR;
    } else {
      current_message_size = rest_message_size;
    }

    grn_memcpy(buffer, current_message, current_message_size);
    ngx_write_fd(fd, buffer, current_message_size);
    rest_message_size -= current_message_size;
    current_message += current_message_size;
  }
}

static void
ngx_http_groonga_logger_log(grn_ctx *ctx, grn_log_level level,
                            const char *timestamp, const char *title,
                            const char *message, const char *location,
                            void *user_data)
{
  ngx_open_file_t *file = user_data;
  char level_marks[] = " EACewnid-";
  u_char buffer[NGX_MAX_ERROR_STR];

  if (!file) {
    return;
  }

  ngx_http_groonga_write_fd(file->fd,
                            buffer, NGX_MAX_ERROR_STR,
                            timestamp, strlen(timestamp));
  ngx_write_fd(file->fd, "|", 1);
  ngx_write_fd(file->fd, level_marks + level, 1);
  ngx_write_fd(file->fd, "|", 1);
  if (location && *location) {
    ngx_http_groonga_write_fd(file->fd,
                              buffer, NGX_MAX_ERROR_STR,
                              location, strlen(location));
    ngx_write_fd(file->fd, ": ", 2);
    if (title && *title) {
      ngx_http_groonga_write_fd(file->fd,
                                buffer, NGX_MAX_ERROR_STR,
                                title, strlen(title));
      ngx_write_fd(file->fd, " ", 1);
    }
  } else {
    ngx_http_groonga_write_fd(file->fd,
                              buffer, NGX_MAX_ERROR_STR,
                              title, strlen(title));
    ngx_write_fd(file->fd, " ", 1);
  }
  ngx_http_groonga_write_fd(file->fd,
                            buffer, NGX_MAX_ERROR_STR,
                            message, strlen(message));
  ngx_write_fd(file->fd, "\n", 1);
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
}

static grn_logger ngx_http_groonga_logger = {
  GRN_LOG_DEFAULT_LEVEL,
  GRN_LOG_TIME | GRN_LOG_MESSAGE | GRN_LOG_PID,
  NULL,
  ngx_http_groonga_logger_log,
  ngx_http_groonga_logger_reopen,
  ngx_http_groonga_logger_fin
};

static ngx_int_t
ngx_http_groonga_context_init_logger(ngx_http_groonga_loc_conf_t *location_conf,
                                     ngx_pool_t *pool,
                                     ngx_log_t *log)
{
  if (ngx_http_groonga_current_location_conf) {
    ngx_http_groonga_current_location_conf->log_level =
      grn_logger_get_max_level(context);
  }

  ngx_http_groonga_logger.max_level = location_conf->log_level;
  ngx_http_groonga_logger.user_data = location_conf->log_file;
  grn_logger_set(context, &ngx_http_groonga_logger);

  return NGX_OK;
}

static void
ngx_http_groonga_query_logger_log(grn_ctx *ctx, unsigned int flag,
                                  const char *timestamp, const char *info,
                                  const char *message, void *user_data)
{
  ngx_open_file_t *file = user_data;
  u_char buffer[NGX_MAX_ERROR_STR];
  u_char *last;

  if (!file) {
    return;
  }

  last = ngx_slprintf(buffer, buffer + NGX_MAX_ERROR_STR,
                      "%s|%s%s\n",
                      timestamp, info, message);
  ngx_write_fd(file->fd, buffer, last - buffer);
}

static void
ngx_http_groonga_query_logger_reopen(grn_ctx *ctx, void *user_data)
{
  ngx_reopen_files((ngx_cycle_t *)ngx_cycle, -1);
}

static void
ngx_http_groonga_query_logger_fin(grn_ctx *ctx, void *user_data)
{
}

static grn_query_logger ngx_http_groonga_query_logger = {
  GRN_QUERY_LOG_DEFAULT,
  NULL,
  ngx_http_groonga_query_logger_log,
  ngx_http_groonga_query_logger_reopen,
  ngx_http_groonga_query_logger_fin
};

static ngx_int_t
ngx_http_groonga_context_init_query_logger(ngx_http_groonga_loc_conf_t *location_conf,
                                           ngx_pool_t *pool,
                                           ngx_log_t *log)
{
  ngx_http_groonga_query_logger.user_data = location_conf->query_log_file;
  grn_query_logger_set(context, &ngx_http_groonga_query_logger);

  return NGX_OK;
}

static ngx_int_t
ngx_http_groonga_context_init(ngx_http_groonga_loc_conf_t *location_conf,
                              ngx_pool_t *pool,
                              ngx_log_t *log)
{
  ngx_int_t status;

  if (location_conf == ngx_http_groonga_current_location_conf) {
    return NGX_OK;
  }

  status = ngx_http_groonga_context_init_logger(location_conf,
                                                pool,
                                                log);
  if (status == NGX_ERROR) {
    return status;
  }

  status = ngx_http_groonga_context_init_query_logger(location_conf,
                                                      pool,
                                                      log);
  if (status == NGX_ERROR) {
    return status;
  }

  grn_ctx_use(context, location_conf->database);
  grn_cache_current_set(context, location_conf->cache);

  /* TODO: It doesn't work yet. We need to implement request timeout
   * handler. */
  if (location_conf->default_request_timeout_msec == NGX_CONF_UNSET_MSEC) {
    grn_set_default_request_timeout(0.0);
  } else {
    double timeout;
    timeout = location_conf->default_request_timeout_msec / 1000.0;
    grn_set_default_request_timeout(timeout);
  }

  ngx_http_groonga_current_location_conf = location_conf;

  return status;
}

static void
ngx_http_groonga_context_log_error(ngx_log_t *log)
{
  if (context->rc == GRN_SUCCESS) {
    return;
  }

  ngx_log_error(NGX_LOG_ERR, log, 0, "%s", context->errbuf);
}

static ngx_int_t
ngx_http_groonga_context_check_error(ngx_log_t *log)
{
  if (context->rc == GRN_SUCCESS) {
    return NGX_OK;
  } else {
    ngx_http_groonga_context_log_error(log);
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

  if (!data->initialized) {
    return;
  }

  GRN_OBJ_FIN(context, &(data->typed.head));
  GRN_OBJ_FIN(context, &(data->typed.body));
  GRN_OBJ_FIN(context, &(data->typed.foot));
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

    ngx_rc = ngx_os_signal_process((ngx_cycle_t *)ngx_cycle,
                                   "quit",
                                   ngx_pid);
    if (ngx_rc == NGX_OK) {
      context->stat &= ~GRN_CTX_QUIT;
      grn_ctx_recv(context, &result, &result_size, &recv_flags);
      context->stat |= GRN_CTX_QUIT;
    } else {
      context->rc = GRN_OPERATION_NOT_PERMITTED;
      result = "false";
      result_size = strlen(result);
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

  switch (grn_ctx_get_output_type(context)) {
  case GRN_CONTENT_GROONGA_COMMAND_LIST :
  case GRN_CONTENT_NONE :
    ngx_http_groonga_context_receive_handler_raw(context, flags, data);
    break;
  default :
    ngx_http_groonga_context_receive_handler_typed(context, flags, data);
    break;
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

  location_conf = ngx_http_get_module_loc_conf(r, ngx_http_groonga_module);

  rc = ngx_http_groonga_context_init(location_conf, r->pool, r->connection->log);
  if (rc != NGX_OK) {
    return rc;
  }

  cleanup = ngx_http_cleanup_add(r, sizeof(ngx_http_groonga_handler_data_t));
  cleanup->handler = ngx_http_groonga_handler_cleanup;
  data = cleanup->data;
  *data_return = data;

  data->initialized = GRN_TRUE;
  data->rc = GRN_SUCCESS;

  data->raw.processed = GRN_FALSE;
  data->raw.header_sent = GRN_FALSE;
  data->raw.r = r;
  data->raw.rc = NGX_OK;
  data->raw.free_chain = NULL;
  data->raw.busy_chain = NULL;

  GRN_TEXT_INIT(&(data->typed.head), GRN_NO_FLAGS);
  GRN_TEXT_INIT(&(data->typed.body), GRN_NO_FLAGS);
  GRN_TEXT_INIT(&(data->typed.foot), GRN_NO_FLAGS);

  grn_ctx_use(context, location_conf->database);
  rc = ngx_http_groonga_context_check_error(r->connection->log);
  if (rc != NGX_OK) {
    return rc;
  }

  grn_ctx_recv_handler_set(context,
                           ngx_http_groonga_context_receive_handler,
                           data);

  return NGX_OK;
}

static void
ngx_http_groonga_handler_process_command_path(ngx_http_request_t *r,
                                              ngx_str_t *command_path,
                                              ngx_http_groonga_handler_data_t *data,
                                              int flags)
{
  grn_obj uri;

  GRN_TEXT_INIT(&uri, 0);
  GRN_TEXT_PUTS(context, &uri, "/d/");
  GRN_TEXT_PUT(context, &uri, command_path->data, command_path->len);
  grn_ctx_send(context, GRN_TEXT_VALUE(&uri), GRN_TEXT_LEN(&uri), flags);
  data->rc = context->rc;
  ngx_http_groonga_context_log_error(r->connection->log);
  GRN_OBJ_FIN(context, &uri);
}

static grn_bool
ngx_http_groonga_handler_validate_post_command(ngx_http_request_t *r,
                                               ngx_str_t *command_path,
                                               ngx_http_groonga_handler_data_t *data)
{
  ngx_str_t command;

  command.data = command_path->data;
  if (r->args.len == 0) {
    command.len = command_path->len;
  } else {
    command.len = command_path->len - r->args.len - strlen("?");
  }
  if (ngx_str_equal_c_string(&command, "load")) {
    return GRN_TRUE;
  }

  data->rc = GRN_INVALID_ARGUMENT;
  ngx_http_groonga_handler_set_content_type(r, "text/plain");
  GRN_TEXT_PUTS(context, &(data->typed.body),
                "command for POST must be <load>: <");
  GRN_TEXT_PUT(context, &(data->typed.body), command.data, command.len);
  GRN_TEXT_PUTS(context, &(data->typed.body), ">");

  return GRN_FALSE;
}

static void
ngx_http_groonga_send_body(ngx_http_request_t *r,
                           ngx_http_groonga_handler_data_t *data)
{
  ngx_log_t *log;
  grn_obj line_buffer;
  size_t line_start_offset;
  size_t line_check_start_offset;
  ngx_chain_t *chain;
  size_t line_buffer_chunk_size = 4096;

  log = r->connection->log;

  GRN_TEXT_INIT(&line_buffer, 0);
  line_start_offset = 0;
  line_check_start_offset = 0;
  for (chain = r->request_body->bufs; chain; chain = chain->next) {
    ngx_buf_t *buffer;
    size_t rest_buffer_size;
    off_t offset;

    buffer = chain->buf;
    rest_buffer_size = ngx_buf_size(buffer);
    offset = 0;
    while (rest_buffer_size > 0) {
      size_t current_buffer_size;

      if (rest_buffer_size > line_buffer_chunk_size) {
        current_buffer_size = line_buffer_chunk_size;
      } else {
        current_buffer_size = rest_buffer_size;
      }

      if (ngx_buf_in_memory(buffer)) {
        GRN_TEXT_PUT(context,
                     &line_buffer,
                     buffer->pos + offset,
                     current_buffer_size);
      } else {
        ngx_int_t rc;
        grn_bulk_reserve(context, &line_buffer, current_buffer_size);
        rc = ngx_read_file(buffer->file,
                           (u_char *)GRN_BULK_CURR(&line_buffer),
                           current_buffer_size,
                           offset);
        if (rc < 0) {
          GRN_PLUGIN_ERROR(context,
                           GRN_INPUT_OUTPUT_ERROR,
                           "[nginx][post][body][read] "
                           "failed to read a request body from file");
          goto exit;
        }
        GRN_BULK_INCR_LEN(&line_buffer, current_buffer_size);
      }
      offset += current_buffer_size;
      rest_buffer_size -= current_buffer_size;

      {
        const char *line_start;
        const char *line_current;
        const char *line_end;

        line_start = GRN_TEXT_VALUE(&line_buffer) + line_start_offset;
        line_end = GRN_TEXT_VALUE(&line_buffer) + GRN_TEXT_LEN(&line_buffer);
        for (line_current = line_start + line_check_start_offset;
             line_current < line_end;
             line_current++) {
          size_t line_length;
          int flags = GRN_NO_FLAGS;

          if (*line_current != '\n') {
            continue;
          }

          line_length = line_current - line_start + 1;
          if (line_current + 1 == line_end &&
              !chain->next &&
              rest_buffer_size == 0) {
            flags |= GRN_CTX_TAIL;
          }
          grn_ctx_send(context, line_start, line_length, flags);
          line_start_offset += line_length;
          line_start += line_length;
          ngx_http_groonga_context_log_error(log);
          if (context->rc != GRN_SUCCESS && data->rc == GRN_SUCCESS) {
            data->rc = context->rc;
          }
        }

        if (line_start_offset == 0) {
          line_buffer_chunk_size *= 2;
          line_check_start_offset = GRN_TEXT_LEN(&line_buffer);
        } else if ((size_t)GRN_TEXT_LEN(&line_buffer) == line_start_offset) {
          GRN_BULK_REWIND(&line_buffer);
          line_start_offset = 0;
          line_check_start_offset = 0;
        } else {
          size_t rest_line_size;
          rest_line_size = GRN_TEXT_LEN(&line_buffer) - line_start_offset;
          grn_memmove(GRN_TEXT_VALUE(&line_buffer),
                      GRN_TEXT_VALUE(&line_buffer) + line_start_offset,
                      rest_line_size);
          grn_bulk_truncate(context, &line_buffer, rest_line_size);
          line_start_offset = 0;
          line_check_start_offset = GRN_TEXT_LEN(&line_buffer);
        }
      }
    }
  }

  if (GRN_TEXT_LEN(&line_buffer) > 0) {
    grn_ctx_send(context,
                 GRN_TEXT_VALUE(&line_buffer),
                 GRN_TEXT_LEN(&line_buffer),
                 GRN_CTX_TAIL);
    ngx_http_groonga_context_log_error(log);
    if (context->rc != GRN_SUCCESS && data->rc == GRN_SUCCESS) {
      data->rc = context->rc;
    }
  }

exit :
  GRN_OBJ_FIN(context, &line_buffer);
}

static void
ngx_http_groonga_handler_process_body(ngx_http_request_t *r,
                                      ngx_http_groonga_handler_data_t *data)
{
  ngx_buf_t *body;

  body = r->request_body->bufs->buf;
  if (!body) {
    data->rc = GRN_INVALID_ARGUMENT;
    ngx_http_groonga_handler_set_content_type(r, "text/plain");
    GRN_TEXT_PUTS(context, &(data->typed.body), "must send load data as body");
    return;
  }

  ngx_http_groonga_send_body(r, data);
}


static void
ngx_http_groonga_handler_process_load(ngx_http_request_t *r,
                                      ngx_str_t *command_path,
                                      ngx_http_groonga_handler_data_t *data)
{
  if (!ngx_http_groonga_handler_validate_post_command(r, command_path, data)) {
    return;
  }

  ngx_http_groonga_handler_process_command_path(r,
                                                command_path,
                                                data,
                                                GRN_NO_FLAGS);
  if (data->rc != GRN_SUCCESS) {
    return;
  }

  ngx_http_groonga_handler_process_body(r, data);
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
  const char *content_type;
  ngx_buf_t *head_buf, *body_buf, *foot_buf;
  ngx_chain_t head_chain, body_chain, foot_chain;
  ngx_chain_t *output_chain = NULL;

  if (data->raw.processed) {
    return data->raw.rc;
  }

  /* set the 'Content-type' header */
  if (r->headers_out.content_type.len == 0) {
    grn_obj *foot = &(data->typed.foot);
    if (grn_ctx_get_output_type(context) == GRN_CONTENT_JSON &&
        GRN_TEXT_LEN(foot) > 0 &&
        GRN_TEXT_VALUE(foot)[GRN_TEXT_LEN(foot) - 1] == ';') {
      content_type = "application/javascript";
    } else {
      content_type = grn_ctx_get_mime_type(context);
    }
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
  r->headers_out.status = ngx_http_groonga_grn_rc_to_http_status(data->rc);
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

  ngx_http_groonga_handler_process_command_path(r,
                                                &command_path,
                                                data,
                                                GRN_CTX_TAIL);

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

  ngx_http_groonga_handler_process_load(r, &command_path, data);
  rc = ngx_http_groonga_handler_send_response(r, data);
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
  if (!grn_log_level_parse(value, &(groonga_location_conf->log_level))) {
    status = "must be one of 'none', 'emergency', 'alert', "
      "'critical', 'error', 'warning', 'notice', 'info', 'debug' and 'dump'";
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
                  "http_groonga: failed to open Groonga query log file: <%V>",
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
  conf->cache_base_path.data = NULL;
  conf->cache_base_path.len = 0;

  return conf;
}

static char *
ngx_http_groonga_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_groonga_loc_conf_t *prev = parent;
  ngx_http_groonga_loc_conf_t *conf = child;
  ngx_flag_t enabled = 0;

  if (conf->enabled != NGX_CONF_UNSET) {
    enabled = conf->enabled;
  }

  ngx_conf_merge_str_value(conf->database_path, prev->database_path, NULL);
  ngx_conf_merge_value(conf->database_auto_create,
                       prev->database_auto_create,
                       GRN_TRUE);
  ngx_conf_merge_size_value(conf->cache_limit, prev->cache_limit,
                            GRN_CACHE_DEFAULT_MAX_N_ENTRIES);

#ifdef NGX_HTTP_GROONGA_LOG_PATH
  ngx_conf_merge_str_value(conf->log_path, prev->log_path,
                           NGX_HTTP_GROONGA_LOG_PATH);
  if (!conf->log_file &&
      ngx_str_is_custom_path(&(conf->log_path)) &&
      enabled) {
    conf->log_file = ngx_conf_open_file(cf->cycle, &(conf->log_path));
    if (!conf->log_file) {
      ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                    "http_groonga: "
                    "failed to open the default Groonga log file: <%V>",
                    &(conf->log_path));
      return NGX_CONF_ERROR;
    }
  }
#endif

  ngx_conf_merge_str_value(conf->query_log_path, prev->query_log_path,
                           NGX_HTTP_GROONGA_QUERY_LOG_PATH);
  if (!conf->query_log_file &&
      ngx_str_is_custom_path(&(conf->query_log_path)) &&
      enabled) {
    conf->query_log_file = ngx_conf_open_file(cf->cycle,
                                              &(conf->query_log_path));
    if (!conf->query_log_file) {
      ngx_log_error(NGX_LOG_ERR, cf->cycle->log, 0,
                    "http_groonga: "
                    "failed to open the default Groonga query log file: <%V>",
                    &(conf->query_log_path));
      return NGX_CONF_ERROR;
    }
  }

  ngx_conf_merge_str_value(conf->cache_base_path,
                           prev->cache_base_path,
                           NULL);

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

#if NGX_PCRE
    if (location_conf->regex_locations) {
      ngx_uint_t j;
      for (j = 0; location_conf->regex_locations[j]; j++) {
        ngx_http_core_loc_conf_t *regex_location_conf;

        regex_location_conf = location_conf->regex_locations[j];
        if (regex_location_conf->handler == ngx_http_groonga_handler) {
          callback(regex_location_conf->loc_conf[ngx_http_groonga_module.ctx_index],
                   user_data);
        }
      }
    }
#endif
  }
}

static void
ngx_http_groonga_set_logger_callback(ngx_http_groonga_loc_conf_t *location_conf,
                                     void *user_data)
{
  ngx_http_groonga_database_callback_data_t *data = user_data;

  data->rc = ngx_http_groonga_context_init_logger(location_conf,
                                                  data->pool,
                                                  data->log);
  if (data->rc != NGX_OK) {
    return;
  }
  data->rc = ngx_http_groonga_context_init_query_logger(location_conf,
                                                        data->pool,
                                                        data->log);
  if (data->rc != NGX_OK) {
    return;
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

  location_conf->database =
    grn_db_create(context, location_conf->database_path_cstr, NULL);
  if (context->rc == GRN_SUCCESS) {
    return;
  }

  ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                "failed to create Groonga database: %s",
                context->errbuf);
  data->rc = NGX_ERROR;
}

static void
ngx_http_groonga_open_database_callback(ngx_http_groonga_loc_conf_t *location_conf,
                                        void *user_data)
{
  ngx_http_groonga_database_callback_data_t *data = user_data;

  data->rc = ngx_http_groonga_context_init_logger(location_conf,
                                                  data->pool,
                                                  data->log);
  if (data->rc != NGX_OK) {
    return;
  }
  data->rc = ngx_http_groonga_context_init_query_logger(location_conf,
                                                        data->pool,
                                                        data->log);
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

  location_conf->database =
    grn_db_open(context, location_conf->database_path_cstr);
  if (context->rc != GRN_SUCCESS) {
    if (location_conf->database_auto_create) {
      ngx_http_groonga_create_database(location_conf, data);
    } else {
      ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                    "failed to open Groonga database: %s",
                    context->errbuf);
      data->rc = NGX_ERROR;
    }
    if (data->rc != NGX_OK) {
      return;
    }
  }

  if (location_conf->cache_base_path.data &&
      ngx_str_is_custom_path(&(location_conf->cache_base_path))) {
    char cache_base_path[PATH_MAX];
    grn_memcpy(cache_base_path,
               location_conf->cache_base_path.data,
               location_conf->cache_base_path.len);
    cache_base_path[location_conf->cache_base_path.len] = '\0';
    location_conf->cache = grn_persistent_cache_open(context, cache_base_path);
  } else {
    location_conf->cache = grn_cache_open(context);
  }
  if (!location_conf->cache) {
    ngx_log_error(NGX_LOG_EMERG, data->log, 0,
                  "failed to open Groonga cache: %s",
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

  ngx_http_groonga_context_init_logger(location_conf,
                                       data->pool,
                                       data->log);
  ngx_http_groonga_context_init_query_logger(location_conf,
                                             data->pool,
                                             data->log);
  grn_cache_current_set(context, location_conf->cache);

  grn_obj_close(context, location_conf->database);
  ngx_http_groonga_context_log_error(data->log);

  grn_cache_current_set(context, NULL);
  grn_cache_close(context, location_conf->cache);
}

static ngx_int_t
ngx_http_groonga_init_process(ngx_cycle_t *cycle)
{
  grn_rc rc;
  ngx_http_conf_ctx_t *http_conf;
  ngx_http_groonga_database_callback_data_t data;

  grn_thread_set_get_limit_func(ngx_http_groonga_get_thread_limit, NULL);

#ifdef NGX_HTTP_GROONGA_LOG_PATH
  grn_default_logger_set_path(NGX_HTTP_GROONGA_LOG_PATH);
#endif

  http_conf =
    (ngx_http_conf_ctx_t *)ngx_get_conf(cycle->conf_ctx, ngx_http_module);

  data.log = cycle->log;
  data.pool = cycle->pool;
  data.rc = NGX_OK;
  ngx_http_groonga_each_loc_conf(http_conf,
                                 ngx_http_groonga_set_logger_callback,
                                 &data);

  if (data.rc != NGX_OK) {
    return data.rc;
  }

  rc = grn_init();
  if (rc != GRN_SUCCESS) {
    return NGX_ERROR;
  }

  grn_set_segv_handler();

  rc = grn_ctx_init(context, GRN_NO_FLAGS);
  if (rc != GRN_SUCCESS) {
    return NGX_ERROR;
  }

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

  grn_ctx_fin(context);

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

  { ngx_string("groonga_default_request_timeout"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_msec_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, default_request_timeout_msec),
    NULL },

  { ngx_string("groonga_cache_base_path"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_groonga_loc_conf_t, cache_base_path),
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
