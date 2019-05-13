/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2010-2015 Brazil

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

/* groonga origin headers */
#include <grn_str.h>
#include <grn_msgpack.h>

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>

#include <fcntl.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/resource.h>

#include "zmq_compatible.h"
#include <event.h>
#include <evhttp.h>
#include <groonga.h>
#include <pthread.h>

#include "util.h"

#define DEFAULT_PORT 8080
#define DEFAULT_MAX_THREADS 8

#define CONST_STR_LEN(x) x, x ? sizeof(x) - 1 : 0

#define LISTEN_BACKLOG 756
#define MIN_MAX_FDS 2048
#define MAX_THREADS 128 /* max 256 */

typedef enum {
  run_mode_none = 0,
  run_mode_usage,
  run_mode_daemon,
  run_mode_error
} run_mode;

#define RUN_MODE_MASK                0x007f
#define RUN_MODE_ENABLE_MAX_FD_CHECK 0x0080


typedef struct {
  grn_ctx *ctx;
  grn_obj *db;
  void *zmq_sock;
  grn_obj cmd_buf;
  grn_obj pass_through_parameters;
  pthread_t thd;
  uint32_t thread_id;
  struct event_base *base;
  struct evhttp *httpd;
  struct event pulse;
  const char *log_base_path;
  FILE *log_file;
  uint32_t log_count;
  grn_bool request_reopen_log_file;
} thd_data;

typedef struct {
  const char *db_path;
  const char *recv_endpoint;
  pthread_t thd;
  void *zmq_ctx;
} recv_thd_data;

#define CMD_BUF_SIZE 1024

static thd_data threads[MAX_THREADS];
static uint32_t default_max_threads = DEFAULT_MAX_THREADS;
static uint32_t max_threads;
static volatile sig_atomic_t loop = 1;
static grn_obj *db;
static uint32_t n_lines_per_log_file = 1000000;

static int
suggest_result(grn_ctx *ctx,
               struct evbuffer *res_buf, const char *types, const char *query,
               const char *target_name, int frequency_threshold,
               double conditional_probability_threshold, int limit,
               grn_obj *cmd_buf, grn_obj *pass_through_parameters)
{
  if (target_name && types && query) {
    GRN_BULK_REWIND(cmd_buf);
    GRN_TEXT_PUTS(ctx, cmd_buf, "/d/suggest?table=item_");
    grn_text_urlenc(ctx, cmd_buf, target_name, strlen(target_name));
    GRN_TEXT_PUTS(ctx, cmd_buf, "&column=kana&types=");
    grn_text_urlenc(ctx, cmd_buf, types, strlen(types));
    GRN_TEXT_PUTS(ctx, cmd_buf, "&query=");
    grn_text_urlenc(ctx, cmd_buf, query, strlen(query));
    GRN_TEXT_PUTS(ctx, cmd_buf, "&frequency_threshold=");
    grn_text_itoa(ctx, cmd_buf, frequency_threshold);
    GRN_TEXT_PUTS(ctx, cmd_buf, "&conditional_probability_threshold=");
    grn_text_ftoa(ctx, cmd_buf, conditional_probability_threshold);
    GRN_TEXT_PUTS(ctx, cmd_buf, "&limit=");
    grn_text_itoa(ctx, cmd_buf, limit);
    if (GRN_TEXT_LEN(pass_through_parameters) > 0) {
      GRN_TEXT_PUTS(ctx, cmd_buf, "&");
      GRN_TEXT_PUT(ctx, cmd_buf,
                   GRN_TEXT_VALUE(pass_through_parameters),
                   GRN_TEXT_LEN(pass_through_parameters));
    }
    {
      char *res;
      int flags;
      unsigned int res_len;

      grn_ctx_send(ctx, GRN_TEXT_VALUE(cmd_buf), GRN_TEXT_LEN(cmd_buf), 0);
      grn_ctx_recv(ctx, &res, &res_len, &flags);

      evbuffer_add(res_buf, res, res_len);
      return res_len;
    }
  } else {
    evbuffer_add(res_buf, "{}", 2);
    return 2;
  }
}

static void
log_send(struct evkeyvalq *output_headers, struct evbuffer *res_buf,
         thd_data *thd, struct evkeyvalq *get_args)
{
  uint64_t millisec;
  int frequency_threshold, limit;
  double conditional_probability_threshold;
  const char *callback, *types, *query, *client_id, *target_name,
             *learn_target_name;

  GRN_BULK_REWIND(&(thd->pass_through_parameters));
  parse_keyval(thd->ctx, get_args, &query, &types, &client_id, &target_name,
               &learn_target_name, &callback, &millisec, &frequency_threshold,
               &conditional_probability_threshold, &limit,
               &(thd->pass_through_parameters));

  /* send data to learn client */
  if (thd->zmq_sock && millisec && client_id && query && learn_target_name) {
    char c;
    size_t l;
    msgpack_packer pk;
    msgpack_sbuffer sbuf;
    int cnt, submit_flag = 0;

    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    cnt = 4;
    if (types && !strcmp(types, "submit")) {
      cnt++;
      types = NULL;
      submit_flag = 1;
    }
    msgpack_pack_map(&pk, cnt);

    c = 'i';
    msgpack_pack_str(&pk, 1);
    msgpack_pack_str_body(&pk, &c, 1);
    l = strlen(client_id);
    msgpack_pack_str(&pk, l);
    msgpack_pack_str_body(&pk, client_id, l);

    c = 'q';
    msgpack_pack_str(&pk, 1);
    msgpack_pack_str_body(&pk, &c, 1);
    l = strlen(query);
    msgpack_pack_str(&pk, l);
    msgpack_pack_str_body(&pk, query, l);

    c = 's';
    msgpack_pack_str(&pk, 1);
    msgpack_pack_str_body(&pk, &c, 1);
    msgpack_pack_uint64(&pk, millisec);

    c = 'l';
    msgpack_pack_str(&pk, 1);
    msgpack_pack_str_body(&pk, &c, 1);
    l = strlen(learn_target_name);
    msgpack_pack_str(&pk, l);
    msgpack_pack_str_body(&pk, learn_target_name, l);

    if (submit_flag) {
      c = 't';
      msgpack_pack_str(&pk, 1);
      msgpack_pack_str_body(&pk, &c, 1);
      msgpack_pack_true(&pk);
    }
    {
      zmq_msg_t msg;
      if (!zmq_msg_init_size(&msg, sbuf.size)) {
        memcpy((void *)zmq_msg_data(&msg), sbuf.data, sbuf.size);
        if (zmq_msg_send(&msg, thd->zmq_sock, 0) == -1) {
          print_error("zmq_msg_send() error");
        }
        zmq_msg_close(&msg);
      }
    }
    msgpack_sbuffer_destroy(&sbuf);
  }
  /* make result */
  {
    int content_length;
    if (callback) {
      evhttp_add_header(output_headers,
                        "Content-Type", "text/javascript; charset=UTF-8");
      content_length = strlen(callback);
      evbuffer_add(res_buf, callback, content_length);
      evbuffer_add(res_buf, "(", 1);
      content_length += suggest_result(thd->ctx,
                                       res_buf, types, query, target_name,
                                       frequency_threshold,
                                       conditional_probability_threshold,
                                       limit,
                                       &(thd->cmd_buf),
                                       &(thd->pass_through_parameters)) + 3;
      evbuffer_add(res_buf, ");", 2);
    } else {
      evhttp_add_header(output_headers,
                        "Content-Type", "application/json; charset=UTF-8");
      content_length = suggest_result(thd->ctx,
                                      res_buf, types, query, target_name,
                                      frequency_threshold,
                                      conditional_probability_threshold,
                                      limit,
                                      &(thd->cmd_buf),
                                      &(thd->pass_through_parameters));
    }
    if (content_length >= 0) {
#define NUM_BUF_SIZE 16
      char num_buf[NUM_BUF_SIZE];
      grn_snprintf(num_buf, NUM_BUF_SIZE, NUM_BUF_SIZE, "%d", content_length);
      evhttp_add_header(output_headers, "Content-Length", num_buf);
#undef NUM_BUF_SIZE
    }
  }
}

static void
cleanup_httpd_thread(thd_data *thd) {
  if (thd->log_file) {
    fclose(thd->log_file);
  }
  if (thd->httpd) {
    evhttp_free(thd->httpd);
  }
  if (thd->zmq_sock) {
    zmq_close(thd->zmq_sock);
  }
  grn_obj_unlink(thd->ctx, &(thd->cmd_buf));
  grn_obj_unlink(thd->ctx, &(thd->pass_through_parameters));
  if (thd->ctx) {
    grn_ctx_close(thd->ctx);
  }
  event_base_free(thd->base);
}

static void
close_log_file(thd_data *thread)
{
  fclose(thread->log_file);
  thread->log_file = NULL;
  thread->request_reopen_log_file = GRN_FALSE;
}

static void
generic_handler(struct evhttp_request *req, void *arg)
{
  struct evkeyvalq args;
  thd_data *thd = arg;

  if (!loop) {
    event_base_loopexit(thd->base, NULL);
    return;
  }
  if (!req->uri) { return; }

  evhttp_parse_query(req->uri, &args);
  {
    struct evbuffer *res_buf;
    if (!(res_buf = evbuffer_new())) {
      err(1, "failed to create response buffer");
    }

    evhttp_add_header(req->output_headers, "Connection", "close");

    log_send(req->output_headers, res_buf, thd, &args);
    evhttp_send_reply(req, HTTP_OK, "OK", res_buf);
    evbuffer_free(res_buf);
    /* logging */
    {
      if (thd->log_base_path) {
        if (thd->log_file && thd->request_reopen_log_file) {
          close_log_file(thd);
        }
        if (!thd->log_file) {
          time_t n;
          struct tm *t_st;
          char p[PATH_MAX + 1];

          time(&n);
          t_st = localtime(&n);

          grn_snprintf(p,
                       PATH_MAX,
                       PATH_MAX,
                       "%s%04d%02d%02d%02d%02d%02d-%02d",
                       thd->log_base_path,
                       t_st->tm_year + 1900,
                       t_st->tm_mon + 1,
                       t_st->tm_mday,
                       t_st->tm_hour,
                       t_st->tm_min,
                       t_st->tm_sec,
                       thd->thread_id);

          if (!(thd->log_file = fopen(p, "a"))) {
            print_error("cannot open log_file %s.", p);
          } else {
            thd->log_count = 0;
          }
        }
        if (thd->log_file) {
          fprintf(thd->log_file, "%s\n", req->uri);
          thd->log_count++;
          if (n_lines_per_log_file > 0 &&
              thd->log_count >= n_lines_per_log_file) {
            close_log_file(thd);
          }
        }
      }
    }
  }
  evhttp_clear_headers(&args);
}

static int
bind_socket(int port)
{
  int nfd;
  if ((nfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    print_error("cannot open socket for http.");
    return -1;
  } else {
    int r, one = 1;
    struct sockaddr_in addr;

    r = setsockopt(nfd, SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(int));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if ((r = bind(nfd, (struct sockaddr *)&addr, sizeof(addr))) < 0) {
      print_error("cannot bind socket for http.");
      return r;
    }
    if ((r = listen(nfd, LISTEN_BACKLOG)) < 0) {
      print_error("cannot listen socket for http.");
      return r;
    }
    if ((r = fcntl(nfd, F_GETFL, 0)) < 0 || fcntl(nfd, F_SETFL, r | O_NONBLOCK) < 0 ) {
      print_error("cannot fcntl socket for http.");
      return -1;
    }
    return nfd;
  }
}

static void
signal_handler(int sig)
{
  loop = 0;
}

static void
signal_reopen_log_file(int sig)
{
  uint32_t i;

  for (i = 0; i < max_threads; i++) {
    threads[i].request_reopen_log_file = GRN_TRUE;
  }
}

void
timeout_handler(int fd, short events, void *arg) {
  thd_data *thd = arg;
  if (!loop) {
    event_base_loopexit(thd->base, NULL);
  } else {
    struct timeval tv = {1, 0};
    evtimer_add(&(thd->pulse), &tv);
  }
}

static void *
dispatch(void *arg)
{
  event_base_dispatch((struct event_base *)arg);
  return NULL;
}

static void
msgpack2json(msgpack_object *o, grn_ctx *ctx, grn_obj *buf)
{
  switch (o->type) {
  case MSGPACK_OBJECT_POSITIVE_INTEGER:
    grn_text_ulltoa(ctx, buf, o->via.u64);
    break;
  case MSGPACK_OBJECT_STR:
    grn_text_esc(ctx, buf,
                 MSGPACK_OBJECT_STR_PTR(o),
                 MSGPACK_OBJECT_STR_SIZE(o));
    break;
  case MSGPACK_OBJECT_ARRAY:
    GRN_TEXT_PUTC(ctx, buf, '[');
    {
      int i;
      for (i = 0; i < o->via.array.size; i++) {
        msgpack2json(o->via.array.ptr, ctx, buf);
      }
    }
    GRN_TEXT_PUTC(ctx, buf, ']');
    break;
  case MSGPACK_OBJECT_FLOAT:
    grn_text_ftoa(ctx, buf, MSGPACK_OBJECT_FLOAT_VALUE(o));
    break;
  default:
    print_error("cannot handle this msgpack type.");
  }
}

static void
load_from_learner(msgpack_object *o, grn_ctx *ctx, grn_obj *cmd_buf)
{
  if (o->type == MSGPACK_OBJECT_MAP && o->via.map.size) {
    msgpack_object_kv *kv;
    msgpack_object *key;
    msgpack_object *value;
    kv = &(o->via.map.ptr[0]);
    key = &(kv->key);
    value = &(kv->val);
    if (key->type == MSGPACK_OBJECT_STR && MSGPACK_OBJECT_STR_SIZE(key) == 6 &&
        !memcmp(MSGPACK_OBJECT_STR_PTR(key), CONST_STR_LEN("target"))) {
      if (value->type == MSGPACK_OBJECT_STR) {
        int i;
        GRN_BULK_REWIND(cmd_buf);
        GRN_TEXT_PUTS(ctx, cmd_buf, "load --table ");
        GRN_TEXT_PUT(ctx, cmd_buf,
                     MSGPACK_OBJECT_STR_PTR(value),
                     MSGPACK_OBJECT_STR_SIZE(value));
        grn_ctx_send(ctx, GRN_TEXT_VALUE(cmd_buf), GRN_TEXT_LEN(cmd_buf), GRN_CTX_MORE);
        grn_ctx_send(ctx, CONST_STR_LEN("["), GRN_CTX_MORE);
        if (MSGPACK_OBJECT_STR_SIZE(value) > 5) {
          if (!memcmp(MSGPACK_OBJECT_STR_PTR(value), CONST_STR_LEN("item_")) ||
              !memcmp(MSGPACK_OBJECT_STR_PTR(value), CONST_STR_LEN("pair_"))) {
            char delim = '{';
            GRN_BULK_REWIND(cmd_buf);
            for (i = 1; i < o->via.map.size; i++) {
              GRN_TEXT_PUTC(ctx, cmd_buf, delim);
              kv = &(o->via.map.ptr[i]);
              msgpack2json(&(kv->key), ctx, cmd_buf);
              GRN_TEXT_PUTC(ctx, cmd_buf, ':');
              msgpack2json(&(kv->val), ctx, cmd_buf);
              delim = ',';
            }
            GRN_TEXT_PUTC(ctx, cmd_buf, '}');
            /* printf("msg: %.*s\n", GRN_TEXT_LEN(cmd_buf), GRN_TEXT_VALUE(cmd_buf)); */
            grn_ctx_send(ctx, GRN_TEXT_VALUE(cmd_buf), GRN_TEXT_LEN(cmd_buf), GRN_CTX_MORE);
          }
        }
        grn_ctx_send(ctx, CONST_STR_LEN("]"), 0);
        {
          char *res;
          int flags;
          unsigned int res_len;
          grn_ctx_recv(ctx, &res, &res_len, &flags);
        }
      }
    }
  }
}

static void
recv_handler(grn_ctx *ctx, void *zmq_recv_sock, msgpack_zone *mempool, grn_obj *cmd_buf)
{
  zmq_msg_t msg;

  if (zmq_msg_init(&msg)) {
    print_error("cannot init zmq message.");
  } else {
    if (zmq_msg_recv(&msg, zmq_recv_sock, 0) == -1) {
      print_error("cannot recv zmq message.");
    } else {
      msgpack_object obj;
      msgpack_unpack_return ret;

      ret = msgpack_unpack(zmq_msg_data(&msg), zmq_msg_size(&msg), NULL, mempool, &obj);
      if (MSGPACK_UNPACK_SUCCESS == ret) {
        load_from_learner(&obj, ctx, cmd_buf);
      } else {
        print_error("invalid recv data.");
      }
      msgpack_zone_clear(mempool);
    }
    zmq_msg_close(&msg);
  }
}

static void *
recv_from_learner(void *arg)
{
  void *zmq_recv_sock;
  recv_thd_data *thd = arg;

  if ((zmq_recv_sock = zmq_socket(thd->zmq_ctx, ZMQ_SUB))) {
    if (!zmq_connect(zmq_recv_sock, thd->recv_endpoint)) {
      grn_ctx ctx;
      if (!grn_ctx_init(&ctx, 0)) {
        if ((!grn_ctx_use(&ctx, db))) {
          msgpack_zone *mempool;
          if ((mempool = msgpack_zone_new(MSGPACK_ZONE_CHUNK_SIZE))) {
            grn_obj cmd_buf;
            zmq_pollitem_t items[] = {
              { zmq_recv_sock, 0, ZMQ_POLLIN, 0}
            };
            GRN_TEXT_INIT(&cmd_buf, 0);
            zmq_setsockopt(zmq_recv_sock, ZMQ_SUBSCRIBE, "", 0);
            while (loop) {
              zmq_poll(items, 1, 10000);
              if (items[0].revents & ZMQ_POLLIN) {
                recv_handler(&ctx, zmq_recv_sock, mempool, &cmd_buf);
              }
            }
            grn_obj_unlink(&ctx, &cmd_buf);
            msgpack_zone_free(mempool);
          } else {
            print_error("cannot create msgpack zone.");
          }
          /* db_close */
        } else {
          print_error("error in grn_db_open() on recv thread.");
        }
        grn_ctx_fin(&ctx);
      } else {
        print_error("error in grn_ctx_init() on recv thread.");
      }
    } else {
      print_error("cannot create recv zmq_socket.");
    }
  } else {
    print_error("cannot connect zmq_socket.");
  }
  return NULL;
}

static int
serve_threads(int nthreads, int port, const char *db_path, void *zmq_ctx,
              const char *send_endpoint, const char *recv_endpoint,
              const char *log_base_path)
{
  int nfd;
  uint32_t i;
  if ((nfd = bind_socket(port)) < 0) {
    print_error("cannot bind socket. please check port number with netstat.");
    return -1;
  }

  for (i = 0; i < nthreads; i++) {
    memset(&threads[i], 0, sizeof(threads[i]));
    threads[i].request_reopen_log_file = GRN_FALSE;
    if (!(threads[i].base = event_init())) {
      print_error("error in event_init() on thread %d.", i);
    } else {
      if (!(threads[i].httpd = evhttp_new(threads[i].base))) {
        print_error("error in evhttp_new() on thread %d.", i);
      } else {
        int r;
        if ((r = evhttp_accept_socket(threads[i].httpd, nfd))) {
          print_error("error in evhttp_accept_socket() on thread %d.", i);
        } else {
          if (send_endpoint) {
            if (!(threads[i].zmq_sock = zmq_socket(zmq_ctx, ZMQ_PUB))) {
              print_error("cannot create zmq_socket.");
            } else if (zmq_connect(threads[i].zmq_sock, send_endpoint)) {
              print_error("cannot connect zmq_socket.");
              zmq_close(threads[i].zmq_sock);
              threads[i].zmq_sock = NULL;
            } else {
              uint64_t hwm = 1;
              zmq_setsockopt(threads[i].zmq_sock, ZMQ_SNDHWM, &hwm, sizeof(uint64_t));
            }
          } else {
            threads[i].zmq_sock = NULL;
          }
          if (!(threads[i].ctx = grn_ctx_open(0))) {
            print_error("error in grn_ctx_open() on thread %d.", i);
          } else if (grn_ctx_use(threads[i].ctx, db)) {
            print_error("error in grn_db_open() on thread %d.", i);
          } else {
            GRN_TEXT_INIT(&(threads[i].cmd_buf), 0);
            GRN_TEXT_INIT(&(threads[i].pass_through_parameters), 0);
            threads[i].log_base_path = log_base_path;
            threads[i].thread_id = i;
            evhttp_set_gencb(threads[i].httpd, generic_handler, &threads[i]);
            evhttp_set_timeout(threads[i].httpd, 10);
            {
              struct timeval tv = {1, 0};
              evtimer_set(&(threads[i].pulse), timeout_handler, &threads[i]);
              evtimer_add(&(threads[i].pulse), &tv);
            }
            if ((r = pthread_create(&(threads[i].thd), NULL, dispatch, threads[i].base))) {
              print_error("error in pthread_create() on thread %d.", i);
            }
          }
        }
      }
    }
  }

  /* recv thread from learner */
  if (recv_endpoint) {
    recv_thd_data rthd;
    rthd.db_path = db_path;
    rthd.recv_endpoint = recv_endpoint;
    rthd.zmq_ctx = zmq_ctx;

    if (pthread_create(&(rthd.thd), NULL, recv_from_learner, &rthd)) {
      print_error("error in pthread_create() on thread %d.", i);
    }
    if (pthread_join(rthd.thd, NULL)) {
      print_error("error in pthread_join() on thread %d.", i);
    }
  } else {
    while (loop) { sleep(1); }
  }

  /* join all httpd thread */
  for (i = 0; i < nthreads; i++) {
    if (threads[i].thd) {
      if (pthread_join(threads[i].thd, NULL)) {
        print_error("error in pthread_join() on thread %d.", i);
      }
    }
    cleanup_httpd_thread(&(threads[i]));
  }
  return 0;
}

static uint32_t
get_core_number(void)
{
#ifdef ACTUALLY_GET_CORE_NUMBER
#ifdef _SC_NPROCESSORS_CONF
  return sysconf(_SC_NPROCESSORS_CONF);
#else /* _SC_NPROCESSORS_CONF */
  int n_processors;
  size_t length = sizeof(n_processors);
  int mib[] = {CTL_HW, HW_NCPU};
  if (sysctl(mib, sizeof(mib) / sizeof(mib[0]),
             &n_processors, &length, NULL, 0) == 0 &&
      length == sizeof(n_processors) &&
      0 < n_processors) {
    return n_processors;
  } else {
    return 1;
  }
#endif /* _SC_NPROCESSORS_CONF */
#endif /* ACTUALLY_GET_CORE_NUMBER */
  return 0;
}

static void
usage(FILE *output)
{
  fprintf(
    output,
    "Usage: groonga-suggest-httpd [options...] db_path\n"
    "db_path:\n"
    "  specify groonga database path which is used for suggestion.\n"
    "\n"
    "options:\n"
    "  -p, --port <port number>                  : http server port number\n"
    "                                              (default: %d)\n"
    /*
    "  --address <ip/hostname>                   : server address to listen\n"
    "                                              (default: %s)\n"
    */
    "  -c <thread number>                        : number of server threads\n"
    "                                              (deprecated. use --n-threads)\n"
    "  -t, --n-threads <thread number>           : number of server threads\n"
    "                                              (default: %d)\n"
    "  -s, --send-endpoint <send endpoint>       : send endpoint\n"
    "                                              (ex. tcp://example.com:1234)\n"
    "  -r, --receive-endpoint <receive endpoint> : receive endpoint\n"
    "                                              (ex. tcp://example.com:1235)\n"
    "  -l, --log-base-path <path prefix>         : log path prefix\n"
    "  --n-lines-per-log-file <lines number>     : number of lines in a log file\n"
    "                                              use 0 for disabling this\n"
    "                                              (default: %d)\n"
    "  -d, --daemon                              : daemonize\n"
    "  --disable-max-fd-check                    : disable max FD check on start\n"
    "  -h, --help                                : show this message\n",
    DEFAULT_PORT, default_max_threads, n_lines_per_log_file);
}

int
main(int argc, char **argv)
{
  int port_no = DEFAULT_PORT;
  const char *max_threads_string = NULL, *port_string = NULL;
  const char *address;
  const char *send_endpoint = NULL, *recv_endpoint = NULL, *log_base_path = NULL;
  const char *n_lines_per_log_file_string = NULL;
  int n_processed_args, flags = RUN_MODE_ENABLE_MAX_FD_CHECK;
  run_mode mode = run_mode_none;

  if (!(default_max_threads = get_core_number())) {
    default_max_threads = DEFAULT_MAX_THREADS;
  }

  /* parse options */
  {
    static grn_str_getopt_opt opts[] = {
      {'c', NULL, NULL, 0, GETOPT_OP_NONE}, /* deprecated */
      {'t', "n-threads", NULL, 0, GETOPT_OP_NONE},
      {'h', "help", NULL, run_mode_usage, GETOPT_OP_UPDATE},
      {'p', "port", NULL, 0, GETOPT_OP_NONE},
      {'\0', "bind-address", NULL, 0, GETOPT_OP_NONE}, /* not supported yet */
      {'s', "send-endpoint", NULL, 0, GETOPT_OP_NONE},
      {'r', "receive-endpoint", NULL, 0, GETOPT_OP_NONE},
      {'l', "log-base-path", NULL, 0, GETOPT_OP_NONE},
      {'\0', "n-lines-per-log-file", NULL, 0, GETOPT_OP_NONE},
      {'d', "daemon", NULL, run_mode_daemon, GETOPT_OP_UPDATE},
      {'\0', "disable-max-fd-check", NULL, RUN_MODE_ENABLE_MAX_FD_CHECK,
       GETOPT_OP_OFF},
      {'\0', NULL, NULL, 0, 0}
    };
    opts[0].arg = &max_threads_string;
    opts[1].arg = &max_threads_string;
    opts[3].arg = &port_string;
    opts[4].arg = &address;
    opts[5].arg = &send_endpoint;
    opts[6].arg = &recv_endpoint;
    opts[7].arg = &log_base_path;
    opts[8].arg = &n_lines_per_log_file_string;

    n_processed_args = grn_str_getopt(argc, argv, opts, &flags);
  }

  /* main */
  mode = (flags & RUN_MODE_MASK);
  if (n_processed_args < 0 ||
      (argc - n_processed_args) != 1 ||
      mode == run_mode_error) {
    usage(stderr);
    return EXIT_FAILURE;
  } else if (mode == run_mode_usage) {
    usage(stdout);
    return EXIT_SUCCESS;
  } else {
    grn_ctx ctx;
    void *zmq_ctx;

    if (max_threads_string) {
      max_threads = atoi(max_threads_string);
      if (max_threads > MAX_THREADS) {
        print_error("too many threads. limit to %d.", MAX_THREADS);
        max_threads = MAX_THREADS;
      }
    } else {
      max_threads = default_max_threads;
    }

    if (port_string) {
      port_no = atoi(port_string);
    }

    if (flags & RUN_MODE_ENABLE_MAX_FD_CHECK) {
      /* check environment */
      struct rlimit rlim;
      if (!getrlimit(RLIMIT_NOFILE, &rlim)) {
        if (rlim.rlim_max < MIN_MAX_FDS) {
          print_error("too small max fds. %d required.", MIN_MAX_FDS);
          return -1;
        }
        rlim.rlim_cur = rlim.rlim_cur;
        setrlimit(RLIMIT_NOFILE, &rlim);
      }
    }

    if (n_lines_per_log_file_string) {
      int64_t n_lines;
      n_lines = grn_atoll(n_lines_per_log_file_string,
                          n_lines_per_log_file_string + strlen(n_lines_per_log_file_string),
                          NULL);
      if (n_lines < 0) {
        print_error("--n-lines-per-log-file must be >= 0: <%s>",
                    n_lines_per_log_file_string);
        return(EXIT_FAILURE);
      }
      if (n_lines > UINT32_MAX) {
        print_error("--n-lines-per-log-file must be <= %ld: <%s>",
                    UINT32_MAX, n_lines_per_log_file_string);
        return(EXIT_FAILURE);
      }
      n_lines_per_log_file = (uint32_t)n_lines;
    }

    if (mode == run_mode_daemon) {
      daemonize();
    }

    grn_init();
    grn_ctx_init(&ctx, 0);
    if ((db = grn_db_open(&ctx, argv[n_processed_args]))) {
      if ((zmq_ctx = zmq_init(1))) {
        signal(SIGTERM, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGQUIT, signal_handler);
        signal(SIGUSR1, signal_reopen_log_file);

        serve_threads(max_threads, port_no, argv[n_processed_args], zmq_ctx,
                      send_endpoint, recv_endpoint, log_base_path);
        zmq_term(zmq_ctx);
      } else {
        print_error("cannot create zmq context.");
      }
      grn_obj_close(&ctx, db);
    } else {
      print_error("cannot open db.");
    }
    grn_ctx_fin(&ctx);
    grn_fin();
  }
  return 0;
}
