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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "grn.h"

#include <stdio.h>
#include <string.h>
#include "grn_ctx_impl.h"

#ifdef WIN32
# include <ws2tcpip.h>
#else
# ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
# endif /* HAVE_SYS_SOCKET_H */
# include <netinet/in.h>
# include <netinet/tcp.h>
# ifdef HAVE_SIGNAL_H
#  include <signal.h>
# endif /* HAVE_SIGNAL_H */
# include <sys/uio.h>
#endif /* WIN32 */

#include "grn_ctx.h"
#include "grn_com.h"

#ifndef PF_INET
#define PF_INET AF_INET
#endif /* PF_INET */

#ifndef SOL_TCP
#  ifdef IPPROTO_TCP
#    define SOL_TCP IPPROTO_TCP
#  else
#    define SOL_TCP 6
#  endif /* IPPROTO_TCP */
#endif /* SOL_TCP */

#ifndef USE_MSG_MORE
#  ifdef MSG_MORE
#    undef MSG_MORE
#  endif
#  define MSG_MORE     0
#endif /* USE_MSG_MORE */


#ifndef USE_MSG_NOSIGNAL
#  ifdef MSG_NOSIGNAL
#    undef MSG_NOSIGNAL
#  endif
#  define MSG_NOSIGNAL 0
#endif /* USE_MSG_NOSIGNAL */
/******* grn_com_queue ********/

grn_rc
grn_com_queue_enque(grn_ctx *ctx, grn_com_queue *q, grn_com_queue_entry *e)
{
  CRITICAL_SECTION_ENTER(q->cs);
  e->next = NULL;
  *q->tail = e;
  q->tail = &e->next;
  CRITICAL_SECTION_LEAVE(q->cs);
  /*
  uint8_t i = q->last + 1;
  e->next = NULL;
  if (q->first == i || q->next) {
    CRITICAL_SECTION_ENTER(q->cs);
    if (q->first == i || q->next) {
      *q->tail = e;
      q->tail = &e->next;
    } else {
      q->bins[q->last] = e;
      q->last = i;
    }
    CRITICAL_SECTION_LEAVE(q->cs);
  } else {
    q->bins[q->last] = e;
    q->last = i;
  }
  */
  return GRN_SUCCESS;
}

grn_com_queue_entry *
grn_com_queue_deque(grn_ctx *ctx, grn_com_queue *q)
{
  grn_com_queue_entry *e = NULL;

  CRITICAL_SECTION_ENTER(q->cs);
  if (q->next) {
    e = q->next;
    if (!(q->next = e->next)) { q->tail = &q->next; }
  }
  CRITICAL_SECTION_LEAVE(q->cs);

  /*
  if (q->first == q->last) {
    if (q->next) {
      CRITICAL_SECTION_ENTER(q->cs);
      e = q->next;
      if (!(q->next = e->next)) { q->tail = &q->next; }
      CRITICAL_SECTION_LEAVE(q->cs);
    }
  } else {
    e = q->bins[q->first++];
  }
  */
  return e;
}

/******* grn_msg ********/

grn_obj *
grn_msg_open(grn_ctx *ctx, grn_com *com, grn_com_queue *old)
{
  grn_msg *msg = NULL;
  if (old && (msg = (grn_msg *)grn_com_queue_deque(ctx, old))) {
    if (msg->ctx != ctx) {
      ERR(GRN_INVALID_ARGUMENT, "ctx unmatch");
      return NULL;
    }
    GRN_BULK_REWIND(&msg->qe.obj);
  } else if ((msg = GRN_MALLOCN(grn_msg, 1))) {
    GRN_OBJ_INIT(&msg->qe.obj, GRN_MSG, 0, GRN_DB_TEXT);
    msg->qe.obj.header.impl_flags |= GRN_OBJ_ALLOCATED;
    msg->ctx = ctx;
  }
  msg->qe.next = NULL;
  msg->u.peer = com;
  msg->old = old;
  memset(&msg->header, 0, sizeof(grn_com_header));
  return (grn_obj *)msg;
}

grn_obj *
grn_msg_open_for_reply(grn_ctx *ctx, grn_obj *query, grn_com_queue *old)
{
  grn_msg *req = (grn_msg *)query, *msg = NULL;
  if (req && (msg = (grn_msg *)grn_msg_open(ctx, req->u.peer, old))) {
    msg->edge_id = req->edge_id;
    msg->header.proto = req->header.proto == GRN_COM_PROTO_MBREQ
      ? GRN_COM_PROTO_MBRES : req->header.proto;
  }
  return (grn_obj *)msg;
}

grn_rc
grn_msg_close(grn_ctx *ctx, grn_obj *obj)
{
  grn_msg *msg = (grn_msg *)obj;
  if (ctx == msg->ctx) { return grn_obj_close(ctx, obj); }
  return grn_com_queue_enque(ctx, msg->old, (grn_com_queue_entry *)msg);
}

grn_rc
grn_msg_set_property(grn_ctx *ctx, grn_obj *obj,
                     uint16_t status, uint32_t key_size, uint8_t extra_size)
{
  grn_com_header *header = &((grn_msg *)obj)->header;
  header->status = htons(status);
  header->keylen = htons(key_size);
  header->level = extra_size;
  return GRN_SUCCESS;
}

grn_rc
grn_msg_send(grn_ctx *ctx, grn_obj *msg, int flags)
{
  grn_rc rc;
  grn_msg *m = (grn_msg *)msg;
  grn_com *peer = m->u.peer;
  grn_com_header *header = &m->header;
  if (GRN_COM_QUEUE_EMPTYP(&peer->new_)) {
    switch (header->proto) {
    case GRN_COM_PROTO_HTTP :
      {
        ssize_t ret;
        ret = send(peer->fd, GRN_BULK_HEAD(msg), GRN_BULK_VSIZE(msg), MSG_NOSIGNAL);
        if (ret == -1) { SOERR("send"); }
        if (ctx->rc != GRN_OPERATION_WOULD_BLOCK) {
          grn_com_queue_enque(ctx, m->old, (grn_com_queue_entry *)msg);
          return ctx->rc;
        }
      }
      break;
    case GRN_COM_PROTO_GQTP :
      {
        if (flags & GRN_CTX_MORE) { flags |= GRN_CTX_QUIET; }
        if (ctx->stat == GRN_CTX_QUIT) { flags |= GRN_CTX_QUIT; }
        header->qtype = (uint8_t) ctx->impl->output.type;
        header->keylen = 0;
        header->level = 0;
        header->flags = flags;
        header->status = htons((uint16_t)ctx->rc);
        header->opaque = 0;
        header->cas = 0;
        //todo : MSG_DONTWAIT
        rc = grn_com_send(ctx, peer, header,
                          GRN_BULK_HEAD(msg), GRN_BULK_VSIZE(msg), 0);
        if (rc != GRN_OPERATION_WOULD_BLOCK) {
          grn_com_queue_enque(ctx, m->old, (grn_com_queue_entry *)msg);
          return rc;
        }
      }
      break;
    case GRN_COM_PROTO_MBREQ :
      return GRN_FUNCTION_NOT_IMPLEMENTED;
    case GRN_COM_PROTO_MBRES :
      rc = grn_com_send(ctx, peer, header,
                        GRN_BULK_HEAD(msg), GRN_BULK_VSIZE(msg),
                        (flags & GRN_CTX_MORE) ? MSG_MORE :0);
      if (rc != GRN_OPERATION_WOULD_BLOCK) {
        grn_com_queue_enque(ctx, m->old, (grn_com_queue_entry *)msg);
        return rc;
      }
      break;
    default :
      return GRN_INVALID_ARGUMENT;
    }
  }
  MUTEX_LOCK(peer->ev->mutex);
  rc = grn_com_queue_enque(ctx, &peer->new_, (grn_com_queue_entry *)msg);
  COND_SIGNAL(peer->ev->cond);
  MUTEX_UNLOCK(peer->ev->mutex);
  return rc;
}

/******* grn_com ********/

grn_rc
grn_com_init(void)
{
#ifdef WIN32
  WSADATA wd;
  if (WSAStartup(MAKEWORD(2, 0), &wd) != 0) {
    grn_ctx *ctx = &grn_gctx;
    SOERR("WSAStartup");
  }
#else /* WIN32 */
#ifndef USE_MSG_NOSIGNAL
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    grn_ctx *ctx = &grn_gctx;
    SERR("signal");
  }
#endif /* USE_MSG_NOSIGNAL */
#endif /* WIN32 */
  return grn_gctx.rc;
}

void
grn_com_fin(void)
{
#ifdef WIN32
  WSACleanup();
#endif /* WIN32 */
}

grn_rc
grn_com_event_init(grn_ctx *ctx, grn_com_event *ev, int max_nevents, int data_size)
{
  ev->max_nevents = max_nevents;
  if ((ev->hash = grn_hash_create(ctx, NULL, sizeof(grn_sock), data_size, 0))) {
    MUTEX_INIT(ev->mutex);
    COND_INIT(ev->cond);
    GRN_COM_QUEUE_INIT(&ev->recv_old);
    ev->msg_handler = NULL;
    memset(&(ev->curr_edge_id), 0, sizeof(grn_com_addr));
    ev->acceptor = NULL;
    ev->opaque = NULL;
#ifndef USE_SELECT
# ifdef USE_EPOLL
    if ((ev->events = GRN_MALLOC(sizeof(struct epoll_event) * max_nevents))) {
      if ((ev->epfd = epoll_create(max_nevents)) != -1) {
        goto exit;
      } else {
        SERR("epoll_create");
      }
      GRN_FREE(ev->events);
    }
# else /* USE_EPOLL */
#  ifdef USE_KQUEUE
    if ((ev->events = GRN_MALLOC(sizeof(struct kevent) * max_nevents))) {
      if ((ev->kqfd = kqueue()) != -1) {
        goto exit;
      } else {
        SERR("kqueue");
      }
      GRN_FREE(ev->events);
    }
#  else /* USE_KQUEUE */
    if ((ev->events = GRN_MALLOC(sizeof(struct pollfd) * max_nevents))) {
      goto exit;
    }
#  endif /* USE_KQUEUE*/
# endif /* USE_EPOLL */
    grn_hash_close(ctx, ev->hash);
    ev->hash = NULL;
    ev->events = NULL;
#else /* USE_SELECT */
    goto exit;
#endif /* USE_SELECT */
  }
exit :
  return ctx->rc;
}

grn_rc
grn_com_event_fin(grn_ctx *ctx, grn_com_event *ev)
{
  grn_obj *msg;
  while ((msg = (grn_obj *)grn_com_queue_deque(ctx, &ev->recv_old))) {
    grn_msg_close(ctx, msg);
  }
  if (ev->hash) { grn_hash_close(ctx, ev->hash); }
#ifndef USE_SELECT
  if (ev->events) { GRN_FREE(ev->events); }
# ifdef USE_EPOLL
  grn_close(ev->epfd);
# endif /* USE_EPOLL */
# ifdef USE_KQUEUE
  grn_close(ev->kqfd);
# endif /* USE_KQUEUE*/
#endif /* USE_SELECT */
  return GRN_SUCCESS;
}

grn_rc
grn_com_event_add(grn_ctx *ctx, grn_com_event *ev, grn_sock fd, int events, grn_com **com)
{
  grn_com *c;
  /* todo : expand events */
  if (!ev || *ev->hash->n_entries == ev->max_nevents) {
    if (ev) { GRN_LOG(ctx, GRN_LOG_ERROR, "too many connections (%d)", ev->max_nevents); }
    return GRN_INVALID_ARGUMENT;
  }
#ifdef USE_EPOLL
  {
    struct epoll_event e;
    memset(&e, 0, sizeof(struct epoll_event));
    e.data.fd = (fd);
    e.events = (uint32_t) events;
    if (epoll_ctl(ev->epfd, EPOLL_CTL_ADD, (fd), &e) == -1) {
      SERR("epoll_ctl");
      return ctx->rc;
    }
  }
#endif /* USE_EPOLL*/
#ifdef USE_KQUEUE
  {
    struct kevent e;
    /* todo: udata should have fd */
    EV_SET(&e, (fd), events, EV_ADD, 0, 0, NULL);
    if (kevent(ev->kqfd, &e, 1, NULL, 0, NULL) == -1) {
      SERR("kevent");
      return ctx->rc;
    }
  }
#endif /* USE_KQUEUE */
  {
    if (grn_hash_add(ctx, ev->hash, &fd, sizeof(grn_sock), (void **)&c, NULL)) {
      c->ev = ev;
      c->fd = fd;
      c->events = events;
      if (com) { *com = c; }
    }
  }
  return ctx->rc;
}

grn_rc
grn_com_event_mod(grn_ctx *ctx, grn_com_event *ev, grn_sock fd, int events, grn_com **com)
{
  grn_com *c;
  if (!ev) { return GRN_INVALID_ARGUMENT; }
  if (grn_hash_get(ctx, ev->hash, &fd, sizeof(grn_sock), (void **)&c)) {
    if (c->fd != fd) {
      GRN_LOG(ctx, GRN_LOG_ERROR,
              "grn_com_event_mod fd unmatch "
              "%" GRN_FMT_SOCKET " != %" GRN_FMT_SOCKET,
              c->fd, fd);
      return GRN_OBJECT_CORRUPT;
    }
    if (com) { *com = c; }
    if (c->events != events) {
#ifdef USE_EPOLL
      struct epoll_event e;
      memset(&e, 0, sizeof(struct epoll_event));
      e.data.fd = (fd);
      e.events = (uint32_t) events;
      if (epoll_ctl(ev->epfd, EPOLL_CTL_MOD, (fd), &e) == -1) {
        SERR("epoll_ctl");
        return ctx->rc;
      }
#endif /* USE_EPOLL*/
#ifdef USE_KQUEUE
      // experimental
      struct kevent e[2];
      EV_SET(&e[0], (fd), GRN_COM_POLLIN|GRN_COM_POLLOUT, EV_DELETE, 0, 0, NULL);
      EV_SET(&e[1], (fd), events, EV_ADD, 0, 0, NULL);
      if (kevent(ev->kqfd, e, 2, NULL, 0, NULL) == -1) {
        SERR("kevent");
        return ctx->rc;
      }
#endif /* USE_KQUEUE */
      c->events = events;
    }
    return GRN_SUCCESS;
  }
  return GRN_INVALID_ARGUMENT;
}

grn_rc
grn_com_event_del(grn_ctx *ctx, grn_com_event *ev, grn_sock fd)
{
  if (!ev) { return GRN_INVALID_ARGUMENT; }
  {
    grn_com *c;
    grn_id id = grn_hash_get(ctx, ev->hash, &fd, sizeof(grn_sock), (void **)&c);
    if (id) {
#ifdef USE_EPOLL
      if (!c->closed) {
        struct epoll_event e;
        memset(&e, 0, sizeof(struct epoll_event));
        e.data.fd = fd;
        e.events = c->events;
        if (epoll_ctl(ev->epfd, EPOLL_CTL_DEL, fd, &e) == -1) {
          SERR("epoll_ctl");
          return ctx->rc;
        }
      }
#endif /* USE_EPOLL*/
#ifdef USE_KQUEUE
      struct kevent e;
      EV_SET(&e, (fd), c->events, EV_DELETE, 0, 0, NULL);
      if (kevent(ev->kqfd, &e, 1, NULL, 0, NULL) == -1) {
        SERR("kevent");
        return ctx->rc;
      }
#endif /* USE_KQUEUE */
      return grn_hash_delete_by_id(ctx, ev->hash, id, NULL);
    } else {
      GRN_LOG(ctx, GRN_LOG_ERROR,
              "%04x| fd(%" GRN_FMT_SOCKET ") not found in ev(%p)",
              grn_getpid(), fd, ev);
      return GRN_INVALID_ARGUMENT;
    }
  }
}

#define LISTEN_BACKLOG 0x1000

grn_rc
grn_com_event_start_accept(grn_ctx *ctx, grn_com_event *ev)
{
  grn_com *com = ev->acceptor;

  if (com->accepting) {return ctx->rc;}

  GRN_API_ENTER;
  if (!grn_com_event_mod(ctx, ev, com->fd, GRN_COM_POLLIN, NULL)) {
    if (listen(com->fd, LISTEN_BACKLOG) == 0) {
      com->accepting = GRN_TRUE;
    } else {
      SOERR("listen - start accept");
    }
  }
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_com_event_stop_accept(grn_ctx *ctx, grn_com_event *ev)
{
  grn_com *com = ev->acceptor;

  if (!com->accepting) {return ctx->rc;}

  GRN_API_ENTER;
  if (!grn_com_event_mod(ctx, ev, com->fd, 0, NULL)) {
    if (listen(com->fd, 0) == 0) {
      com->accepting = GRN_FALSE;
    } else {
      SOERR("listen - disable accept");
    }
  }
  GRN_API_RETURN(ctx->rc);
}

static void
grn_com_receiver(grn_ctx *ctx, grn_com *com)
{
  grn_com_event *ev = com->ev;
  ERRCLR(ctx);
  if (ev->acceptor == com) {
    grn_com *ncs;
    grn_sock fd = accept(com->fd, NULL, NULL);
    if (fd == -1) {
      if (errno == EMFILE) {
        grn_com_event_stop_accept(ctx, ev);
      } else {
        SOERR("accept");
      }
      return;
    }
    if (grn_com_event_add(ctx, ev, fd, GRN_COM_POLLIN, (grn_com **)&ncs)) {
      grn_sock_close(fd);
      return;
    }
    ncs->has_sid = 0;
    ncs->closed = 0;
    ncs->opaque = NULL;
    GRN_COM_QUEUE_INIT(&ncs->new_);
    // GRN_LOG(ctx, GRN_LOG_NOTICE, "accepted (%d)", fd);
    return;
  } else {
    grn_msg *msg = (grn_msg *)grn_msg_open(ctx, com, &ev->recv_old);
    grn_com_recv(ctx, msg->u.peer, &msg->header, (grn_obj *)msg);
    if (msg->u.peer /* is_edge_request(msg)*/) {
      grn_memcpy(&msg->edge_id, &ev->curr_edge_id, sizeof(grn_com_addr));
      if (!com->has_sid) {
        com->has_sid = 1;
        com->sid = ev->curr_edge_id.sid++;
      }
      msg->edge_id.sid = com->sid;
    }
    msg->acceptor = ev->acceptor;
    ev->msg_handler(ctx, (grn_obj *)msg);
  }
}

grn_rc
grn_com_event_poll(grn_ctx *ctx, grn_com_event *ev, int timeout)
{
  int nevents;
  grn_com *com;
#ifdef USE_SELECT
  uint32_t dummy;
  grn_sock *pfd;
  int nfds = 0;
  fd_set rfds;
  fd_set wfds;
  struct timeval tv;
  if (timeout >= 0) {
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
  }
  FD_ZERO(&rfds);
  FD_ZERO(&wfds);
  ctx->errlvl = GRN_OK;
  ctx->rc = GRN_SUCCESS;
  {
    grn_hash_cursor *cursor;
    cursor = grn_hash_cursor_open(ctx, ev->hash, NULL, 0, NULL, 0, 0, -1, 0);
    if (cursor) {
      grn_id id;
      while ((id = grn_hash_cursor_next(ctx, cursor))) {
        grn_hash_cursor_get_key_value(ctx,
                                      cursor,
                                      (void **)(&pfd),
                                      &dummy,
                                      (void **)(&com));
        if (com->events & GRN_COM_POLLIN) { FD_SET(*pfd, &rfds); }
        if (com->events & GRN_COM_POLLOUT) { FD_SET(*pfd, &wfds); }
# ifndef WIN32
        if (*pfd > nfds) { nfds = *pfd; }
# endif /* WIN32 */
      }
      grn_hash_cursor_close(ctx, cursor);
    }
  }
  nevents = select(nfds + 1, &rfds, &wfds, NULL, (timeout >= 0) ? &tv : NULL);
  if (nevents < 0) {
    SOERR("select");
    if (ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) { ERRCLR(ctx); }
    return ctx->rc;
  }
  if (timeout < 0 && !nevents) { GRN_LOG(ctx, GRN_LOG_NOTICE, "select returns 0 events"); }
  GRN_HASH_EACH(ctx, ev->hash, eh, &pfd, &dummy, &com, {
    if (FD_ISSET(*pfd, &rfds)) { grn_com_receiver(ctx, com); }
  });
#else /* USE_SELECT */
# ifdef USE_EPOLL
  struct epoll_event *ep;
  ctx->errlvl = GRN_OK;
  ctx->rc = GRN_SUCCESS;
  nevents = epoll_wait(ev->epfd, ev->events, ev->max_nevents, timeout);
  if (nevents < 0) {
    SERR("epoll_wait");
  }
# else /* USE_EPOLL */
#  ifdef USE_KQUEUE
  struct kevent *ep;
  struct timespec tv;
  if (timeout >= 0) {
    tv.tv_sec = timeout / 1000;
    tv.tv_nsec = (timeout % 1000) * 1000;
  }
  nevents = kevent(ev->kqfd, NULL, 0, ev->events, ev->max_nevents, &tv);
  if (nevents < 0) {
    SERR("kevent");
  }
#  else /* USE_KQUEUE */
  uint32_t dummy;
  int nfd = 0, *pfd;
  struct pollfd *ep = ev->events;
  ctx->errlvl = GRN_OK;
  ctx->rc = GRN_SUCCESS;
  GRN_HASH_EACH(ctx, ev->hash, eh, &pfd, &dummy, &com, {
    ep->fd = *pfd;
    //    ep->events =(short) com->events;
    ep->events = POLLIN;
    ep->revents = 0;
    ep++;
    nfd++;
  });
  nevents = poll(ev->events, nfd, timeout);
  if (nevents < 0) {
    SERR("poll");
  }
#  endif /* USE_KQUEUE */
# endif /* USE_EPOLL */
  if (ctx->rc != GRN_SUCCESS) {
    if (ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) {
      ERRCLR(ctx);
    }
    return ctx->rc;
  }
  if (timeout < 0 && !nevents) { GRN_LOG(ctx, GRN_LOG_NOTICE, "poll returns 0 events"); }
  for (ep = ev->events; nevents; ep++) {
    int efd;
# ifdef USE_EPOLL
    efd = ep->data.fd;
    nevents--;
    // todo : com = ep->data.ptr;
    if (!grn_hash_get(ctx, ev->hash, &efd, sizeof(grn_sock), (void *)&com)) {
      struct epoll_event e;
      GRN_LOG(ctx, GRN_LOG_ERROR, "fd(%d) not found in ev->hash", efd);
      memset(&e, 0, sizeof(struct epoll_event));
      e.data.fd = efd;
      e.events = ep->events;
      if (epoll_ctl(ev->epfd, EPOLL_CTL_DEL, efd, &e) == -1) { SERR("epoll_ctl"); }
      if (grn_sock_close(efd) == -1) { SOERR("close"); }
      continue;
    }
    if (ep->events & GRN_COM_POLLIN) { grn_com_receiver(ctx, com); }
# else /* USE_EPOLL */
#  ifdef USE_KQUEUE
    efd = ep->ident;
    nevents--;
    // todo : com = ep->udata;
    if (!grn_hash_get(ctx, ev->hash, &efd, sizeof(grn_sock), (void *)&com)) {
      struct kevent e;
      GRN_LOG(ctx, GRN_LOG_ERROR, "fd(%d) not found in ev->set", efd);
      EV_SET(&e, efd, ep->filter, EV_DELETE, 0, 0, NULL);
      if (kevent(ev->kqfd, &e, 1, NULL, 0, NULL) == -1) { SERR("kevent"); }
      if (grn_sock_close(efd) == -1) { SOERR("close"); }
      continue;
    }
    if (ep->filter == GRN_COM_POLLIN) { grn_com_receiver(ctx, com); }
#  else
    efd = ep->fd;
    if (!(ep->events & ep->revents)) { continue; }
    nevents--;
    if (!grn_hash_get(ctx, ev->hash, &efd, sizeof(grn_sock), (void *)&com)) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "fd(%d) not found in ev->hash", efd);
      if (grn_sock_close(efd) == -1) { SOERR("close"); }
      continue;
    }
    if (ep->revents & GRN_COM_POLLIN) { grn_com_receiver(ctx, com); }
#  endif /* USE_KQUEUE */
# endif /* USE_EPOLL */
  }
#endif /* USE_SELECT */
  /* todo :
  while (!(msg = (grn_com_msg *)grn_com_queue_deque(&recv_old))) {
    grn_msg_close(ctx, msg);
  }
  */
  return GRN_SUCCESS;
}

grn_rc
grn_com_send_http(grn_ctx *ctx, grn_com *cs, const char *path, uint32_t path_len, int flags)
{
  ssize_t ret;
  grn_obj buf;
  GRN_TEXT_INIT(&buf, 0);
  GRN_TEXT_PUTS(ctx, &buf, "GET ");
  grn_bulk_write(ctx, &buf, path, path_len);
  GRN_TEXT_PUTS(ctx, &buf, " HTTP/1.0\r\n\r\n");
  // todo : refine
  if ((ret = send(cs->fd, GRN_BULK_HEAD(&buf), GRN_BULK_VSIZE(&buf), MSG_NOSIGNAL|flags)) == -1) {
    SOERR("send");
  }
  if (ret != GRN_BULK_VSIZE(&buf)) {
    GRN_LOG(ctx, GRN_LOG_NOTICE, "send %d != %d", (int)ret, (int)GRN_BULK_VSIZE(&buf));
  }
  grn_obj_close(ctx, &buf);
  return ctx->rc;
}

grn_rc
grn_com_send(grn_ctx *ctx, grn_com *cs,
             grn_com_header *header, const char *body, uint32_t size, int flags)
{
  grn_rc rc = GRN_SUCCESS;
  size_t whole_size = sizeof(grn_com_header) + size;
  ssize_t ret;
  header->size = htonl(size);
  GRN_LOG(ctx, GRN_LOG_INFO, "send (%d,%x,%d,%02x,%02x,%04x)", size, header->flags, header->proto, header->qtype, header->level, header->status);

  if (size) {
#ifdef WIN32
    WSABUF wsabufs[2];
    DWORD n_sent;
    wsabufs[0].buf = (char *)header;
    wsabufs[0].len = sizeof(grn_com_header);
    wsabufs[1].buf = (char *)body;
    wsabufs[1].len = size;
    if (WSASend(cs->fd, wsabufs, 2, &n_sent, 0, NULL, NULL) == SOCKET_ERROR) {
      SOERR("WSASend");
    }
    ret = n_sent;
#else /* WIN32 */
    struct iovec msg_iov[2];
    struct msghdr msg;
    memset(&msg, 0, sizeof(struct msghdr));
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = msg_iov;
    msg.msg_iovlen = 2;
    msg_iov[0].iov_base = (char*) header;
    msg_iov[0].iov_len = sizeof(grn_com_header);
    msg_iov[1].iov_base = (char *)body;
    msg_iov[1].iov_len = size;
    if ((ret = sendmsg(cs->fd, &msg, MSG_NOSIGNAL|flags)) == -1) {
      SOERR("sendmsg");
      rc = ctx->rc;
    }
#endif /* WIN32 */
  } else {
    if ((ret = send(cs->fd, (const void *)header, whole_size, MSG_NOSIGNAL|flags)) == -1) {
      SOERR("send");
      rc = ctx->rc;
    }
  }
  if (ret != whole_size) {
    GRN_LOG(ctx, GRN_LOG_ERROR,
            "sendmsg(%" GRN_FMT_SOCKET "): %" GRN_FMT_LLD " < %" GRN_FMT_LLU,
            cs->fd, (long long int)ret, (unsigned long long int)whole_size);
    rc = ctx->rc;
  }
  return rc;
}

#define RETRY_MAX 10

static const char *
scan_delimiter(const char *p, const char *e)
{
  while (p + 4 <= e) {
    if (p[3] == '\n') {
      if (p[2] == '\r') {
        if (p[1] == '\n') {
          if (p[0] == '\r') { return p + 4; } else { p += 2; }
        } else { p += 2; }
      } else { p += 4; }
    } else { p += p[3] == '\r' ? 1 : 4; }
  }
  return NULL;
}

#define BUFSIZE 4096

static grn_rc
grn_com_recv_text(grn_ctx *ctx, grn_com *com,
                  grn_com_header *header, grn_obj *buf, ssize_t ret)
{
  const char *p;
  int retry = 0;
  grn_bulk_write(ctx, buf, (char *)header, ret);
  if ((p = scan_delimiter(GRN_BULK_HEAD(buf), GRN_BULK_CURR(buf)))) {
    header->qtype = *GRN_BULK_HEAD(buf);
    header->proto = GRN_COM_PROTO_HTTP;
    header->size = GRN_BULK_VSIZE(buf);
    goto exit;
  }
  for (;;) {
    if (grn_bulk_reserve(ctx, buf, BUFSIZE)) { return ctx->rc; }
    if ((ret = recv(com->fd, GRN_BULK_CURR(buf), BUFSIZE, 0)) < 0) {
      SOERR("recv text");
      if (ctx->rc == GRN_OPERATION_WOULD_BLOCK ||
          ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) {
        ERRCLR(ctx);
        continue;
      }
      goto exit;
    }
    if (ret) {
      off_t o = GRN_BULK_VSIZE(buf);
      p = GRN_BULK_CURR(buf);
      GRN_BULK_INCR_LEN(buf, ret);
      if (scan_delimiter(p - (o > 3 ? 3 : o), p + ret)) {
        break;
      }
    } else {
      if (++retry > RETRY_MAX) {
        // ERR(GRN_RETRY_MAX, "retry max in recv text");
        goto exit;
      }
    }
  }
  header->qtype = *GRN_BULK_HEAD(buf);
  header->proto = GRN_COM_PROTO_HTTP;
  header->size = GRN_BULK_VSIZE(buf);
exit :
  if (header->qtype == 'H') {
    //todo : refine
    /*
    GRN_BULK_REWIND(buf);
    grn_bulk_reserve(ctx, buf, BUFSIZE);
    if ((ret = recv(com->fd, GRN_BULK_CURR(buf), BUFSIZE, 0)) < 0) {
      SOERR("recv text body");
    } else {
      GRN_BULK_CURR(buf) += ret;
    }
    */
  }
  return ctx->rc;
}

grn_rc
grn_com_recv(grn_ctx *ctx, grn_com *com, grn_com_header *header, grn_obj *buf)
{
  ssize_t ret;
  int retry = 0;
  byte *p = (byte *)header;
  size_t rest = sizeof(grn_com_header);
  do {
    if ((ret = recv(com->fd, p, rest, 0)) < 0) {
      SOERR("recv size");
      GRN_LOG(ctx, GRN_LOG_ERROR, "recv error (%" GRN_FMT_SOCKET ")", com->fd);
      if (ctx->rc == GRN_OPERATION_WOULD_BLOCK ||
          ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) {
        ERRCLR(ctx);
        continue;
      }
      goto exit;
    }
    if (ret) {
      if (header->proto < 0x80) {
        return grn_com_recv_text(ctx, com, header, buf, ret);
      }
      rest -= ret, p += ret;
    } else {
      if (++retry > RETRY_MAX) {
        // ERR(GRN_RETRY_MAX, "retry max in recv header (%d)", com->fd);
        goto exit;
      }
    }
  } while (rest);
  GRN_LOG(ctx, GRN_LOG_INFO,
          "recv (%u,%x,%d,%02x,%02x,%04x)",
          (uint32_t)ntohl(header->size),
          header->flags,
          header->proto,
          header->qtype,
          header->level,
          header->status);
  {
    uint8_t proto = header->proto;
    size_t value_size = ntohl(header->size);
    GRN_BULK_REWIND(buf);
    switch (proto) {
    case GRN_COM_PROTO_GQTP :
    case GRN_COM_PROTO_MBREQ :
      if (GRN_BULK_WSIZE(buf) < value_size) {
        if (grn_bulk_resize(ctx, buf, value_size)) {
          goto exit;
        }
      }
      retry = 0;
      for (rest = value_size; rest;) {
        if ((ret = recv(com->fd, GRN_BULK_CURR(buf), rest, MSG_WAITALL)) < 0) {
          SOERR("recv body");
          if (ctx->rc == GRN_OPERATION_WOULD_BLOCK ||
              ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) {
            ERRCLR(ctx);
            continue;
          }
          goto exit;
        }
        if (ret) {
          rest -= ret;
          GRN_BULK_INCR_LEN(buf, ret);
        } else {
          if (++retry > RETRY_MAX) {
            // ERR(GRN_RETRY_MAX, "retry max in recv body");
            goto exit;
          }
        }
      }
      break;
    default :
      GRN_LOG(ctx, GRN_LOG_ERROR, "illegal header: %d", proto);
      ctx->rc = GRN_INVALID_FORMAT;
      goto exit;
    }
  }
exit :
  return ctx->rc;
}

grn_com *
grn_com_copen(grn_ctx *ctx, grn_com_event *ev, const char *dest, int port)
{
  grn_sock fd = -1;
  grn_com *cs = NULL;

  struct addrinfo hints, *addrinfo_list, *addrinfo_ptr;
  char port_string[16];
  int getaddrinfo_result;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
#ifdef AI_NUMERICSERV
  hints.ai_flags = AI_NUMERICSERV;
#endif
  grn_snprintf(port_string, sizeof(port_string), sizeof(port_string),
               "%d", port);

  getaddrinfo_result = getaddrinfo(dest, port_string, &hints, &addrinfo_list);
  if (getaddrinfo_result != 0) {
    switch (getaddrinfo_result) {
#ifdef EAI_MEMORY
    case EAI_MEMORY:
      ERR(GRN_NO_MEMORY_AVAILABLE, "getaddrinfo: <%s:%s>: %s",
          dest, port_string, gai_strerror(getaddrinfo_result));
      break;
#endif
#ifdef EAI_SYSTEM
    case EAI_SYSTEM:
      SOERR("getaddrinfo");
      break;
#endif
    default:
      ERR(GRN_INVALID_ARGUMENT, "getaddrinfo: <%s:%s>: %s",
          dest, port_string, gai_strerror(getaddrinfo_result));
      break;
    }
    return NULL;
  }

  for (addrinfo_ptr = addrinfo_list; addrinfo_ptr;
       addrinfo_ptr = addrinfo_ptr->ai_next) {
    fd = socket(addrinfo_ptr->ai_family, addrinfo_ptr->ai_socktype,
                addrinfo_ptr->ai_protocol);
    if (fd == -1) {
      SOERR("socket");
      continue;
    }
#ifdef TCP_NODELAY
    {
      static const int value = 1;
      if (setsockopt(fd, 6, TCP_NODELAY,
                     (const char *)&value, sizeof(value)) != 0) {
        SOERR("setsockopt");
        grn_sock_close(fd);
        continue;
      }
    }
#endif
    if (connect(fd, addrinfo_ptr->ai_addr, addrinfo_ptr->ai_addrlen) != 0) {
      SOERR("connect");
      grn_sock_close(fd);
      continue;
    }

    break;
  }

  freeaddrinfo(addrinfo_list);

  if (!addrinfo_ptr) {
    return NULL;
  }
  ctx->errlvl = GRN_OK;
  ctx->rc = GRN_SUCCESS;

  if (ev) {
    grn_com_event_add(ctx, ev, fd, GRN_COM_POLLIN, &cs);
  } else {
    cs = GRN_CALLOC(sizeof(grn_com));
    if (cs) {
      cs->fd = fd;
    }
  }
  if (!cs) {
    grn_sock_close(fd);
  }
  return cs;
}

void
grn_com_close_(grn_ctx *ctx, grn_com *com)
{
  grn_sock fd = com->fd;
  if (shutdown(fd, SHUT_RDWR) == -1) { /* SOERR("shutdown"); */ }
  if (grn_sock_close(fd) == -1) {
    SOERR("close");
  } else {
    com->closed = 1;
  }
}

grn_rc
grn_com_close(grn_ctx *ctx, grn_com *com)
{
  grn_sock fd = com->fd;
  grn_com_event *ev = com->ev;
  if (ev) {
    grn_com *acceptor = ev->acceptor;
    grn_com_event_del(ctx, ev, fd);
    if (acceptor) { grn_com_event_start_accept(ctx, ev); }
  }
  if (!com->closed) { grn_com_close_(ctx, com); }
  if (!ev) { GRN_FREE(com); }
  return GRN_SUCCESS;
}

grn_rc
grn_com_sopen(grn_ctx *ctx, grn_com_event *ev,
              const char *bind_address, int port, grn_msg_handler *func,
              struct hostent *he)
{
  grn_sock lfd = -1;
  grn_com *cs = NULL;
  int getaddrinfo_result;
  struct addrinfo *bind_address_info = NULL;
  struct addrinfo hints;
  char port_string[6]; /* ceil(log10(65535)) + 1 ('\0')*/

  GRN_API_ENTER;
  if (!bind_address) {
    bind_address = "0.0.0.0";
  }
  grn_snprintf(port_string, sizeof(port_string), sizeof(port_string),
               "%d", port);
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = PF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
#ifdef AI_NUMERICSERV
  hints.ai_flags = AI_NUMERICSERV;
#endif
  getaddrinfo_result = getaddrinfo(bind_address, port_string,
                                   &hints, &bind_address_info);
  if (getaddrinfo_result != 0) {
    switch (getaddrinfo_result) {
#ifdef EAI_MEMORY
    case EAI_MEMORY:
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "getaddrinfo: <%s:%s>: %s",
          bind_address, port_string, gai_strerror(getaddrinfo_result));
      break;
#endif
#ifdef EAI_SYSTEM
    case EAI_SYSTEM:
      SOERR("getaddrinfo");
      break;
#endif
    default:
      ERR(GRN_INVALID_ARGUMENT,
          "getaddrinfo: <%s:%s>: %s",
          bind_address, port_string, gai_strerror(getaddrinfo_result));
      break;
    }
    goto exit;
  }
  if ((lfd = socket(bind_address_info->ai_family, SOCK_STREAM, 0)) == -1) {
    SOERR("socket");
    goto exit;
  }
  grn_memcpy(&ev->curr_edge_id.addr, he->h_addr, he->h_length);
  ev->curr_edge_id.port = htons(port);
  ev->curr_edge_id.sid = 0;
  {
    int v = 1;
#ifdef TCP_NODELAY
    if (setsockopt(lfd, SOL_TCP, TCP_NODELAY, (void *) &v, sizeof(int)) == -1) {
      SOERR("setsockopt");
      goto exit;
    }
#endif
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (void *) &v, sizeof(int)) == -1) {
      SOERR("setsockopt");
      goto exit;
    }
  }
  if (bind(lfd, bind_address_info->ai_addr, bind_address_info->ai_addrlen) < 0) {
    SOERR("bind");
    goto exit;
  }
  if (listen(lfd, LISTEN_BACKLOG) < 0) {
    SOERR("listen");
    goto exit;
  }
  if (ev) {
    if (grn_com_event_add(ctx, ev, lfd, GRN_COM_POLLIN, &cs)) { goto exit; }
    ev->acceptor = cs;
    ev->msg_handler = func;
    cs->has_sid = 0;
    cs->closed = 0;
    cs->opaque = NULL;
    GRN_COM_QUEUE_INIT(&cs->new_);
  } else {
    if (!(cs = GRN_MALLOC(sizeof(grn_com)))) { goto exit; }
    cs->fd = lfd;
  }
  cs->accepting = GRN_TRUE;
exit :
  if (!cs && lfd != 1) { grn_sock_close(lfd); }
  if (bind_address_info) { freeaddrinfo(bind_address_info); }
  GRN_API_RETURN(ctx->rc);
}


grn_hash *grn_edges = NULL;
void (*grn_dispatcher)(grn_ctx *ctx, grn_edge *edge);

void
grn_edges_init(grn_ctx *ctx, void (*dispatcher)(grn_ctx *ctx, grn_edge *edge))
{
  grn_edges = grn_hash_create(ctx, NULL, sizeof(grn_com_addr), sizeof(grn_edge), 0);
  grn_dispatcher = dispatcher;
}

void
grn_edges_fin(grn_ctx *ctx)
{
  grn_hash_close(ctx, grn_edges);
}

grn_edge *
grn_edges_add(grn_ctx *ctx, grn_com_addr *addr, int *added)
{
  if (grn_io_lock(ctx, grn_edges->io, grn_lock_timeout)) {
    return NULL;
  } else {
    grn_edge *edge;
    grn_id id = grn_hash_add(ctx, grn_edges, addr, sizeof(grn_com_addr),
                             (void **)&edge, added);
    grn_io_unlock(grn_edges->io);
    if (id) { edge->id = id; }
    return edge;
  }
}

void
grn_edges_delete(grn_ctx *ctx, grn_edge *edge)
{
  if (!grn_io_lock(ctx, grn_edges->io, grn_lock_timeout)) {
    grn_hash_delete_by_id(ctx, grn_edges, edge->id, NULL);
    grn_io_unlock(grn_edges->io);
  }
}

grn_edge *
grn_edges_add_communicator(grn_ctx *ctx, grn_com_addr *addr)
{
  int added;
  grn_edge *edge = grn_edges_add(ctx, addr, &added);
  if (added) {
    grn_ctx_init(&edge->ctx, 0);
    GRN_COM_QUEUE_INIT(&edge->recv_new);
    GRN_COM_QUEUE_INIT(&edge->send_old);
    edge->com = NULL;
    edge->stat = 0 /*EDGE_IDLE*/;
    edge->flags = GRN_EDGE_COMMUNICATOR;
  }
  return edge;
}

void
grn_edge_dispatch(grn_ctx *ctx, grn_edge *edge, grn_obj *msg)
{
  grn_com_queue_enque(ctx, &edge->recv_new, (grn_com_queue_entry *)msg);
  grn_dispatcher(ctx, edge);
}
