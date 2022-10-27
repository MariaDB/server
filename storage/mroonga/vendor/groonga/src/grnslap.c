/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2012 Brazil

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

#include <grn_com.h>
#include <grn_ctx_impl.h>
#include <string.h>
#include <stdio.h>
#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */
#ifndef WIN32
# include <netinet/in.h>
#endif /* WIN32 */

#define DEFAULT_PORT 10041
#define DEFAULT_HOST "localhost"
#define DEFAULT_MAX_CONCURRENCY 10
#define DEFAULT_MAX_THROUGHPUT 10000
#define MAX_DEST 256

typedef struct {
  const char *host;
  uint16_t port;
} grn_slap_dest;

static int proto = 'g';
static int verbose = 0;
static int dest_cnt = 0;
static grn_slap_dest dests[MAX_DEST];
static int max_con = DEFAULT_MAX_CONCURRENCY;
static int max_tp = DEFAULT_MAX_THROUGHPUT;

#include <stdarg.h>
static void
lprint(grn_ctx *ctx, const char *fmt, ...)
{
  char buf[1024];
  grn_timeval tv;
  int len;
  va_list argp;
  grn_timeval_now(ctx, &tv);
  grn_timeval2str(ctx, &tv, buf, 1024);
  len = strlen(buf);
  buf[len++] = '|';
  va_start(argp, fmt);
  vsnprintf(buf + len, 1023 - len, fmt, argp);
  va_end(argp);
  buf[1023] = '\0';
  puts(buf);
}

static void
parse_dest(char *deststr, grn_slap_dest *dest)
{
  int p;
  char *d;
  if ((d = strchr(deststr, ':'))) {
    if ((p = atoi(d + 1))) {
      *d = '\0';
      dest->host = deststr;
      dest->port = p;
      return;
    }
  }
  dest->host = NULL;
  dest->port = 0;
}

static void
usage(void)
{
  fprintf(stderr,
          "Usage: grnslap [options...] [dest...]\n"
          "options:\n"
          "  -P <protocol>:      http or gqtp (default: gqtp)\n"
          "  -m <max concurrency>:   number of max concurrency (default: %d)\n"
          "dest: hostname:port number (default: \"%s:%d\")\n",
          DEFAULT_MAX_CONCURRENCY, DEFAULT_HOST, DEFAULT_PORT);
}

#define BUFSIZE 0x1000000

typedef struct _session session;

struct _session {
  grn_com_queue_entry eq;
  grn_com *com;
  struct timeval tv;
  grn_id id;
  int stat;
  int query_id;
  int n_query;
  int n_sessions;
};

static grn_com_event ev;
static grn_com_queue fsessions;
static grn_hash *sessions;
static int done = 0;
static int nsent = 0;
static int nrecv = 0;
static int etime_min = INT32_MAX;
static int etime_max = 0;
static int64_t etime_amount = 0;

static session *
session_open(grn_ctx *ctx, grn_slap_dest *dest)
{
  grn_id id;
  session *s;
  grn_com *com;
  if (!(com = grn_com_copen(ctx, &ev, dest->host, dest->port))) { return NULL; }
  id = grn_hash_add(ctx, sessions, &com->fd, sizeof(grn_sock), (void **)&s, NULL);
  com->opaque = s;
  s->com = com;
  s->id = id;
  s->stat = 1;
  return s;
}

static void
session_close(grn_ctx *ctx, session *s)
{
  if (!s->stat) { return; }
  grn_com_close(ctx, s->com);
  s->stat = 0;
  grn_hash_delete_by_id(ctx, sessions, s->id, NULL);
}

static session *
session_alloc(grn_ctx *ctx, grn_slap_dest *dest)
{
  session *s;
  while ((s = (session *)grn_com_queue_deque(ctx, &fsessions))) {
    if (s->n_query < 1000000 && !s->com->closed) { return s; }
    //session_close(ctx, s);
  }
  return session_open(ctx, dest);
}

static void
msg_handler(grn_ctx *ctx, grn_obj *msg)
{
  uint32_t etime;
  struct timeval tv;
  grn_msg *m = (grn_msg *)msg;
  grn_com *com = ((grn_msg *)msg)->u.peer;
  session *s = com->opaque;
  s->stat = 3;
  gettimeofday(&tv, NULL);
  etime = (tv.tv_sec - s->tv.tv_sec) * 1000000 + (tv.tv_usec - s->tv.tv_usec);
  if (etime > etime_max) { etime_max = etime; }
  if (etime < etime_min) { etime_min = etime; }
  if (ctx->rc) { m->header.proto = 0; }
  switch (m->header.proto) {
  case GRN_COM_PROTO_GQTP :
    if (GRN_BULK_VSIZE(msg) == 2) {
      etime_amount += etime;
    } else {
      if (verbose) {
        GRN_TEXT_PUTC(ctx, msg, '\0');
        lprint(ctx, "%8d(%4d) %8d : %s", s->query_id, s->n_sessions, etime, GRN_BULK_HEAD(msg));
      }
    }
    if ((m->header.flags & GRN_CTX_TAIL)) {
      grn_com_queue_enque(ctx, &fsessions, (grn_com_queue_entry *)s);
      nrecv++;
    }
    break;
  case GRN_COM_PROTO_HTTP :
    nrecv++;
    /* lprint(ctx, "recv: %d, %d", (int)GRN_BULK_VSIZE(msg), nrecv); */
    grn_com_close_(ctx, com);
    grn_com_queue_enque(ctx, &fsessions, (grn_com_queue_entry *)s);
    break;
  default :
    grn_com_close_(ctx, com);
    grn_com_queue_enque(ctx, &fsessions, (grn_com_queue_entry *)s);
    break;
  }
  grn_msg_close(ctx, msg);
}

static grn_thread_func_result CALLBACK
receiver(void *arg)
{
  grn_ctx ctx_, *ctx = &ctx_;
  grn_ctx_init(ctx, 0);
  while (!grn_com_event_poll(ctx, &ev, 100)) {
    if (nsent == nrecv && done) { break; }
    /*
    {
      session *s;
      GRN_HASH_EACH(ctx, sessions, id, NULL, NULL, &s, {
          printf("id=%d: fd=%d stat=%d q=%d n=%d\n", s->id, s->com->fd, s->stat, s->query_id, s->n_query);
      });
    }
    */
  }
  grn_ctx_fin(ctx);
  return GRN_THREAD_FUNC_RETURN_VALUE;
}

static int
do_client()
{
  int rc = -1;
  grn_obj text;
  grn_thread thread;
  struct timeval tvb, tve;
  grn_com_header sheader;
  grn_ctx ctx_, *ctx = &ctx_;
  grn_ctx_init(ctx, 0);
  GRN_COM_QUEUE_INIT(&fsessions);
  sessions = grn_hash_create(ctx, NULL, sizeof(grn_sock), sizeof(session), 0);
  sheader.proto = GRN_COM_PROTO_GQTP;
  sheader.qtype = 0;
  sheader.keylen = 0;
  sheader.level = 0;
  sheader.flags = 0;
  sheader.status = 0;
  sheader.opaque = 0;
  sheader.cas = 0;
  GRN_TEXT_INIT(&text, 0);
  rc = grn_bulk_reserve(ctx, &text, BUFSIZE);
  if (!rc) {
    char *buf = GRN_TEXT_VALUE(&text);
    if (!grn_com_event_init(ctx, &ev, 1000, sizeof(grn_com))) {
      ev.msg_handler = msg_handler;
      if (!THREAD_CREATE(thread, receiver, NULL)) {
        int cnt = 0;
        gettimeofday(&tvb, NULL);
        lprint(ctx, "begin: procotol=%c max_concurrency=%d max_tp=%d", proto, max_con, max_tp);
        while (fgets(buf, BUFSIZE, stdin)) {
          uint32_t size = strlen(buf) - 1;
          session *s = session_alloc(ctx, dests + (cnt++ % dest_cnt));
          if (s) {
            gettimeofday(&s->tv, NULL);
            s->n_query++;
            s->query_id = ++nsent;
            s->n_sessions = (nsent - nrecv);
            switch (proto) {
            case 'H' :
            case 'h' :
              if (grn_com_send_http(ctx, s->com, buf, size, 0)) {
                fprintf(stderr, "grn_com_send_http failed\n");
              }
              s->stat = 2;
              /*
              lprint(ctx, "sent %04d %04d %d",
                     s->n_query, s->query_id, s->com->fd);
              */
              break;
            default :
              if (grn_com_send(ctx, s->com, &sheader, buf, size, 0)) {
                fprintf(stderr, "grn_com_send failed\n");
              }
              break;
            }
          } else {
            fprintf(stderr, "grn_com_copen failed\n");
          }
          for (;;) {
            gettimeofday(&tve, NULL);
            if ((nrecv < max_tp * (tve.tv_sec - tvb.tv_sec)) &&
                (nsent - nrecv) < max_con) { break; }
            /* lprint(ctx, "s:%d r:%d", nsent, nrecv); */
            grn_nanosleep(1000000);
          }
          if (!(nsent % 1000)) { lprint(ctx, "     : %d", nsent); }
        }
        done = 1;
        if (THREAD_JOIN(thread)) {
          fprintf(stderr, "THREAD_JOIN failed\n");
        }
        gettimeofday(&tve, NULL);
        {
          double qps;
          uint64_t etime = (tve.tv_sec - tvb.tv_sec);
          etime *= 1000000;
          etime += (tve.tv_usec - tvb.tv_usec);
          qps = (double)nsent * 1000000 / etime;
          lprint(ctx, "end  : n=%d min=%d max=%d avg=%d qps=%f etime=%d.%06d", nsent, etime_min, etime_max, (int)(etime_amount / nsent), qps, etime / 1000000, etime % 1000000);
        }
        {
          session *s;
          GRN_HASH_EACH(ctx, sessions, id, NULL, NULL, &s, {
            session_close(ctx, s);
          });
        }
        rc = 0;
      } else {
        fprintf(stderr, "THREAD_CREATE failed\n");
      }
      grn_com_event_fin(ctx, &ev);
    } else {
      fprintf(stderr, "grn_com_event_init failed\n");
    }
  }
  grn_obj_unlink(ctx, &text);
  grn_hash_close(ctx, sessions);
  grn_ctx_fin(ctx);
  return rc;
}

enum {
  flag_usage = 1,
  flag_verbose = 2
};

int
main(int argc, char **argv)
{
  const char *protostr = NULL, *maxconstr = NULL, *maxtpstr = NULL;
  int r, i, flags = 0;
  static grn_str_getopt_opt opts[] = {
    {'P', NULL, NULL, 0, GETOPT_OP_NONE},
    {'m', NULL, NULL, 0, GETOPT_OP_NONE},
    {'t', NULL, NULL, 0, GETOPT_OP_NONE},
    {'h', NULL, NULL, flag_usage, GETOPT_OP_ON},
    {'v', NULL, NULL, flag_verbose, GETOPT_OP_ON},
    {'\0', NULL, NULL, 0, 0}
  };
  opts[0].arg = &protostr;
  opts[1].arg = &maxconstr;
  opts[2].arg = &maxtpstr;
  i = grn_str_getopt(argc, argv, opts, &flags);
  if (protostr) { proto = *protostr; }
  if (maxconstr) { max_con = atoi(maxconstr); }
  if (maxtpstr) { max_tp = atoi(maxtpstr); }
  if (flags & flag_verbose) { verbose = 1; }

  if (argc <= i) {
    dests[0].host = DEFAULT_HOST;
    dests[0].port = DEFAULT_PORT;
    dest_cnt = 1;
  } else if (i > 0 && argc <= (i + MAX_DEST)){
    for (dest_cnt = 0; i < argc; i++) {
      parse_dest(argv[i], &dests[dest_cnt]);
      if (dests[dest_cnt].host) {
        dest_cnt++;
      }
    }
    if (!dest_cnt) { flags |= flag_usage; }
  } else {
    /* too much dests */
    flags |= flag_usage;
  }

  grn_default_logger_set_path(GRN_LOG_PATH);

  if (grn_init()) { return -1; }
  if (flags & flag_usage) {
    usage(); r = -1;
  } else {
    r = do_client();
  }
  grn_fin();
  return r;
}
