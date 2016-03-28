/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2012 Brazil

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

#ifndef GRN_COM_H
#define GRN_COM_H

#include "grn.h"
#include "grn_str.h"
#include "grn_hash.h"

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif /* HAVE_NETDB_H */

#ifdef __cplusplus
extern "C" {
#endif

/******* grn_com_queue ********/

typedef struct _grn_com_queue grn_com_queue;
typedef struct _grn_com_queue_entry grn_com_queue_entry;

#define GRN_COM_QUEUE_BINSIZE (0x100)

struct _grn_com_queue_entry {
  grn_obj obj;
  struct _grn_com_queue_entry *next;
};

struct _grn_com_queue {
  grn_com_queue_entry *bins[GRN_COM_QUEUE_BINSIZE];
  grn_com_queue_entry *next;
  grn_com_queue_entry **tail;
  uint8_t first;
  uint8_t last;
  grn_critical_section cs;
};

#define GRN_COM_QUEUE_INIT(q) do {\
  (q)->next = NULL;\
  (q)->tail = &(q)->next;\
  (q)->first = 0;\
  (q)->last = 0;\
  CRITICAL_SECTION_INIT((q)->cs);\
} while (0)

#define GRN_COM_QUEUE_EMPTYP(q) (((q)->first == (q)->last) && !(q)->next)

GRN_API grn_rc grn_com_queue_enque(grn_ctx *ctx, grn_com_queue *q, grn_com_queue_entry *e);
GRN_API grn_com_queue_entry *grn_com_queue_deque(grn_ctx *ctx, grn_com_queue *q);

/******* grn_com ********/

#ifdef USE_SELECT
# ifdef HAVE_SYS_SELECT_H
#  include <sys/select.h>
# endif /* HAVE_SYS_SELECT_H */
# define GRN_COM_POLLIN  1
# define GRN_COM_POLLOUT 2
#else /* USE_SELECT */
# ifdef USE_EPOLL
#  include <sys/epoll.h>
#  define GRN_COM_POLLIN  EPOLLIN
#  define GRN_COM_POLLOUT EPOLLOUT
# else /* USE_EPOLL */
#  ifdef USE_KQUEUE
#   include <sys/event.h>
#   define GRN_COM_POLLIN  EVFILT_READ
#   define GRN_COM_POLLOUT EVFILT_WRITE
#  else /* USE_KQUEUE */
#    if defined(HAVE_POLL_H)
#      include <poll.h>
#    elif defined(HAVE_SYS_POLL_H)
#      include <sys/poll.h>
#    endif /* defined(HAVE_POLL_H) */
#   define GRN_COM_POLLIN  POLLIN
#   define GRN_COM_POLLOUT POLLOUT
#  endif /* USE_KQUEUE */
# endif /* USE_EPOLL */
#endif /* USE_SELECT */

typedef struct _grn_com grn_com;
typedef struct _grn_com_event grn_com_event;
typedef struct _grn_com_addr grn_com_addr;
typedef void grn_com_callback(grn_ctx *ctx, grn_com_event *, grn_com *);
typedef void grn_msg_handler(grn_ctx *ctx, grn_obj *msg);

enum {
  grn_com_ok = 0,
  grn_com_emem,
  grn_com_erecv_head,
  grn_com_erecv_body,
  grn_com_eproto,
};

struct _grn_com_addr {
  uint32_t addr;
  uint16_t port;
  uint16_t sid;
};

struct _grn_com {
  grn_sock fd;
  int events;
  uint16_t sid;
  uint8_t has_sid;
  uint8_t closed;
  grn_com_queue new_;
  grn_com_event *ev;
  void *opaque;
  grn_bool accepting;
};

struct _grn_com_event {
  struct _grn_hash *hash;
  int max_nevents;
  grn_ctx *ctx;
  grn_mutex mutex;
  grn_cond cond;
  grn_com_queue recv_old;
  grn_msg_handler *msg_handler;
  grn_com_addr curr_edge_id;
  grn_com *acceptor;
  void *opaque;
#ifndef USE_SELECT
#ifdef USE_EPOLL
  int epfd;
  struct epoll_event *events;
#else /* USE_EPOLL */
#ifdef USE_KQUEUE
  int kqfd;
  struct kevent *events;
#else /* USE_KQUEUE */
  int dummy; /* dummy */
  struct pollfd *events;
#endif /* USE_KQUEUE */
#endif /* USE_EPOLL */
#endif /* USE_SELECT */
};

grn_rc grn_com_init(void);
void grn_com_fin(void);
GRN_API grn_rc grn_com_event_init(grn_ctx *ctx, grn_com_event *ev, int max_nevents, int data_size);
GRN_API grn_rc grn_com_event_fin(grn_ctx *ctx, grn_com_event *ev);
GRN_API grn_rc grn_com_event_start_accept(grn_ctx *ctx, grn_com_event *ev);
grn_rc grn_com_event_stop_accept(grn_ctx *ctx, grn_com_event *ev);
grn_rc grn_com_event_add(grn_ctx *ctx, grn_com_event *ev, grn_sock fd, int events, grn_com **com);
grn_rc grn_com_event_mod(grn_ctx *ctx, grn_com_event *ev, grn_sock fd, int events, grn_com **com);
GRN_API grn_rc grn_com_event_del(grn_ctx *ctx, grn_com_event *ev, grn_sock fd);
GRN_API grn_rc grn_com_event_poll(grn_ctx *ctx, grn_com_event *ev, int timeout);
grn_rc grn_com_event_each(grn_ctx *ctx, grn_com_event *ev, grn_com_callback *func);

/******* grn_com_gqtp ********/

#define GRN_COM_PROTO_HTTP   0x47
#define GRN_COM_PROTO_GQTP   0xc7
#define GRN_COM_PROTO_MBREQ  0x80
#define GRN_COM_PROTO_MBRES  0x81

typedef struct _grn_com_header grn_com_header;

struct _grn_com_header {
  uint8_t proto;
  uint8_t qtype;
  uint16_t keylen;
  uint8_t level;
  uint8_t flags;
  uint16_t status;
  uint32_t size;
  uint32_t opaque;
  uint64_t cas;
};

GRN_API grn_com *grn_com_copen(grn_ctx *ctx, grn_com_event *ev, const char *dest, int port);
GRN_API grn_rc grn_com_sopen(grn_ctx *ctx, grn_com_event *ev,
                             const char *bind_address, int port,
                             grn_msg_handler *func, struct hostent *he);

GRN_API void grn_com_close_(grn_ctx *ctx, grn_com *com);
GRN_API grn_rc grn_com_close(grn_ctx *ctx, grn_com *com);

GRN_API grn_rc grn_com_send(grn_ctx *ctx, grn_com *cs,
                            grn_com_header *header, const char *body, uint32_t size, int flags);
grn_rc grn_com_recv(grn_ctx *ctx, grn_com *cs, grn_com_header *header, grn_obj *buf);
GRN_API grn_rc grn_com_send_http(grn_ctx *ctx, grn_com *cs, const char *path, uint32_t path_len, int flags);

/******* grn_msg ********/

typedef struct _grn_msg grn_msg;

struct _grn_msg {
  grn_com_queue_entry qe;
  union {
    grn_com *peer;
    grn_sock fd;
  } u;
  grn_ctx *ctx;
  grn_com_queue *old;
  grn_com_header header;
  grn_com_addr edge_id;
  grn_com *acceptor;
};

GRN_API grn_rc grn_msg_send(grn_ctx *ctx, grn_obj *msg, int flags);
GRN_API grn_obj *grn_msg_open_for_reply(grn_ctx *ctx, grn_obj *query, grn_com_queue *old);
GRN_API grn_obj *grn_msg_open(grn_ctx *ctx, grn_com *com, grn_com_queue *old);
GRN_API grn_rc grn_msg_set_property(grn_ctx *ctx, grn_obj *obj,
                                    uint16_t status, uint32_t key_size, uint8_t extra_size);
GRN_API grn_rc grn_msg_close(grn_ctx *ctx, grn_obj *msg);

/******* grn_edge ********/

#define GRN_EDGE_WORKER       0
#define GRN_EDGE_COMMUNICATOR 1

typedef struct {
  grn_com_queue_entry eq;
  grn_ctx ctx;
  grn_com_queue recv_new;
  grn_com_queue send_old;
  grn_com *com;
  grn_com_addr *addr;
  grn_msg *msg;
  uint8_t stat;
  uint8_t flags;
  grn_id id;
} grn_edge;

GRN_VAR grn_hash *grn_edges;
GRN_API void grn_edges_init(grn_ctx *ctx, void (*dispatcher)(grn_ctx *ctx, grn_edge *edge));
GRN_API void grn_edges_fin(grn_ctx *ctx);
GRN_API grn_edge *grn_edges_add(grn_ctx *ctx, grn_com_addr *addr, int *added);
grn_edge *grn_edges_add_communicator(grn_ctx *ctx, grn_com_addr *addr);
GRN_API void grn_edges_delete(grn_ctx *ctx, grn_edge *edge);
void grn_edge_dispatch(grn_ctx *ctx, grn_edge *edge, grn_obj *msg);

#ifdef __cplusplus
}
#endif

#endif /* GRN_COM_H */
