/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2015 Brazil

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

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>

#ifdef WIN32
# define GROONGA_MAIN
#endif /* WIN32 */
#include <grn.h>

#include <grn_com.h>
#include <grn_ctx_impl.h>
#include <grn_proc.h>
#include <grn_db.h>
#include <grn_util.h>

#ifdef HAVE_SYS_WAIT_H
# include <sys/wait.h>
#endif /* HAVE_SYS_WAIT_H */
#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif /* HAVE_SYS_SOCKET_H */
#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif /* HAVE_NETINET_IN_H */

#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif /* HAVE_SYS_RESOURCE_H */

#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif /* HAVE_SYS_SYSCTL_H */

#ifdef HAVE_IO_H
# include <io.h>
#endif /* HAVE_IO_H */

#ifdef HAVE__STRNICMP
# ifdef strncasecmp
#  undef strncasecmp
# endif /* strcasecmp */
# define strncasecmp(s1,s2,n) _strnicmp(s1,s2,n)
#endif /* HAVE__STRNICMP */

#ifndef USE_MSG_NOSIGNAL
# ifdef MSG_NOSIGNAL
#  undef MSG_NOSIGNAL
# endif
# define MSG_NOSIGNAL 0
#endif /* USE_MSG_NOSIGNAL */

#ifndef STDIN_FILENO
# define STDIN_FILENO 0
#endif /* STDIN_FILENO */
#ifndef STDOUT_FILENO
# define STDOUT_FILENO 1
#endif /* STDOUT_FILENO */
#ifndef STDERR_FILENO
# define STDERR_FILENO 2
#endif /* STDERR_FILENO */

#define DEFAULT_HTTP_PORT 10041
#define DEFAULT_GQTP_PORT 10043
#define DEFAULT_DEST "localhost"
#define DEFAULT_MAX_NFTHREADS 8
#define MAX_CON 0x10000

#define RLIMIT_NOFILE_MINIMUM 4096

static char bind_address[HOST_NAME_MAX + 1];
static char hostname[HOST_NAME_MAX + 1];
static int port = DEFAULT_GQTP_PORT;
static int batchmode;
static int number_of_lines = 0;
static int newdb;
static grn_bool is_daemon_mode = GRN_FALSE;
static int (*do_client)(int argc, char **argv);
static int (*do_server)(char *path);
static const char *pid_file_path = NULL;
static const char *input_path = NULL;
static FILE *output = NULL;

static int ready_notify_pipe[2];
#define PIPE_READ  0
#define PIPE_WRITE 1

static grn_encoding encoding;
static grn_command_version default_command_version;
static int64_t default_match_escalation_threshold;
static int log_level;

static int
grn_rc_to_exit_code(grn_rc rc)
{
  if (rc == GRN_SUCCESS) {
    return EXIT_SUCCESS;
  } else {
    return EXIT_FAILURE;
  }
}

#ifdef GRN_WITH_LIBEDIT
#include <locale.h>
#include <histedit.h>
static EditLine   *line_editor = NULL;
static HistoryW   *line_editor_history = NULL;
static HistEventW line_editor_history_event;
static char       line_editor_history_path[PATH_MAX] = "";

static const wchar_t *
line_editor_prompt(EditLine *e __attribute__((unused)))
{
  return L"> ";
}
static const wchar_t * const line_editor_editor = L"emacs";

static void
line_editor_init(int argc __attribute__((unused)), char *argv[])
{
  const char * const HOME_PATH = getenv("HOME");
  const char * const HISTORY_PATH = "/.groonga-history";

  setlocale(LC_ALL, "");

  if (strlen(HOME_PATH) + strlen(HISTORY_PATH) < PATH_MAX) {
    strcpy(line_editor_history_path, HOME_PATH);
    strcat(line_editor_history_path, HISTORY_PATH);
  } else {
    line_editor_history_path[0] = '\0';
  }

  line_editor_history = history_winit();
  history_w(line_editor_history, &line_editor_history_event, H_SETSIZE, 200);
  if (line_editor_history_path[0]) {
    history_w(line_editor_history, &line_editor_history_event,
              H_LOAD, line_editor_history_path);
  }

  line_editor = el_init(argv[0], stdin, stdout, stderr);
  el_wset(line_editor, EL_PROMPT, &line_editor_prompt);
  el_wset(line_editor, EL_EDITOR, line_editor_editor);
  el_wset(line_editor, EL_HIST, history_w, line_editor_history);
  el_source(line_editor, NULL);
}

static void
line_editor_fin(void)
{
  if (line_editor) {
    el_end(line_editor);
    if (line_editor_history) {
      if (line_editor_history_path[0]) {
        history_w(line_editor_history, &line_editor_history_event,
                  H_SAVE, line_editor_history_path);
      }
      history_wend(line_editor_history);
    }
  }
}

static grn_rc
line_editor_fgets(grn_ctx *ctx, grn_obj *buf)
{
  grn_rc rc = GRN_SUCCESS;
  const wchar_t *line;
  int nchar;
  line = el_wgets(line_editor, &nchar);
  if (nchar > 0) {
    int i;
    char multibyte_buf[MB_CUR_MAX];
    size_t multibyte_len;
    mbstate_t ps;
    history_w(line_editor_history, &line_editor_history_event, H_ENTER, line);
    memset(&ps, 0, sizeof(ps));
    wcrtomb(NULL, L'\0', &ps);
    for (i = 0; i < nchar; i++) {
      multibyte_len = wcrtomb(multibyte_buf, line[i], &ps);
      if (multibyte_len == (size_t)-1) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "[prompt][libedit] failed to read input: %s", strerror(errno));
        rc = GRN_INVALID_ARGUMENT;
      } else {
        GRN_TEXT_PUT(ctx, buf, multibyte_buf, multibyte_len);
      }
    }
  } else {
    rc = GRN_END_OF_DATA;
  }
  return rc;
}
#endif /* GRN_WITH_LIBEDIT */

inline static grn_rc
read_next_line(grn_ctx *ctx, grn_obj *buf)
{
  static int the_first_read = GRN_TRUE;
  grn_rc rc = GRN_SUCCESS;
  if (!batchmode) {
#ifdef GRN_WITH_LIBEDIT
    rc = line_editor_fgets(ctx, buf);
#else
    fprintf(stderr, "> ");
    fflush(stderr);
    rc = grn_text_fgets(ctx, buf, stdin);
#endif
  } else {
    rc = grn_text_fgets(ctx, buf, stdin);
    if (rc != GRN_END_OF_DATA) {
      number_of_lines++;
    }
  }
  if (the_first_read && GRN_TEXT_LEN(buf) > 0) {
    const char bom[] = {0xef, 0xbb, 0xbf};
    if (GRN_CTX_GET_ENCODING(ctx) == GRN_ENC_UTF8 &&
        GRN_TEXT_LEN(buf) > 3 && !memcmp(GRN_TEXT_VALUE(buf), bom, 3)) {
      grn_obj buf_without_bom;
      GRN_TEXT_INIT(&buf_without_bom, 0);
      GRN_TEXT_PUT(ctx, &buf_without_bom,
                   GRN_TEXT_VALUE(buf) + 3, GRN_TEXT_LEN(buf) - 3);
      GRN_TEXT_SET(ctx, buf,
                   GRN_TEXT_VALUE(&buf_without_bom),
                   GRN_TEXT_LEN(&buf_without_bom));
      grn_obj_unlink(ctx, &buf_without_bom);
    }
    the_first_read = GRN_FALSE;
  }
  if (GRN_TEXT_LEN(buf) > 0 &&
      GRN_TEXT_VALUE(buf)[GRN_TEXT_LEN(buf) - 1] == '\n') {
    grn_bulk_truncate(ctx, buf, GRN_TEXT_LEN(buf) - 1);
  }
  if (GRN_TEXT_LEN(buf) > 0 &&
      GRN_TEXT_VALUE(buf)[GRN_TEXT_LEN(buf) - 1] == '\r') {
    grn_bulk_truncate(ctx, buf, GRN_TEXT_LEN(buf) - 1);
  }
  return rc;
}

inline static grn_rc
prompt(grn_ctx *ctx, grn_obj *buf)
{
  grn_rc rc = GRN_SUCCESS;
  grn_bool need_next_line = GRN_TRUE;
  GRN_BULK_REWIND(buf);
  while (need_next_line) {
    rc = read_next_line(ctx, buf);
    if (rc == GRN_SUCCESS &&
        GRN_TEXT_LEN(buf) > 0 &&
        GRN_TEXT_VALUE(buf)[GRN_TEXT_LEN(buf) - 1] == '\\') {
      grn_bulk_truncate(ctx, buf, GRN_TEXT_LEN(buf) - 1);
      need_next_line = GRN_TRUE;
    } else {
      need_next_line = GRN_FALSE;
    }
  }
  return rc;
}

static void
output_envelope(grn_ctx *ctx, grn_rc rc, grn_obj *head, grn_obj *body, grn_obj *foot)
{
  grn_output_envelope(ctx, rc, head, body, foot, input_path, number_of_lines);
}

static void
s_output_raw(grn_ctx *ctx, int flags, FILE *stream)
{
  char *chunk = NULL;
  unsigned int chunk_size = 0;
  int recv_flags;

  grn_ctx_recv(ctx, &chunk, &chunk_size, &recv_flags);
  if (chunk_size > 0) {
    fwrite(chunk, 1, chunk_size, stream);
  }

  if (flags & GRN_CTX_TAIL) {
    grn_obj *command;

    fflush(stream);

    command = GRN_CTX_USER_DATA(ctx)->ptr;
    GRN_BULK_REWIND(command);
  }
}

static void
s_output_typed(grn_ctx *ctx, int flags, FILE *stream)
{
  if (ctx && ctx->impl && (flags & GRN_CTX_TAIL)) {
    char *chunk = NULL;
    unsigned int chunk_size = 0;
    int recv_flags;
    grn_obj body;
    grn_obj *command;

    GRN_TEXT_INIT(&body, 0);
    grn_ctx_recv(ctx, &chunk, &chunk_size, &recv_flags);
    GRN_TEXT_SET(ctx, &body, chunk, chunk_size);

    if (GRN_TEXT_LEN(&body) || ctx->rc) {
      grn_obj head, foot;
      GRN_TEXT_INIT(&head, 0);
      GRN_TEXT_INIT(&foot, 0);
      output_envelope(ctx, ctx->rc, &head, &body, &foot);
      fwrite(GRN_TEXT_VALUE(&head), 1, GRN_TEXT_LEN(&head), stream);
      fwrite(GRN_TEXT_VALUE(&body), 1, GRN_TEXT_LEN(&body), stream);
      fwrite(GRN_TEXT_VALUE(&foot), 1, GRN_TEXT_LEN(&foot), stream);
      fputc('\n', stream);
      fflush(stream);
      GRN_OBJ_FIN(ctx, &head);
      GRN_OBJ_FIN(ctx, &foot);
    }
    GRN_OBJ_FIN(ctx, &body);

    command = GRN_CTX_USER_DATA(ctx)->ptr;
    GRN_BULK_REWIND(command);
  }
}

static void
s_output(grn_ctx *ctx, int flags, void *arg)
{
  FILE *stream = (FILE *)arg;

  if (grn_ctx_get_output_type(ctx) == GRN_CONTENT_NONE) {
    s_output_raw(ctx, flags, stream);
  } else {
    s_output_typed(ctx, flags, stream);
  }
}

static int
do_alone(int argc, char **argv)
{
  int exit_code = EXIT_FAILURE;
  char *path = NULL;
  grn_obj *db;
  grn_ctx ctx_, *ctx = &ctx_;
  grn_ctx_init(ctx, 0);
  if (argc > 0 && argv) { path = *argv++; argc--; }
  db = (newdb || !path) ? grn_db_create(ctx, path, NULL) : grn_db_open(ctx, path);
  if (db) {
    grn_obj command;
    GRN_TEXT_INIT(&command, 0);
    GRN_CTX_USER_DATA(ctx)->ptr = &command;
    grn_ctx_recv_handler_set(ctx, s_output, output);
    if (!argc) {
      grn_obj text;
      GRN_TEXT_INIT(&text, 0);
      while (prompt(ctx, &text) != GRN_END_OF_DATA) {
        GRN_TEXT_PUT(ctx, &command, GRN_TEXT_VALUE(&text), GRN_TEXT_LEN(&text));
        grn_ctx_send(ctx, GRN_TEXT_VALUE(&text), GRN_TEXT_LEN(&text), 0);
        if (ctx->stat == GRN_CTX_QUIT) { break; }
      }
      exit_code = grn_rc_to_exit_code(ctx->rc);
      grn_obj_unlink(ctx, &text);
    } else {
      grn_rc rc;
      rc = grn_ctx_sendv(ctx, argc, argv, 0);
      exit_code = grn_rc_to_exit_code(rc);
    }
    grn_obj_unlink(ctx, &command);
    grn_obj_close(ctx, db);
  } else {
    fprintf(stderr, "db open failed (%s): %s\n", path, ctx->errbuf);
  }
  grn_ctx_fin(ctx);
  return exit_code;
}

static int
c_output(grn_ctx *ctx)
{
  int flags;
  char *str;
  unsigned int str_len;
  do {
    grn_ctx_recv(ctx, &str, &str_len, &flags);
    /*
    if (ctx->rc) {
      fprintf(stderr, "grn_ctx_recv failed\n");
      return -1;
    }
    */
    if (str_len || ctx->rc) {
      grn_obj head, body, foot;
      GRN_TEXT_INIT(&head, 0);
      GRN_TEXT_INIT(&body, GRN_OBJ_DO_SHALLOW_COPY);
      GRN_TEXT_INIT(&foot, 0);
      if (ctx->rc == GRN_SUCCESS) {
        GRN_TEXT_SET(ctx, &body, str, str_len);
      } else {
        ERR(ctx->rc, "%.*s", str_len, str);
      }
      output_envelope(ctx, ctx->rc, &head, &body, &foot);
      fwrite(GRN_TEXT_VALUE(&head), 1, GRN_TEXT_LEN(&head), output);
      fwrite(GRN_TEXT_VALUE(&body), 1, GRN_TEXT_LEN(&body), output);
      fwrite(GRN_TEXT_VALUE(&foot), 1, GRN_TEXT_LEN(&foot), output);
      fputc('\n', output);
      fflush(output);
      GRN_OBJ_FIN(ctx, &head);
      GRN_OBJ_FIN(ctx, &body);
      GRN_OBJ_FIN(ctx, &foot);
    }
  } while ((flags & GRN_CTX_MORE));
  return 0;
}

static int
g_client(int argc, char **argv)
{
  int exit_code = EXIT_FAILURE;
  grn_ctx ctx_, *ctx = &ctx_;
  const char *hostname = DEFAULT_DEST;
  if (argc > 0 && argv) { hostname = *argv++; argc--; }
  grn_ctx_init(ctx, 0);
  if (!grn_ctx_connect(ctx, hostname, port, 0)) {
    if (!argc) {
      grn_obj text;
      GRN_TEXT_INIT(&text, 0);
      while (prompt(ctx, &text) != GRN_END_OF_DATA) {
        grn_ctx_send(ctx, GRN_TEXT_VALUE(&text), GRN_TEXT_LEN(&text), 0);
        exit_code = grn_rc_to_exit_code(ctx->rc);
        if (ctx->rc != GRN_SUCCESS) { break; }
        if (c_output(ctx)) { goto exit; }
        if (ctx->stat == GRN_CTX_QUIT) { break; }
      }
      grn_obj_unlink(ctx, &text);
    } else {
      grn_rc rc;
      rc = grn_ctx_sendv(ctx, argc, argv, 0);
      exit_code = grn_rc_to_exit_code(rc);
      if (c_output(ctx)) { goto exit; }
    }
  } else {
    fprintf(stderr, "grn_ctx_connect failed (%s:%d)\n", hostname, port);
  }
exit :
  grn_ctx_fin(ctx);
  return exit_code;
}

/* server */

typedef void (*grn_edge_dispatcher_func)(grn_ctx *ctx, grn_edge *edge);
typedef void (*grn_handler_func)(grn_ctx *ctx, grn_obj *msg);

static grn_com_queue ctx_new;
static grn_com_queue ctx_old;
static grn_mutex q_mutex;
static grn_cond q_cond;
static uint32_t nthreads = 0, nfthreads = 0, max_nfthreads;

static void
reset_ready_notify_pipe(void)
{
  ready_notify_pipe[PIPE_READ]  = 0;
  ready_notify_pipe[PIPE_WRITE] = 0;
}

static void
close_ready_notify_pipe(void)
{
  if (ready_notify_pipe[PIPE_READ] > 0) {
    close(ready_notify_pipe[PIPE_READ]);
  }
  if (ready_notify_pipe[PIPE_WRITE] > 0) {
    close(ready_notify_pipe[PIPE_WRITE]);
  }
  reset_ready_notify_pipe();
}

static void
send_ready_notify(void)
{
  if (ready_notify_pipe[PIPE_WRITE] > 0) {
    const char *ready_notify_message = "ready";
    write(ready_notify_pipe[PIPE_WRITE],
          ready_notify_message,
          strlen(ready_notify_message));
  }
  close_ready_notify_pipe();
}

static int
daemonize(void)
{
  int exit_code = EXIT_SUCCESS;
#ifndef WIN32
  pid_t pid;

  if (pipe(ready_notify_pipe) == -1) {
    reset_ready_notify_pipe();
  }

  switch (fork()) {
  case 0:
    break;
  case -1:
    perror("fork");
    return EXIT_FAILURE;
  default:
    wait(NULL);
    if (ready_notify_pipe[PIPE_READ] > 0) {
      int max_fd;
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(ready_notify_pipe[PIPE_READ], &read_fds);
      max_fd = ready_notify_pipe[PIPE_READ] + 1;
      select(max_fd, &read_fds, NULL, NULL, NULL);
    }
    close_ready_notify_pipe();
    _exit(EXIT_SUCCESS);
  }
  switch (fork()) {
  case 0:
    {
      FILE *pid_file = NULL;
      if (pid_file_path) {
        pid_file = fopen(pid_file_path, "w");
      }
      pid = getpid();
      if (!pid_file) {
        fprintf(stderr, "%d\n", pid);
      } else {
        fprintf(pid_file, "%d\n", pid);
        fclose(pid_file);
        pid_file = NULL;
      }
    }
    break;
  case -1:
    perror("fork");
    return EXIT_FAILURE;
  default:
    close_ready_notify_pipe();
    _exit(EXIT_SUCCESS);
  }
  {
    int null_fd = GRN_OPEN("/dev/null", O_RDWR, 0);
    if (null_fd != -1) {
      dup2(null_fd, STDIN_FILENO);
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      if (null_fd > STDERR_FILENO) { GRN_CLOSE(null_fd); }
    }
  }
#endif /* WIN32 */
  return exit_code;
}

static void
clean_pid_file(void)
{
#ifndef WIN32
  if (pid_file_path) {
    unlink(pid_file_path);
  }
#endif
}

static void
run_server_loop(grn_ctx *ctx, grn_com_event *ev)
{
  while (!grn_com_event_poll(ctx, ev, 1000) && grn_gctx.stat != GRN_CTX_QUIT) {
    grn_edge *edge;
    while ((edge = (grn_edge *)grn_com_queue_deque(ctx, &ctx_old))) {
      grn_obj *msg;
      while ((msg = (grn_obj *)grn_com_queue_deque(ctx, &edge->send_old))) {
        grn_msg_close(&edge->ctx, msg);
      }
      while ((msg = (grn_obj *)grn_com_queue_deque(ctx, &edge->recv_new))) {
        grn_msg_close(ctx, msg);
      }
      grn_ctx_fin(&edge->ctx);
      if (edge->com->has_sid && edge->com->opaque == edge) {
        grn_com_close(ctx, edge->com);
      }
      grn_edges_delete(ctx, edge);
    }
    /* todo : log stat */
  }
  for (;;) {
    MUTEX_LOCK(q_mutex);
    if (nthreads == nfthreads) { break; }
    MUTEX_UNLOCK(q_mutex);
    grn_nanosleep(1000000);
  }
  {
    grn_edge *edge;
    GRN_HASH_EACH(ctx, grn_edges, id, NULL, NULL, &edge, {
      grn_obj *obj;
      while ((obj = (grn_obj *)grn_com_queue_deque(ctx, &edge->send_old))) {
        grn_msg_close(&edge->ctx, obj);
      }
      while ((obj = (grn_obj *)grn_com_queue_deque(ctx, &edge->recv_new))) {
        grn_msg_close(ctx, obj);
      }
      grn_ctx_fin(&edge->ctx);
      if (edge->com->has_sid) {
        grn_com_close(ctx, edge->com);
      }
      grn_edges_delete(ctx, edge);
    });
  }
  {
    grn_com *com;
    GRN_HASH_EACH(ctx, ev->hash, id, NULL, NULL, &com, { grn_com_close(ctx, com); });
  }
}

static int
run_server(grn_ctx *ctx, grn_obj *db, grn_com_event *ev,
           grn_edge_dispatcher_func dispatcher, grn_handler_func handler)
{
  int exit_code = EXIT_SUCCESS;
  struct hostent *he;
  if (!(he = gethostbyname(hostname))) {
    send_ready_notify();
    SOERR("gethostbyname");
  } else {
    ev->opaque = db;
    grn_edges_init(ctx, dispatcher);
    if (!grn_com_sopen(ctx, ev, bind_address, port, handler, he)) {
      send_ready_notify();
      run_server_loop(ctx, ev);
      exit_code = EXIT_SUCCESS;
    } else {
      send_ready_notify();
      fprintf(stderr, "grn_com_sopen failed (%s:%d): %s\n",
              bind_address, port, ctx->errbuf);
    }
    grn_edges_fin(ctx);
  }
  return exit_code;
}

static int
start_service(grn_ctx *ctx, const char *db_path,
              grn_edge_dispatcher_func dispatcher, grn_handler_func handler)
{
  int exit_code = EXIT_SUCCESS;
  grn_com_event ev;

  if (is_daemon_mode) {
    exit_code = daemonize();
    if (exit_code != EXIT_SUCCESS) {
      return exit_code;
    }
  }

  if (!grn_com_event_init(ctx, &ev, MAX_CON, sizeof(grn_com))) {
    grn_obj *db;
    db = (newdb || !db_path) ? grn_db_create(ctx, db_path, NULL) : grn_db_open(ctx, db_path);
    if (db) {
      exit_code = run_server(ctx, db, &ev, dispatcher, handler);
      grn_obj_close(ctx, db);
    } else {
      fprintf(stderr, "db open failed (%s)\n", db_path);
      exit_code = EXIT_FAILURE;
      send_ready_notify();
    }
    grn_com_event_fin(ctx, &ev);
  } else {
    fprintf(stderr, "grn_com_event_init failed\n");
    exit_code = EXIT_FAILURE;
    send_ready_notify();
  }

  if (is_daemon_mode) {
    clean_pid_file();
  }

  return exit_code;
}

typedef struct {
  grn_msg *msg;
  grn_bool in_body;
  grn_bool is_chunked;
} ht_context;

static void
h_output_set_header(grn_ctx *ctx, grn_obj *header,
                    grn_rc rc, long long int content_length)
{
  switch (rc) {
  case GRN_SUCCESS :
    GRN_TEXT_SETS(ctx, header, "HTTP/1.1 200 OK\r\n");
    break;
  case GRN_INVALID_ARGUMENT :
  case GRN_SYNTAX_ERROR :
    GRN_TEXT_SETS(ctx, header, "HTTP/1.1 400 Bad Request\r\n");
    break;
  case GRN_NO_SUCH_FILE_OR_DIRECTORY :
    GRN_TEXT_SETS(ctx, header, "HTTP/1.1 404 Not Found\r\n");
    break;
  default :
    GRN_TEXT_SETS(ctx, header, "HTTP/1.1 500 Internal Server Error\r\n");
    break;
  }
  GRN_TEXT_PUTS(ctx, header, "Content-Type: ");
  GRN_TEXT_PUTS(ctx, header, grn_ctx_get_mime_type(ctx));
  GRN_TEXT_PUTS(ctx, header, "\r\n");
  if (content_length >= 0) {
    GRN_TEXT_PUTS(ctx, header, "Connection: close\r\n");
    GRN_TEXT_PUTS(ctx, header, "Content-Length: ");
    grn_text_lltoa(ctx, header, content_length);
    GRN_TEXT_PUTS(ctx, header, "\r\n");
  } else {
    GRN_TEXT_PUTS(ctx, header, "Transfer-Encoding: chunked\r\n");
  }
  GRN_TEXT_PUTS(ctx, header, "\r\n");
}

static void
h_output_send(grn_ctx *ctx, grn_sock fd,
              grn_obj *header, grn_obj *head, grn_obj *body, grn_obj *foot)
{
  ssize_t ret;
  ssize_t len = 0;
#ifdef WIN32
  int n_buffers = 0;
  WSABUF wsabufs[4];
  if (header) {
    wsabufs[n_buffers].buf = GRN_TEXT_VALUE(header);
    wsabufs[n_buffers].len = GRN_TEXT_LEN(header);
    len += GRN_TEXT_LEN(header);
    n_buffers++;
  }
  if (head) {
    wsabufs[n_buffers].buf = GRN_TEXT_VALUE(head);
    wsabufs[n_buffers].len = GRN_TEXT_LEN(head);
    len += GRN_TEXT_LEN(head);
    n_buffers++;
  }
  if (body) {
    wsabufs[n_buffers].buf = GRN_TEXT_VALUE(body);
    wsabufs[n_buffers].len = GRN_TEXT_LEN(body);
    len += GRN_TEXT_LEN(body);
    n_buffers++;
  }
  if (foot) {
    wsabufs[n_buffers].buf = GRN_TEXT_VALUE(foot);
    wsabufs[n_buffers].len = GRN_TEXT_LEN(foot);
    len += GRN_TEXT_LEN(foot);
    n_buffers++;
  }
  {
    DWORD sent;
    if (WSASend(fd, wsabufs, n_buffers, &sent, 0, NULL, NULL) == SOCKET_ERROR) {
      SOERR("WSASend");
    }
    ret = sent;
  }
#else /* WIN32 */
  struct iovec msg_iov[4];
  struct msghdr msg;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_iov = msg_iov;
  msg.msg_iovlen = 0;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

  if (header) {
    msg_iov[msg.msg_iovlen].iov_base = GRN_TEXT_VALUE(header);
    msg_iov[msg.msg_iovlen].iov_len = GRN_TEXT_LEN(header);
    len += GRN_TEXT_LEN(header);
    msg.msg_iovlen++;
  }
  if (head) {
    msg_iov[msg.msg_iovlen].iov_base = GRN_TEXT_VALUE(head);
    msg_iov[msg.msg_iovlen].iov_len = GRN_TEXT_LEN(head);
    len += GRN_TEXT_LEN(head);
    msg.msg_iovlen++;
  }
  if (body) {
    msg_iov[msg.msg_iovlen].iov_base = GRN_TEXT_VALUE(body);
    msg_iov[msg.msg_iovlen].iov_len = GRN_TEXT_LEN(body);
    len += GRN_TEXT_LEN(body);
    msg.msg_iovlen++;
  }
  if (foot) {
    msg_iov[msg.msg_iovlen].iov_base = GRN_TEXT_VALUE(foot);
    msg_iov[msg.msg_iovlen].iov_len = GRN_TEXT_LEN(foot);
    len += GRN_TEXT_LEN(foot);
    msg.msg_iovlen++;
  }
  if ((ret = sendmsg(fd, &msg, MSG_NOSIGNAL)) == -1) {
    SOERR("sendmsg");
  }
#endif /* WIN32 */
  if (ret != len) {
    GRN_LOG(&grn_gctx, GRN_LOG_NOTICE,
            "couldn't send all data (%" GRN_FMT_LLD "/%" GRN_FMT_LLD ")",
            (long long int)ret, (long long int)len);
  }
}

static void
h_output_raw(grn_ctx *ctx, int flags, ht_context *hc)
{
  grn_rc expr_rc = ctx->rc;
  grn_sock fd = hc->msg->u.fd;
  grn_obj header_;
  grn_obj head_;
  grn_obj body_;
  grn_obj foot_;
  grn_obj *header = NULL;
  grn_obj *head = NULL;
  grn_obj *body = NULL;
  grn_obj *foot = NULL;
  char *chunk = NULL;
  unsigned int chunk_size = 0;
  int recv_flags;
  grn_bool is_last_message = (flags & GRN_CTX_TAIL);

  GRN_TEXT_INIT(&header_, 0);
  GRN_TEXT_INIT(&head_, 0);
  GRN_TEXT_INIT(&body_, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_INIT(&foot_, 0);

  grn_ctx_recv(ctx, &chunk, &chunk_size, &recv_flags);
  GRN_TEXT_SET(ctx, &body_, chunk, chunk_size);

  if (!hc->in_body) {
    if (is_last_message) {
      h_output_set_header(ctx, &header_, expr_rc, GRN_TEXT_LEN(&body_));
      hc->is_chunked = GRN_FALSE;
    } else {
      h_output_set_header(ctx, &header_, expr_rc, -1);
      hc->is_chunked = GRN_TRUE;
    }
    header = &header_;
    hc->in_body = GRN_TRUE;
  }

  if (GRN_TEXT_LEN(&body_) > 0) {
    if (hc->is_chunked) {
      grn_text_printf(ctx, &head_,
                      "%x\r\n", (unsigned int)GRN_TEXT_LEN(&body_));
      head = &head_;
      GRN_TEXT_PUTS(ctx, &foot_, "\r\n");
      foot = &foot_;
    }
    body = &body_;
  }

  if (is_last_message) {
    if (hc->is_chunked) {
      GRN_TEXT_PUTS(ctx, &foot_, "0\r\n");
      GRN_TEXT_PUTS(ctx, &foot_, "Connection: close\r\n");
      GRN_TEXT_PUTS(ctx, &foot_, "\r\n");
      foot = &foot_;
    }
  }

  h_output_send(ctx, fd, header, head, body, foot);

  GRN_OBJ_FIN(ctx, &foot_);
  GRN_OBJ_FIN(ctx, &body_);
  GRN_OBJ_FIN(ctx, &head_);
  GRN_OBJ_FIN(ctx, &header_);
}

static void
h_output_typed(grn_ctx *ctx, int flags, ht_context *hc)
{
  grn_rc expr_rc = ctx->rc;
  grn_sock fd = hc->msg->u.fd;
  grn_obj header, head, body, foot;
  char *chunk = NULL;
  unsigned int chunk_size = 0;
  int recv_flags;
  grn_bool should_return_body;

  if (!(flags & GRN_CTX_TAIL)) { return; }

  switch (hc->msg->header.qtype) {
  case 'G' :
  case 'P' :
    should_return_body = GRN_TRUE;
    break;
  default :
    should_return_body = GRN_FALSE;
    break;
  }

  GRN_TEXT_INIT(&header, 0);
  GRN_TEXT_INIT(&head, 0);
  GRN_TEXT_INIT(&body, 0);
  GRN_TEXT_INIT(&foot, 0);

  grn_ctx_recv(ctx, &chunk, &chunk_size, &recv_flags);
  GRN_TEXT_SET(ctx, &body, chunk, chunk_size);

  output_envelope(ctx, expr_rc, &head, &body, &foot);
  h_output_set_header(ctx, &header, expr_rc,
                      GRN_TEXT_LEN(&head) +
                      GRN_TEXT_LEN(&body) +
                      GRN_TEXT_LEN(&foot));
  if (should_return_body) {
    h_output_send(ctx, fd, &header, &head, &body, &foot);
  } else {
    h_output_send(ctx, fd, &header, NULL, NULL, NULL);
  }
  GRN_OBJ_FIN(ctx, &foot);
  GRN_OBJ_FIN(ctx, &body);
  GRN_OBJ_FIN(ctx, &head);
  GRN_OBJ_FIN(ctx, &header);
}

static void
h_output(grn_ctx *ctx, int flags, void *arg)
{
  ht_context *hc = (ht_context *)arg;

  if (grn_ctx_get_output_type(ctx) == GRN_CONTENT_NONE) {
    h_output_raw(ctx, flags, hc);
  } else {
    h_output_typed(ctx, flags, hc);
  }
}

static void
do_htreq_get(grn_ctx *ctx, grn_msg *msg)
{
  char *path = NULL;
  char *pathe = GRN_BULK_HEAD((grn_obj *)msg);
  char *e = GRN_BULK_CURR((grn_obj *)msg);
  for (;; pathe++) {
    if (e <= pathe + 6) {
      /* invalid request */
      return;
    }
    if (*pathe == ' ') {
      if (!path) {
        path = pathe + 1;
      } else {
        if (!memcmp(pathe + 1, "HTTP/1", 6)) {
          break;
        }
      }
    }
  }
  grn_ctx_send(ctx, path, pathe - path, 0);
}

typedef struct {
  const char *path_start;
  int path_length;
  int content_length;
  grn_bool have_100_continue;
  const char *body_start;
} h_post_header;

#define STRING_EQUAL(string, string_length, constant_string)\
  (string_length == strlen(constant_string) &&\
   strncmp(string, constant_string, string_length) == 0)

#define STRING_EQUAL_CI(string, string_length, constant_string)\
  (string_length == strlen(constant_string) &&\
   strncasecmp(string, constant_string, string_length) == 0)

static const char *
do_htreq_post_parse_header_request_line(grn_ctx *ctx,
                                        const char *start,
                                        const char *end,
                                        h_post_header *header)
{
  const char *current;

  {
    const char *method = start;
    int method_length = -1;

    for (current = method; current < end; current++) {
      if (current[0] == '\n') {
        return NULL;
      }
      if (current[0] == ' ') {
        method_length = current - method;
        current++;
        break;
      }
    }
    if (method_length == -1) {
      return NULL;
    }
    if (!STRING_EQUAL_CI(method, method_length, "POST")) {
      return NULL;
    }
  }

  {
    header->path_start = current;
    header->path_length = -1;
    for (; current < end; current++) {
      if (current[0] == '\n') {
        return NULL;
      }
      if (current[0] == ' ') {
        header->path_length = current - header->path_start;
        current++;
        break;
      }
    }
    if (header->path_length == -1) {
      return NULL;
    }
  }

  {
    const char *http_version_start = current;
    int http_version_length = -1;
    for (; current < end; current++) {
      if (current[0] == '\n') {
        http_version_length = current - http_version_start;
        if (http_version_length > 0 &&
            http_version_start[http_version_length - 1] == '\r') {
          http_version_length--;
        }
        current++;
        break;
      }
    }
    if (http_version_length == -1) {
      return NULL;
    }
    if (!(STRING_EQUAL_CI(http_version_start, http_version_length, "HTTP/1.0") ||
          STRING_EQUAL_CI(http_version_start, http_version_length, "HTTP/1.1"))) {
      return NULL;
    }
  }

  return current;
}

static const char *
do_htreq_post_parse_header_values(grn_ctx *ctx,
                                  const char *start,
                                  const char *end,
                                  h_post_header *header)
{
  const char *current;
  const char *name = start;
  int name_length = -1;
  const char *value = NULL;
  int value_length = -1;

  for (current = start; current < end; current++) {
    switch (current[0]) {
    case '\n' :
      if (name_length == -1) {
        if (current - name == 1 && current[-1] == '\r') {
          return current + 1;
        } else {
          /* No ":" header line. TODO: report error. */
          return NULL;
        }
      } else {
        while (value < current && value[0] == ' ') {
          value++;
        }
        value_length = current - value;
        if (value_length > 0 && value[value_length - 1] == '\r') {
          value_length--;
        }
        if (STRING_EQUAL_CI(name, name_length, "Content-Length")) {
          const char *rest;
          header->content_length = grn_atoi(value, value + value_length, &rest);
          if (rest != value + value_length) {
            /* Invalid Content-Length value. TODO: report error. */
            header->content_length = -1;
          }
        } else if (STRING_EQUAL_CI(name, name_length, "Expect")) {
          if (STRING_EQUAL(value, value_length, "100-continue")) {
            header->have_100_continue = GRN_TRUE;
          }
        }
      }
      name = current + 1;
      name_length = -1;
      value = NULL;
      value_length = -1;
      break;
    case ':' :
      if (name_length == -1) {
        name_length = current - name;
        value = current + 1;
      }
      break;
    default :
      break;
    }
  }

  return NULL;
}

static grn_bool
do_htreq_post_parse_header(grn_ctx *ctx,
                           const char *start,
                           const char *end,
                           h_post_header *header)
{
  const char *current;

  current = do_htreq_post_parse_header_request_line(ctx, start, end, header);
  if (!current) {
    return GRN_FALSE;
  }
  current = do_htreq_post_parse_header_values(ctx, current, end, header);
  if (!current) {
    return GRN_FALSE;
  }

  if (current == end) {
    header->body_start = NULL;
  } else {
    header->body_start = current;
  }

  return GRN_TRUE;
}

static void
do_htreq_post(grn_ctx *ctx, grn_msg *msg)
{
  grn_sock fd = msg->u.fd;
  const char *end;
  h_post_header header;

  header.path_start = NULL;
  header.path_length = -1;
  header.content_length = -1;
  header.body_start = NULL;
  header.have_100_continue = GRN_FALSE;

  end = GRN_BULK_CURR((grn_obj *)msg);
  if (!do_htreq_post_parse_header(ctx,
                                  GRN_BULK_HEAD((grn_obj *)msg),
                                  end,
                                  &header)) {
    return;
  }

  grn_ctx_send(ctx, header.path_start, header.path_length, GRN_CTX_QUIET);
  if (ctx->rc != GRN_SUCCESS) {
    ht_context context;
    context.msg = msg;
    context.in_body = GRN_FALSE;
    context.is_chunked = GRN_FALSE;
    h_output(ctx, GRN_CTX_TAIL, &context);
    return;
  }

  if (header.have_100_continue) {
    const char *continue_message = "HTTP/1.1 100 Continue\r\n";
    ssize_t send_size;
    int send_flags = MSG_NOSIGNAL;
    send_size = send(fd, continue_message, strlen(continue_message), send_flags);
    if (send_size == -1) {
      SOERR("send");
      return;
    }
  }

  {
    grn_obj line_buffer;
    int read_content_length = 0;

    GRN_TEXT_INIT(&line_buffer, 0);
    while (read_content_length < header.content_length) {
#define POST_BUFFER_SIZE 8192
      grn_rc rc;
      char buffer[POST_BUFFER_SIZE];
      const char *buffer_start, *buffer_current, *buffer_end;

      if (header.body_start) {
        buffer_start = header.body_start;
        buffer_end = end;
        header.body_start = NULL;
      } else {
        ssize_t recv_length;
        int recv_flags = 0;
        recv_length = recv(fd, buffer, POST_BUFFER_SIZE, recv_flags);
        if (recv_length == 0) {
          break;
        }
        if (recv_length == -1) {
          SOERR("recv");
          break;
        }
        buffer_start = buffer;
        buffer_end = buffer_start + recv_length;
      }
      read_content_length += buffer_end - buffer_start;

      rc = GRN_SUCCESS;
      buffer_current = buffer_start;
      for (; rc == GRN_SUCCESS && buffer_current < buffer_end; buffer_current++) {
        if (buffer_current[0] != '\n') {
          continue;
        }
        GRN_TEXT_PUT(ctx,
                     &line_buffer,
                     buffer_start,
                     buffer_current - buffer_start);
        {
          int flags = 0;
          if (!(read_content_length == header.content_length &&
                buffer_current + 1 == buffer_end)) {
            flags |= GRN_CTX_QUIET;
          }
          rc = grn_ctx_send(ctx,
                            GRN_TEXT_VALUE(&line_buffer),
                            GRN_TEXT_LEN(&line_buffer),
                            flags);
        }
        buffer_start = buffer_current + 1;
        GRN_BULK_REWIND(&line_buffer);
      }
      GRN_TEXT_PUT(ctx, &line_buffer, buffer_start, buffer_end - buffer_start);
#undef POST_BUFFER_SIZE
    }

    if (GRN_TEXT_LEN(&line_buffer) > 0) {
      grn_ctx_send(ctx,
                   GRN_TEXT_VALUE(&line_buffer),
                   GRN_TEXT_LEN(&line_buffer),
                   0);
    }

    GRN_OBJ_FIN(ctx, &line_buffer);
  }
}

static void
do_htreq(grn_ctx *ctx, grn_msg *msg)
{
  grn_com_header *header = &msg->header;
  switch (header->qtype) {
  case 'G' : /* GET */
  case 'H' : /* HEAD */
    do_htreq_get(ctx, msg);
    break;
  case 'P' : /* POST */
    do_htreq_post(ctx, msg);
    break;
  }
  grn_ctx_set_next_expr(ctx, NULL);
  /* if (ctx->rc != GRN_OPERATION_WOULD_BLOCK) {...} */
  grn_msg_close(ctx, (grn_obj *)msg);
  /* if not keep alive connection */
  grn_sock_close(msg->u.fd);
  grn_com_event_start_accept(ctx, msg->acceptor->ev);
}

enum {
  MBRES_SUCCESS = 0x00,
  MBRES_KEY_ENOENT = 0x01,
  MBRES_KEY_EEXISTS = 0x02,
  MBRES_E2BIG = 0x03,
  MBRES_EINVAL = 0x04,
  MBRES_NOT_STORED = 0x05,
  MBRES_UNKNOWN_COMMAND = 0x81,
  MBRES_ENOMEM = 0x82,
};

enum {
  MBCMD_GET = 0x00,
  MBCMD_SET = 0x01,
  MBCMD_ADD = 0x02,
  MBCMD_REPLACE = 0x03,
  MBCMD_DELETE = 0x04,
  MBCMD_INCREMENT = 0x05,
  MBCMD_DECREMENT = 0x06,
  MBCMD_QUIT = 0x07,
  MBCMD_FLUSH = 0x08,
  MBCMD_GETQ = 0x09,
  MBCMD_NOOP = 0x0a,
  MBCMD_VERSION = 0x0b,
  MBCMD_GETK = 0x0c,
  MBCMD_GETKQ = 0x0d,
  MBCMD_APPEND = 0x0e,
  MBCMD_PREPEND = 0x0f,
  MBCMD_STAT = 0x10,
  MBCMD_SETQ = 0x11,
  MBCMD_ADDQ = 0x12,
  MBCMD_REPLACEQ = 0x13,
  MBCMD_DELETEQ = 0x14,
  MBCMD_INCREMENTQ = 0x15,
  MBCMD_DECREMENTQ = 0x16,
  MBCMD_QUITQ = 0x17,
  MBCMD_FLUSHQ = 0x18,
  MBCMD_APPENDQ = 0x19,
  MBCMD_PREPENDQ = 0x1a
};

static grn_critical_section cache_lock;
static grn_obj *cache_table = NULL;
static grn_obj *cache_value = NULL;
static grn_obj *cache_flags = NULL;
static grn_obj *cache_expire = NULL;
static grn_obj *cache_cas = NULL;

#define CTX_GET(name) (grn_ctx_get(ctx, (name), strlen(name)))

static grn_obj *
cache_init(grn_ctx *ctx)
{
  if (cache_cas) { return cache_cas; }
  CRITICAL_SECTION_ENTER(cache_lock);
  if (!cache_cas) {
    if ((cache_table = CTX_GET("Memcache"))) {
      cache_value = CTX_GET("Memcache.value");
      cache_flags = CTX_GET("Memcache.flags");
      cache_expire = CTX_GET("Memcache.expire");
      cache_cas = CTX_GET("Memcache.cas");
    } else {
      if (!cache_table) {
        grn_obj *uint32_type = grn_ctx_at(ctx, GRN_DB_UINT32);
        grn_obj *uint64_type = grn_ctx_at(ctx, GRN_DB_UINT64);
        grn_obj *shorttext_type = grn_ctx_at(ctx, GRN_DB_SHORT_TEXT);
        if ((cache_table = grn_table_create(ctx, "Memcache", 8, NULL,
                                            GRN_OBJ_TABLE_PAT_KEY|GRN_OBJ_PERSISTENT,
                                            shorttext_type, NULL))) {
          cache_value = grn_column_create(ctx, cache_table, "value", 5, NULL,
                                          GRN_OBJ_PERSISTENT, shorttext_type);
          cache_flags = grn_column_create(ctx, cache_table, "flags", 5, NULL,
                                          GRN_OBJ_PERSISTENT, uint32_type);
          cache_expire = grn_column_create(ctx, cache_table, "expire", 6, NULL,
                                           GRN_OBJ_PERSISTENT, uint32_type);
          cache_cas = grn_column_create(ctx, cache_table, "cas", 3, NULL,
                                        GRN_OBJ_PERSISTENT, uint64_type);
        }
      }
    }
  }
  CRITICAL_SECTION_LEAVE(cache_lock);
  return cache_cas;
}

#define RELATIVE_TIME_THRESH 1000000000

#define MBRES(ctx,re,status,key_len,extra_len,flags) do {\
  grn_msg_set_property((ctx), (re), (status), (key_len), (extra_len));\
  grn_msg_send((ctx), (re), (flags));\
} while (0)

#define GRN_MSG_MBRES(block) do {\
  if (!quiet) {\
    grn_obj *re = grn_msg_open_for_reply(ctx, (grn_obj *)msg, &edge->send_old);\
    ((grn_msg *)re)->header.qtype = header->qtype;\
    block\
  }\
} while (0)

static uint64_t
get_mbreq_cas_id()
{
  static uint64_t cas_id = 0;
  /* FIXME: use GRN_ATOMIC_ADD_EX_64, but it is not implemented */
  return ++cas_id;
}

static void
do_mbreq(grn_ctx *ctx, grn_edge *edge)
{
  int quiet = 0;
  int flags = 0;
  grn_msg *msg = edge->msg;
  grn_com_header *header = &msg->header;

  switch (header->qtype) {
  case MBCMD_GETQ :
    flags = GRN_CTX_MORE;
    /* fallthru */
  case MBCMD_GET :
    {
      grn_id rid;
      uint16_t keylen = ntohs(header->keylen);
      char *key = GRN_BULK_HEAD((grn_obj *)msg);
      cache_init(ctx);
      rid = grn_table_get(ctx, cache_table, key, keylen);
      if (!rid) {
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
        });
      } else {
        grn_timeval tv;
        uint32_t expire;
        {
          grn_obj expire_buf;
          GRN_UINT32_INIT(&expire_buf, 0);
          grn_obj_get_value(ctx, cache_expire, rid, &expire_buf);
          expire = GRN_UINT32_VALUE(&expire_buf);
          grn_obj_close(ctx, &expire_buf);
        }
        grn_timeval_now(ctx, &tv);
        if (expire && expire < tv.tv_sec) {
          grn_table_delete_by_id(ctx, cache_table, rid);
          GRN_MSG_MBRES({
            MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
          });
        } else {
          grn_obj cas_buf;
          GRN_UINT64_INIT(&cas_buf, 0);
          grn_obj_get_value(ctx, cache_cas, rid, &cas_buf);
          GRN_MSG_MBRES({
            grn_obj_get_value(ctx, cache_flags, rid, re);
            grn_obj_get_value(ctx, cache_value, rid, re);
            ((grn_msg *)re)->header.cas = GRN_UINT64_VALUE(&cas_buf);
            MBRES(ctx, re, MBRES_SUCCESS, 0, 4, flags);
          });
          grn_obj_close(ctx, &cas_buf);
        }
      }
    }
    break;
  case MBCMD_SETQ :
  case MBCMD_ADDQ :
  case MBCMD_REPLACEQ :
    quiet = 1;
    /* fallthru */
  case MBCMD_SET :
  case MBCMD_ADD :
  case MBCMD_REPLACE :
    {
      grn_id rid;
      uint32_t size = ntohl(header->size);
      uint16_t keylen = ntohs(header->keylen);
      uint8_t extralen = header->level;
      char *body = GRN_BULK_HEAD((grn_obj *)msg);
      uint32_t flags = *((uint32_t *)body);
      uint32_t expire = ntohl(*((uint32_t *)(body + 4)));
      uint32_t valuelen = size - keylen - extralen;
      char *key = body + 8;
      char *value = key + keylen;
      int added = 0;
      int f = (header->qtype == MBCMD_REPLACE ||
               header->qtype == MBCMD_REPLACEQ) ? 0 : GRN_TABLE_ADD;
      GRN_ASSERT(extralen == 8);
      cache_init(ctx);
      if (header->qtype == MBCMD_REPLACE || header->qtype == MBCMD_REPLACEQ) {
        rid = grn_table_get(ctx, cache_table, key, keylen);
      } else {
        rid = grn_table_add(ctx, cache_table, key, keylen, &added);
      }
      if (!rid) {
        GRN_MSG_MBRES({
          MBRES(ctx, re, (f & GRN_TABLE_ADD) ? MBRES_ENOMEM : MBRES_NOT_STORED, 0, 0, 0);
        });
      } else {
        if (added) {
          if (header->cas) {
            GRN_MSG_MBRES({
              MBRES(ctx, re, MBRES_EINVAL, 0, 0, 0);
            });
          } else {
            grn_obj text_buf, uint32_buf;
            GRN_TEXT_INIT(&text_buf, GRN_OBJ_DO_SHALLOW_COPY);
            GRN_TEXT_SET_REF(&text_buf, value, valuelen);
            grn_obj_set_value(ctx, cache_value, rid, &text_buf, GRN_OBJ_SET);
            GRN_UINT32_INIT(&uint32_buf, 0);
            GRN_UINT32_SET(ctx, &uint32_buf, flags);
            grn_obj_set_value(ctx, cache_flags, rid, &uint32_buf, GRN_OBJ_SET);
            if (expire && expire < RELATIVE_TIME_THRESH) {
              grn_timeval tv;
              grn_timeval_now(ctx, &tv);
              expire += tv.tv_sec;
            }
            GRN_UINT32_SET(ctx, &uint32_buf, expire);
            grn_obj_set_value(ctx, cache_expire, rid, &uint32_buf, GRN_OBJ_SET);
            grn_obj_close(ctx, &uint32_buf);
            {
              grn_obj cas_buf;
              uint64_t cas_id = get_mbreq_cas_id();
              GRN_UINT64_INIT(&cas_buf, 0);
              GRN_UINT64_SET(ctx, &cas_buf, cas_id);
              grn_obj_set_value(ctx, cache_cas, rid, &cas_buf, GRN_OBJ_SET);
              grn_obj_close(ctx, &cas_buf);
              GRN_MSG_MBRES({
                ((grn_msg *)re)->header.cas = cas_id;
                MBRES(ctx, re, MBRES_SUCCESS, 0, 0, 0);
              });
            }
          }
        } else {
          if (header->qtype != MBCMD_SET && header->qtype != MBCMD_SETQ) {
            grn_obj uint32_buf;
            grn_timeval tv;
            uint32_t oexpire;

            GRN_UINT32_INIT(&uint32_buf, 0);
            grn_obj_get_value(ctx, cache_expire, rid, &uint32_buf);
            oexpire = GRN_UINT32_VALUE(&uint32_buf);
            grn_timeval_now(ctx, &tv);

            if (oexpire && oexpire < tv.tv_sec) {
              if (header->qtype == MBCMD_REPLACE ||
                  header->qtype == MBCMD_REPLACEQ) {
                grn_table_delete_by_id(ctx, cache_table, rid);
                GRN_MSG_MBRES({
                  MBRES(ctx, re, MBRES_NOT_STORED, 0, 0, 0);
                });
                break;
              }
            } else if (header->qtype == MBCMD_ADD ||
                       header->qtype == MBCMD_ADDQ) {
              GRN_MSG_MBRES({
                MBRES(ctx, re, MBRES_NOT_STORED, 0, 0, 0);
              });
              break;
            }
          }
          {
            if (header->cas) {
              grn_obj cas_buf;
              GRN_UINT64_INIT(&cas_buf, 0);
              grn_obj_get_value(ctx, cache_cas, rid, &cas_buf);
              if (header->cas != GRN_UINT64_VALUE(&cas_buf)) {
                GRN_MSG_MBRES({
                  MBRES(ctx, re, MBRES_NOT_STORED, 0, 0, 0);
                });
              }
            }
            {
              grn_obj text_buf, uint32_buf;
              GRN_TEXT_INIT(&text_buf, GRN_OBJ_DO_SHALLOW_COPY);
              GRN_TEXT_SET_REF(&text_buf, value, valuelen);
              grn_obj_set_value(ctx, cache_value, rid, &text_buf, GRN_OBJ_SET);
              GRN_UINT32_INIT(&uint32_buf, 0);
              GRN_UINT32_SET(ctx, &uint32_buf, flags);
              grn_obj_set_value(ctx, cache_flags, rid, &uint32_buf, GRN_OBJ_SET);
              if (expire && expire < RELATIVE_TIME_THRESH) {
                grn_timeval tv;
                grn_timeval_now(ctx, &tv);
                expire += tv.tv_sec;
              }
              GRN_UINT32_SET(ctx, &uint32_buf, expire);
              grn_obj_set_value(ctx, cache_expire, rid, &uint32_buf, GRN_OBJ_SET);
              {
                grn_obj cas_buf;
                uint64_t cas_id = get_mbreq_cas_id();
                GRN_UINT64_INIT(&cas_buf, 0);
                GRN_UINT64_SET(ctx, &cas_buf, cas_id);
                grn_obj_set_value(ctx, cache_cas, rid, &cas_buf, GRN_OBJ_SET);
                GRN_MSG_MBRES({
                  ((grn_msg *)re)->header.cas = cas_id;
                  MBRES(ctx, re, MBRES_SUCCESS, 0, 0, 0);
                });
              }
            }
          }
        }
      }
    }
    break;
  case MBCMD_DELETEQ :
    quiet = 1;
    /* fallthru */
  case MBCMD_DELETE :
    {
      grn_id rid;
      uint16_t keylen = ntohs(header->keylen);
      char *key = GRN_BULK_HEAD((grn_obj *)msg);
      cache_init(ctx);
      rid = grn_table_get(ctx, cache_table, key, keylen);
      if (!rid) {
        /* GRN_LOG(ctx, GRN_LOG_NOTICE, "GET k=%d not found", keylen); */
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
        });
      } else {
        grn_table_delete_by_id(ctx, cache_table, rid);
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_SUCCESS, 0, 4, 0);
        });
      }
    }
    break;
  case MBCMD_INCREMENTQ :
  case MBCMD_DECREMENTQ :
    quiet = 1;
    /* fallthru */
  case MBCMD_INCREMENT :
  case MBCMD_DECREMENT :
    {
      grn_id rid;
      int added = 0;
      uint64_t delta, init;
      uint16_t keylen = ntohs(header->keylen);
      char *body = GRN_BULK_HEAD((grn_obj *)msg);
      char *key = body + 20;
      uint32_t expire = ntohl(*((uint32_t *)(body + 16)));
      grn_ntoh(&delta, body, 8);
      grn_ntoh(&init, body + 8, 8);
      GRN_ASSERT(header->level == 20); /* extralen */
      cache_init(ctx);
      if (expire == 0xffffffff) {
        rid = grn_table_get(ctx, cache_table, key, keylen);
      } else {
        rid = grn_table_add(ctx, cache_table, key, keylen, &added);
      }
      if (!rid) {
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
        });
      } else {
        grn_obj uint32_buf, text_buf;
        GRN_UINT32_INIT(&uint32_buf, 0);
        GRN_TEXT_INIT(&text_buf, GRN_OBJ_DO_SHALLOW_COPY);
        if (added) {
          GRN_TEXT_SET_REF(&text_buf, &init, 8);
          grn_obj_set_value(ctx, cache_value, rid, &text_buf, GRN_OBJ_SET);
          GRN_UINT32_SET(ctx, &uint32_buf, 0);
          grn_obj_set_value(ctx, cache_flags, rid, &uint32_buf, GRN_OBJ_SET);
        } else {
          grn_timeval tv;
          uint32_t oexpire;

          grn_obj_get_value(ctx, cache_expire, rid, &uint32_buf);
          oexpire = GRN_UINT32_VALUE(&uint32_buf);
          grn_timeval_now(ctx, &tv);

          if (oexpire && oexpire < tv.tv_sec) {
            if (expire == 0xffffffffU) {
              GRN_MSG_MBRES({
                MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
              });
              break;
            } else {
              GRN_TEXT_SET_REF(&text_buf, &init, 8);
              grn_obj_set_value(ctx, cache_value, rid, &text_buf, GRN_OBJ_SET);
              GRN_UINT32_SET(ctx, &uint32_buf, 0);
              grn_obj_set_value(ctx, cache_flags, rid, &uint32_buf, GRN_OBJ_SET);
            }
          } else {
            grn_obj uint64_buf;
            GRN_UINT64_INIT(&uint64_buf, 0);
            GRN_UINT64_SET(ctx, &uint64_buf, delta);
            grn_obj_set_value(ctx, cache_value, rid, &uint64_buf,
                              header->qtype == MBCMD_INCREMENT ||
                              header->qtype == MBCMD_INCREMENTQ
                              ? GRN_OBJ_INCR
                              : GRN_OBJ_DECR);
          }
        }
        if (expire && expire < RELATIVE_TIME_THRESH) {
          grn_timeval tv;
          grn_timeval_now(ctx, &tv);
          expire += tv.tv_sec;
        }
        GRN_UINT32_SET(ctx, &uint32_buf, expire);
        grn_obj_set_value(ctx, cache_expire, rid, &uint32_buf, GRN_OBJ_SET);
        GRN_MSG_MBRES({
          /* TODO: get_mbreq_cas_id() */
          grn_obj_get_value(ctx, cache_value, rid, re);
          grn_hton(&delta, (uint64_t *)GRN_BULK_HEAD(re), 8);
          GRN_TEXT_SET(ctx, re, &delta, sizeof(uint64_t));
          MBRES(ctx, re, MBRES_SUCCESS, 0, sizeof(uint64_t), 0);
        });
      }
    }
    break;
  case MBCMD_FLUSHQ :
    quiet = 1;
    /* fallthru */
  case MBCMD_FLUSH :
    {
      uint32_t expire;
      uint8_t extralen = header->level;
      if (extralen) {
        char *body = GRN_BULK_HEAD((grn_obj *)msg);
        GRN_ASSERT(extralen == 4);
        expire = ntohl(*((uint32_t *)(body)));
        if (expire < RELATIVE_TIME_THRESH) {
          grn_timeval tv;
          grn_timeval_now(ctx, &tv);
          if (expire) {
            expire += tv.tv_sec;
          } else {
            expire = tv.tv_sec - 1;
          }
        }
      } else {
        grn_timeval tv;
        grn_timeval_now(ctx, &tv);
        expire = tv.tv_sec - 1;
      }
      {
        grn_obj exp_buf;
        GRN_UINT32_INIT(&exp_buf, 0);
        GRN_UINT32_SET(ctx, &exp_buf, expire);
        GRN_TABLE_EACH(ctx, cache_table, 0, 0, rid, NULL, NULL, NULL, {
          grn_obj_set_value(ctx, cache_expire, rid, &exp_buf, GRN_OBJ_SET);
        });
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_SUCCESS, 0, 4, 0);
        });
        grn_obj_close(ctx, &exp_buf);
      }
    }
    break;
  case MBCMD_NOOP :
    break;
  case MBCMD_VERSION :
    GRN_MSG_MBRES({
      grn_bulk_write(ctx, re, PACKAGE_VERSION, strlen(PACKAGE_VERSION));
      MBRES(ctx, re, MBRES_SUCCESS, 0, 0, 0);
    });
    break;
  case MBCMD_GETKQ :
    flags = GRN_CTX_MORE;
    /* fallthru */
  case MBCMD_GETK :
    {
      grn_id rid;
      uint16_t keylen = ntohs(header->keylen);
      char *key = GRN_BULK_HEAD((grn_obj *)msg);
      cache_init(ctx);
      rid = grn_table_get(ctx, cache_table, key, keylen);
      if (!rid) {
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
        });
      } else {
        grn_obj uint32_buf;
        grn_timeval tv;
        uint32_t expire;
        GRN_UINT32_INIT(&uint32_buf, 0);
        grn_obj_get_value(ctx, cache_expire, rid, &uint32_buf);
        expire = GRN_UINT32_VALUE(&uint32_buf);
        grn_timeval_now(ctx, &tv);
        if (expire && expire < tv.tv_sec) {
          grn_table_delete_by_id(ctx, cache_table, rid);
          GRN_MSG_MBRES({
            MBRES(ctx, re, MBRES_KEY_ENOENT, 0, 0, 0);
          });
        } else {
          grn_obj uint64_buf;
          GRN_UINT64_INIT(&uint64_buf, 0);
          grn_obj_get_value(ctx, cache_cas, rid, &uint64_buf);
          GRN_MSG_MBRES({
            grn_obj_get_value(ctx, cache_flags, rid, re);
            grn_bulk_write(ctx, re, key, keylen);
            grn_obj_get_value(ctx, cache_value, rid, re);
            ((grn_msg *)re)->header.cas = GRN_UINT64_VALUE(&uint64_buf);
            MBRES(ctx, re, MBRES_SUCCESS, keylen, 4, flags);
          });
        }
      }
    }
    break;
  case MBCMD_APPENDQ :
  case MBCMD_PREPENDQ :
    quiet = 1;
    /* fallthru */
  case MBCMD_APPEND :
  case MBCMD_PREPEND :
    {
      grn_id rid;
      uint32_t size = ntohl(header->size);
      uint16_t keylen = ntohs(header->keylen);
      char *key = GRN_BULK_HEAD((grn_obj *)msg);
      char *value = key + keylen;
      uint32_t valuelen = size - keylen;
      cache_init(ctx);
      rid = grn_table_add(ctx, cache_table, key, keylen, NULL);
      if (!rid) {
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_ENOMEM, 0, 0, 0);
        });
      } else {
        /* FIXME: check expire */
        grn_obj buf;
        int flags = header->qtype == MBCMD_APPEND ? GRN_OBJ_APPEND : GRN_OBJ_PREPEND;
        GRN_TEXT_INIT(&buf, GRN_OBJ_DO_SHALLOW_COPY);
        GRN_TEXT_SET_REF(&buf, value, valuelen);
        grn_obj_set_value(ctx, cache_value, rid, &buf, flags);
        GRN_MSG_MBRES({
          MBRES(ctx, re, MBRES_SUCCESS, 0, 0, 0);
        });
      }
    }
    break;
  case MBCMD_STAT :
    {
      pid_t pid = getpid();
      GRN_MSG_MBRES({
        grn_bulk_write(ctx, re, "pid", 3);
        grn_text_itoa(ctx, re, pid);
        MBRES(ctx, re, MBRES_SUCCESS, 3, 0, 0);
      });
    }
    break;
  case MBCMD_QUITQ :
    quiet = 1;
    /* fallthru */
  case MBCMD_QUIT :
    GRN_MSG_MBRES({
      MBRES(ctx, re, MBRES_SUCCESS, 0, 0, 0);
    });
    /* fallthru */
  default :
    ctx->stat = GRN_CTX_QUIT;
    break;
  }
}

/* worker thread */

enum {
  EDGE_IDLE = 0x00,
  EDGE_WAIT = 0x01,
  EDGE_DOING = 0x02,
  EDGE_ABORT = 0x03,
};

static void
check_rlimit_nofile(grn_ctx *ctx)
{
#ifndef WIN32
  struct rlimit limit;
  limit.rlim_cur = 0;
  limit.rlim_max = 0;
  getrlimit(RLIMIT_NOFILE, &limit);
  if (limit.rlim_cur < RLIMIT_NOFILE_MINIMUM) {
    limit.rlim_cur = RLIMIT_NOFILE_MINIMUM;
    limit.rlim_max = RLIMIT_NOFILE_MINIMUM;
    setrlimit(RLIMIT_NOFILE, &limit);
    limit.rlim_cur = 0;
    limit.rlim_max = 0;
    getrlimit(RLIMIT_NOFILE, &limit);
  }
  GRN_LOG(ctx, GRN_LOG_NOTICE,
          "RLIMIT_NOFILE(%" GRN_FMT_LLD ",%" GRN_FMT_LLD ")",
          (long long int)limit.rlim_cur, (long long int)limit.rlim_max);
#endif /* WIN32 */
}

static grn_thread_func_result CALLBACK
h_worker(void *arg)
{
  ht_context hc;
  grn_ctx ctx_, *ctx = &ctx_;
  grn_ctx_init(ctx, 0);
  grn_ctx_use(ctx, (grn_obj *)arg);
  grn_ctx_recv_handler_set(ctx, h_output, &hc);
  GRN_LOG(&grn_gctx, GRN_LOG_NOTICE, "thread start (%d/%d)", nfthreads, nthreads + 1);
  MUTEX_LOCK(q_mutex);
  do {
    grn_obj *msg;
    nfthreads++;
    while (!(msg = (grn_obj *)grn_com_queue_deque(&grn_gctx, &ctx_new))) {
      COND_WAIT(q_cond, q_mutex);
      if (grn_gctx.stat == GRN_CTX_QUIT) {
        nfthreads--;
        goto exit;
      }
    }
    nfthreads--;
    MUTEX_UNLOCK(q_mutex);
    hc.msg = (grn_msg *)msg;
    hc.in_body = GRN_FALSE;
    hc.is_chunked = GRN_FALSE;
    do_htreq(ctx, (grn_msg *)msg);
    MUTEX_LOCK(q_mutex);
  } while (nfthreads < max_nfthreads && grn_gctx.stat != GRN_CTX_QUIT);
exit :
  nthreads--;
  MUTEX_UNLOCK(q_mutex);
  GRN_LOG(&grn_gctx, GRN_LOG_NOTICE, "thread end (%d/%d)", nfthreads, nthreads);
  grn_ctx_fin(ctx);
  return GRN_THREAD_FUNC_RETURN_VALUE;
}

static void
h_handler(grn_ctx *ctx, grn_obj *msg)
{
  grn_com *com = ((grn_msg *)msg)->u.peer;
  if (ctx->rc) {
    grn_com_close(ctx, com);
    grn_msg_close(ctx, msg);
  } else {
    grn_sock fd = com->fd;
    void *arg = com->ev->opaque;
    /* if not keep alive connection */
    grn_com_event_del(ctx, com->ev, fd);
    ((grn_msg *)msg)->u.fd = fd;
    MUTEX_LOCK(q_mutex);
    grn_com_queue_enque(ctx, &ctx_new, (grn_com_queue_entry *)msg);
    if (!nfthreads && nthreads < max_nfthreads) {
      grn_thread thread;
      nthreads++;
      if (THREAD_CREATE(thread, h_worker, arg)) { SERR("pthread_create"); }
    }
    COND_SIGNAL(q_cond);
    MUTEX_UNLOCK(q_mutex);
  }
}

static int
h_server(char *path)
{
  int exit_code = EXIT_FAILURE;
  grn_ctx ctx_, *ctx = &ctx_;
  grn_ctx_init(ctx, 0);
  MUTEX_INIT(q_mutex);
  COND_INIT(q_cond);
  CRITICAL_SECTION_INIT(cache_lock);
  GRN_COM_QUEUE_INIT(&ctx_new);
  GRN_COM_QUEUE_INIT(&ctx_old);
  check_rlimit_nofile(ctx);
  exit_code = start_service(ctx, path, NULL, h_handler);
  grn_ctx_fin(ctx);
  return exit_code;
}

static grn_thread_func_result CALLBACK
g_worker(void *arg)
{
  GRN_LOG(&grn_gctx, GRN_LOG_NOTICE, "thread start (%d/%d)", nfthreads, nthreads + 1);
  MUTEX_LOCK(q_mutex);
  do {
    grn_ctx *ctx;
    grn_edge *edge;
    nfthreads++;
    while (!(edge = (grn_edge *)grn_com_queue_deque(&grn_gctx, &ctx_new))) {
      COND_WAIT(q_cond, q_mutex);
      if (grn_gctx.stat == GRN_CTX_QUIT) {
        nfthreads--;
        goto exit;
      }
    }
    ctx = &edge->ctx;
    nfthreads--;
    if (edge->stat == EDGE_DOING) { continue; }
    if (edge->stat == EDGE_WAIT) {
      edge->stat = EDGE_DOING;
      while (!GRN_COM_QUEUE_EMPTYP(&edge->recv_new)) {
        grn_obj *msg;
        MUTEX_UNLOCK(q_mutex);
        /* if (edge->flags == GRN_EDGE_WORKER) */
        while (ctx->stat != GRN_CTX_QUIT &&
               (edge->msg = (grn_msg *)grn_com_queue_deque(ctx, &edge->recv_new))) {
          grn_com_header *header = &edge->msg->header;
          msg = (grn_obj *)edge->msg;
          switch (header->proto) {
          case GRN_COM_PROTO_MBREQ :
            do_mbreq(ctx, edge);
            break;
          case GRN_COM_PROTO_GQTP :
            grn_ctx_send(ctx, GRN_BULK_HEAD(msg), GRN_BULK_VSIZE(msg), header->flags);
            ERRCLR(ctx);
            break;
          default :
            ctx->stat = GRN_CTX_QUIT;
            break;
          }
          grn_msg_close(ctx, msg);
        }
        while ((msg = (grn_obj *)grn_com_queue_deque(ctx, &edge->send_old))) {
          grn_msg_close(ctx, msg);
        }
        MUTEX_LOCK(q_mutex);
        if (ctx->stat == GRN_CTX_QUIT || edge->stat == EDGE_ABORT) { break; }
      }
    }
    if (ctx->stat == GRN_CTX_QUIT || edge->stat == EDGE_ABORT) {
      grn_com_queue_enque(&grn_gctx, &ctx_old, (grn_com_queue_entry *)edge);
      edge->stat = EDGE_ABORT;
    } else {
      edge->stat = EDGE_IDLE;
    }
  } while (nfthreads < max_nfthreads && grn_gctx.stat != GRN_CTX_QUIT);
exit :
  nthreads--;
  MUTEX_UNLOCK(q_mutex);
  GRN_LOG(&grn_gctx, GRN_LOG_NOTICE, "thread end (%d/%d)", nfthreads, nthreads);
  return GRN_THREAD_FUNC_RETURN_VALUE;
}

static void
g_dispatcher(grn_ctx *ctx, grn_edge *edge)
{
  MUTEX_LOCK(q_mutex);
  if (edge->stat == EDGE_IDLE) {
    grn_com_queue_enque(ctx, &ctx_new, (grn_com_queue_entry *)edge);
    edge->stat = EDGE_WAIT;
    if (!nfthreads && nthreads < max_nfthreads) {
      grn_thread thread;
      nthreads++;
      if (THREAD_CREATE(thread, g_worker, NULL)) { SERR("pthread_create"); }
    }
    COND_SIGNAL(q_cond);
  }
  MUTEX_UNLOCK(q_mutex);
}

static void
g_output(grn_ctx *ctx, int flags, void *arg)
{
  grn_edge *edge = arg;
  grn_com *com = edge->com;
  grn_msg *req = edge->msg, *msg = (grn_msg *)ctx->impl->outbuf;
  msg->edge_id = req->edge_id;
  msg->header.proto = req->header.proto == GRN_COM_PROTO_MBREQ
    ? GRN_COM_PROTO_MBRES : req->header.proto;
  if (ctx->rc != GRN_SUCCESS && GRN_BULK_VSIZE(ctx->impl->outbuf) == 0) {
    GRN_TEXT_PUTS(ctx, ctx->impl->outbuf, ctx->errbuf);
  }
  if (grn_msg_send(ctx, (grn_obj *)msg,
                   (flags & GRN_CTX_MORE) ? GRN_CTX_MORE : GRN_CTX_TAIL)) {
    edge->stat = EDGE_ABORT;
  }
  ctx->impl->outbuf = grn_msg_open(ctx, com, &edge->send_old);
}

static void
g_handler(grn_ctx *ctx, grn_obj *msg)
{
  grn_edge *edge;
  grn_com *com = ((grn_msg *)msg)->u.peer;
  if (ctx->rc) {
    if (com->has_sid) {
      if ((edge = com->opaque)) {
        MUTEX_LOCK(q_mutex);
        if (edge->stat == EDGE_IDLE) {
          grn_com_queue_enque(ctx, &ctx_old, (grn_com_queue_entry *)edge);
        }
        edge->stat = EDGE_ABORT;
        MUTEX_UNLOCK(q_mutex);
      } else {
        grn_com_close(ctx, com);
      }
    }
    grn_msg_close(ctx, msg);
  } else {
    int added;
    edge = grn_edges_add(ctx, &((grn_msg *)msg)->edge_id, &added);
    if (added) {
      grn_ctx_init(&edge->ctx, 0);
      GRN_COM_QUEUE_INIT(&edge->recv_new);
      GRN_COM_QUEUE_INIT(&edge->send_old);
      grn_ctx_use(&edge->ctx, (grn_obj *)com->ev->opaque);
      grn_ctx_recv_handler_set(&edge->ctx, g_output, edge);
      com->opaque = edge;
      grn_obj_close(&edge->ctx, edge->ctx.impl->outbuf);
      edge->ctx.impl->outbuf = grn_msg_open(&edge->ctx, com, &edge->send_old);
      edge->com = com;
      edge->stat = EDGE_IDLE;
      edge->flags = GRN_EDGE_WORKER;
    }
    if (edge->ctx.stat == GRN_CTX_QUIT || edge->stat == EDGE_ABORT) {
      grn_msg_close(ctx, msg);
    } else {
      grn_com_queue_enque(ctx, &edge->recv_new, (grn_com_queue_entry *)msg);
      g_dispatcher(ctx, edge);
    }
  }
}

static int
g_server(char *path)
{
  int exit_code = EXIT_FAILURE;
  grn_ctx ctx_, *ctx = &ctx_;
  grn_ctx_init(ctx, 0);
  MUTEX_INIT(q_mutex);
  COND_INIT(q_cond);
  CRITICAL_SECTION_INIT(cache_lock);
  GRN_COM_QUEUE_INIT(&ctx_new);
  GRN_COM_QUEUE_INIT(&ctx_old);
  check_rlimit_nofile(ctx);
  exit_code = start_service(ctx, path, g_dispatcher, g_handler);
  grn_ctx_fin(ctx);
  return exit_code;
}

enum {
  ACTION_USAGE = 1,
  ACTION_VERSION,
  ACTION_SHOW_CONFIG,
  ACTION_ERROR
};

#define ACTION_MASK       (0x0f)
#define FLAG_MODE_ALONE      (1 << 4)
#define FLAG_MODE_CLIENT     (1 << 5)
#define FLAG_MODE_DAEMON     (1 << 6)
#define FLAG_MODE_SERVER     (1 << 7)
#define FLAG_NEW_DB     (1 << 8)

static uint32_t
get_core_number(void)
{
#ifdef WIN32
  SYSTEM_INFO sinfo;
  GetSystemInfo(&sinfo);
  return sinfo.dwNumberOfProcessors;
#else /* WIN32 */
#  ifdef _SC_NPROCESSORS_CONF
  return sysconf(_SC_NPROCESSORS_CONF);
#  else
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
#  endif /* _SC_NPROCESSORS_CONF */
#endif /* WIN32 */
}

/*
 * The length of each line, including an end-of-line, in config file should be
 * shorter than (CONFIG_FILE_BUF_SIZE - 1) bytes. Too long lines are ignored.
 * Note that both '\r' and '\n' are handled as end-of-lines.
 *
 * '#' and ';' are special symbols to start comments. A comment ends with an
 * end-of-line.
 *
 * Format: name[=value]
 * - Preceding/trailing white-spaces of each line are removed.
 * - White-spaces aroung '=' are removed.
 * - name does not allow white-spaces.
 */
#define CONFIG_FILE_BUF_SIZE 4096
#define CONFIG_FILE_MAX_NAME_LENGTH 128
#define CONFIG_FILE_MAX_VALUE_LENGTH 2048

typedef enum {
  CONFIG_FILE_SUCCESS,
  CONFIG_FILE_FORMAT_ERROR,
  CONFIG_FILE_FOPEN_ERROR,
  CONFIG_FILE_MALLOC_ERROR,
  CONFIG_FILE_ATEXIT_ERROR
} config_file_status;

/*
 * The node type of a linked list for storing values. Note that a value is
 * stored in the extra space of an object.
 */
typedef struct _config_file_entry {
  struct _config_file_entry *next;
} config_file_entry;

static config_file_entry *config_file_entry_head = NULL;

static void
config_file_clear(void) {
  while (config_file_entry_head) {
    config_file_entry *next = config_file_entry_head->next;
    free(config_file_entry_head);
    config_file_entry_head = next;
  }
}

static config_file_status
config_file_register(const char *path, const grn_str_getopt_opt *opts,
                     int *flags, const char *name, size_t name_length,
                     const char *value, size_t value_length)
{
  char name_buf[CONFIG_FILE_MAX_NAME_LENGTH + 3];
  config_file_entry *entry = NULL;
  char *args[4];

  name_buf[0] = name_buf[1] = '-';
  strcpy(name_buf + 2, name);

  if (value) {
    const size_t entry_size = sizeof(config_file_entry) + value_length + 1;
    entry = (config_file_entry *)malloc(entry_size);
    if (!entry) {
      fprintf(stderr, "memory allocation failed: %u bytes\n",
              (unsigned int)entry_size);
      return CONFIG_FILE_MALLOC_ERROR;
    }
    strcpy((char *)(entry + 1), value);
    entry->next = config_file_entry_head;
    if (!config_file_entry_head) {
      if (atexit(config_file_clear)) {
        free(entry);
        return CONFIG_FILE_ATEXIT_ERROR;
      }
    }
    config_file_entry_head = entry;
  }

  args[0] = (char *)path;
  args[1] = name_buf;
  args[2] = entry ? (char *)(entry + 1) : NULL;
  args[3] = NULL;
  grn_str_getopt(entry ? 3 : 2, args, opts, flags);
  return CONFIG_FILE_SUCCESS;
}

static config_file_status
config_file_parse(const char *path, const grn_str_getopt_opt *opts,
                  int *flags, char *buf) {
  char *ptr, *name, *value;
  size_t name_length, value_length;

  while (isspace(*buf)) {
    buf++;
  }

  ptr = buf;
  while (*ptr && *ptr != '#' && *ptr != ';') {
    ptr++;
  }

  do {
    *ptr-- = '\0';
  } while (ptr >= buf && isspace(*ptr));

  if (!*buf) {
    return CONFIG_FILE_SUCCESS;
  }

  name = ptr = buf;
  while (*ptr && !isspace(*ptr) && *ptr != '=') {
    ptr++;
  }
  while (isspace(*ptr)) {
    *ptr++ = '\0';
  }

  name_length = strlen(name);
  if (name_length == 0) {
    return CONFIG_FILE_SUCCESS;
  } else if (name_length > CONFIG_FILE_MAX_NAME_LENGTH) {
    fprintf(stderr, "too long name in config file: %u bytes\n",
            (unsigned int)name_length);
    return CONFIG_FILE_FORMAT_ERROR;
  }

  if (*ptr == '=') {
    *ptr++ = '\0';
    while (isspace(*ptr)) {
      ptr++;
    }
    value = ptr;
  } else if (*ptr) {
    fprintf(stderr, "invalid name in config file\n");
    return CONFIG_FILE_FORMAT_ERROR;
  } else {
    value = NULL;
  }

  value_length = value ? strlen(value) : 0;
  if (value_length > CONFIG_FILE_MAX_VALUE_LENGTH) {
    fprintf(stderr, "too long value in config file: %u bytes\n",
            (unsigned int)value_length);
    return CONFIG_FILE_FORMAT_ERROR;
  }

  return config_file_register(path, opts, flags,
                              name, name_length, value, value_length);
}

static config_file_status
config_file_load(const char *path, const grn_str_getopt_opt *opts, int *flags)
{
  config_file_status status = CONFIG_FILE_SUCCESS;
  char buf[CONFIG_FILE_BUF_SIZE];
  size_t length = 0;
  FILE * const file = fopen(path, "rb");
  if (!file) {
    return CONFIG_FILE_FOPEN_ERROR;
  }

  for ( ; ; ) {
    int c = fgetc(file);
    if (c == '\r' || c == '\n' || c == EOF) {
      if (length < sizeof(buf) - 1) {
        buf[length] = '\0';
        status = config_file_parse(path, opts, flags, buf);
        if (status != CONFIG_FILE_SUCCESS) {
          break;
        }
      }
      length = 0;
    } else if (c == '\0') {
      fprintf(stderr, "prohibited '\\0' in config file: %s\n", path);
      status = CONFIG_FILE_FORMAT_ERROR;
      break;
    } else {
      if (length < sizeof(buf) - 1) {
        buf[length] = (char)c;
      }
      length++;
    }

    if (c == EOF) {
      break;
    }
  }

  fclose(file);
  return status;
}

static const int default_http_port = DEFAULT_HTTP_PORT;
static const int default_gqtp_port = DEFAULT_GQTP_PORT;
static grn_encoding default_encoding = GRN_ENC_DEFAULT;
static uint32_t default_max_num_threads = DEFAULT_MAX_NFTHREADS;
static const int default_log_level = GRN_LOG_DEFAULT_LEVEL;
static const char * const default_protocol = "gqtp";
static const char *default_hostname = "localhost";
static const char * const default_dest = "localhost";
static const char *default_log_path = "";
static const char *default_query_log_path = "";
static const char *default_config_path = "";
static const char *default_document_root = "";
static grn_command_version default_default_command_version =
    GRN_COMMAND_VERSION_DEFAULT;
static int64_t default_default_match_escalation_threshold = 0;
static const char * const default_bind_address = "0.0.0.0";

static void
init_default_hostname(void)
{
  static char hostname[HOST_NAME_MAX + 1];
  struct addrinfo hints, *result;

  hostname[HOST_NAME_MAX] = '\0';
  if (gethostname(hostname, HOST_NAME_MAX) == -1)
    return;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_addr = NULL;
  hints.ai_canonname = NULL;
  hints.ai_next = NULL;
  if (getaddrinfo(hostname, NULL, &hints, &result) != 0)
    return;
  freeaddrinfo(result);

  default_hostname = hostname;
}

static void
init_default_settings(void)
{
  output = stdout;

  default_encoding = grn_encoding_parse(GRN_DEFAULT_ENCODING);

  {
    const uint32_t num_cores = get_core_number();
    if (num_cores != 0) {
      default_max_num_threads = num_cores;
    }
  }

  init_default_hostname();

  default_log_path = grn_default_logger_get_path();
  default_query_log_path = grn_default_query_logger_get_path();

  default_config_path = getenv("GRN_CONFIG_PATH");
  if (!default_config_path) {
    default_config_path = GRN_CONFIG_PATH;
    if (!default_config_path) {
      default_config_path = "";
    }
  }

#ifdef WIN32
  {
    static char win32_default_document_root[PATH_MAX];
    size_t document_root_length = strlen(grn_win32_base_dir()) + 1 +
        strlen(GRN_DEFAULT_RELATIVE_DOCUMENT_ROOT) + 1;
    if (document_root_length >= PATH_MAX) {
      fprintf(stderr, "can't use default root: too long path\n");
    } else {
      strcpy(win32_default_document_root, grn_win32_base_dir());
      strcat(win32_default_document_root, "/");
      strcat(win32_default_document_root, GRN_DEFAULT_RELATIVE_DOCUMENT_ROOT);
      default_document_root = win32_default_document_root;
    }
  }
#else
  default_document_root = GRN_DEFAULT_DOCUMENT_ROOT;
#endif

  default_default_command_version = grn_get_default_command_version();
  default_default_match_escalation_threshold =
      grn_get_default_match_escalation_threshold();
}

static void
show_config(FILE *out, const grn_str_getopt_opt *opts, int flags)
{
  const grn_str_getopt_opt *o;

  for (o = opts; o->opt || o->longopt; o++) {
    switch (o->op) {
    case GETOPT_OP_NONE:
      if (o->arg && *o->arg) {
        if (o->longopt && strcmp(o->longopt, "config-path")) {
          fprintf(out, "%s=%s\n", o->longopt, *o->arg);
        }
      }
      break;
    case GETOPT_OP_ON:
      if (flags & o->flag) {
        goto no_arg;
      }
      break;
    case GETOPT_OP_OFF:
      if (!(flags & o->flag)) {
        goto no_arg;
      }
      break;
    case GETOPT_OP_UPDATE:
      if (flags == o->flag) {
      no_arg:
        if (o->longopt) {
          fprintf(out, "%s\n", o->longopt);
        }
      }
      break;
    }
  }
}

static void
show_version(void)
{
  printf("%s %s [",
         grn_get_package(),
         grn_get_version());

  /* FIXME: Should we detect host information dynamically on Windows? */
#ifdef HOST_OS
  printf("%s,", HOST_OS);
#endif
#ifdef HOST_CPU
  printf("%s,", HOST_CPU);
#endif
  printf("%s", GRN_DEFAULT_ENCODING);

  printf(",match-escalation-threshold=%" GRN_FMT_LLD,
         grn_get_default_match_escalation_threshold());

#ifndef NO_NFKC
  printf(",nfkc");
#endif
#ifdef GRN_WITH_MECAB
  printf(",mecab");
#endif
#ifdef GRN_WITH_MESSAGE_PACK
  printf(",msgpack");
#endif
#ifdef GRN_WITH_ZLIB
  printf(",zlib");
#endif
#ifdef GRN_WITH_LZ4
  printf(",lz4");
#endif
#ifdef USE_KQUEUE
  printf(",kqueue");
#endif
#ifdef USE_EPOLL
  printf(",epoll");
#endif
#ifdef USE_POLL
  printf(",poll");
#endif
  printf("]\n");

#ifdef CONFIGURE_OPTIONS
  printf("\n");
  printf("configure options: <%s>\n", CONFIGURE_OPTIONS);
#endif
}

static void
show_usage(FILE *output)
{
  uint32_t default_cache_limit = GRN_CACHE_DEFAULT_MAX_N_ENTRIES;

  fprintf(output,
          "Usage: groonga [options...] [dest]\n"
          "\n"
          "Mode options: (default: standalone)\n"
          " By default, groonga runs in standalone mode.\n"
          "  -c:   run in client mode\n"
          "  -s:   run in server mode\n"
          "  -d:   run in daemon mode\n"
          "\n"
          "Database creation options:\n"
          "  -n:                  create new database (except client mode)\n"
          "  -e, --encoding <encoding>:\n"
          "                       specify encoding for new database\n"
          "                       [none|euc|utf8|sjis|latin1|koi8r] (default: %s)\n"
          "\n"
          "Standalone/client options:\n"
          "      --file <path>:          read commands from specified file\n"
          "      --input-fd <FD>:        read commands from specified file descriptor\n"
          "                              --file has a prioriry over --input-fd\n"
          "      --output-fd <FD>:       output response to specifid file descriptor\n"
          "  -p, --port <port number>:   specify server port number (client mode only)\n"
          "                              (default: %d)\n"
          "\n"
          "Server/daemon options:\n"
          "      --bind-address <ip/hostname>:\n"
          "                                specify server address to bind\n"
          "                                (default: %s)\n"
          "  -p, --port <port number>:     specify server port number\n"
          "                                (HTTP default: %d, GQTP default: %d)\n"
          "  -i, --server-id <ip/hostname>:\n"
          "                                specify server ID address (default: %s)\n"
          "      --protocol <protocol>:    specify server protocol to listen\n"
          "                                [gqtp|http|memcached] (default: %s)\n"
          "      --document-root <path>:   specify document root path (http only)\n"
          "                                (default: %s)\n"
          "      --cache-limit <limit>:    specify max number of cache data (default: %u)\n"
          "  -t, --max-threads <max threads>:\n"
          "                                specify max number of threads (default: %u)\n"
          "      --pid-path <path>:        specify file to write process ID to\n"
          "                                (daemon mode only)\n"
          "\n"
          "Logging options:\n"
          "  -l, --log-level <log level>:\n"
          "                           specify log level (default: %d)\n"
          "      --log-path <path>:   specify log path\n"
          "                           (default: %s)\n"
          "      --query-log-path <path>:\n"
          "                           specify query log path\n"
          "                           (default: %s)\n"
          "\n"
          "Common options:\n"
          "      --working-directory <path>:\n"
          "                       specify working directory path\n"
          "                       (none)\n"
          "      --config-path <path>:\n"
          "                       specify config file path\n"
          "                       (default: %s)\n"
          "      --default-command-version <version>:\n"
          "                       specify default command version (default: %d)\n"
          "      --default-match-escalation-threshold <threshold>:\n"
          "                       specify default match escalation threshold"
          " (default: %" GRN_FMT_LLD ")\n"
          "\n"
          "      --show-config:   show config\n"
          "  -h, --help:          show usage\n"
          "      --version:       show groonga version\n"
          "\n"
          "dest:\n"
          "  <db pathname> [<commands>]: in standalone mode\n"
          "  <db pathname>: in server/daemon mode\n"
          "  <dest hostname> [<commands>]: in client mode (default: %s)\n",
          grn_encoding_to_string(default_encoding),
          default_gqtp_port, default_bind_address,
          default_http_port, default_gqtp_port, default_hostname, default_protocol,
          default_document_root, default_cache_limit, default_max_num_threads,
          default_log_level, default_log_path, default_query_log_path,
          default_config_path, default_default_command_version,
          (long long int)default_default_match_escalation_threshold,
          default_dest);
}

int
main(int argc, char **argv)
{
  const char *port_arg = NULL, *encoding_arg = NULL,
    *max_num_threads_arg = NULL, *log_level_arg = NULL,
    *bind_address_arg = NULL, *hostname_arg = NULL, *protocol_arg = NULL,
    *log_path_arg = NULL, *query_log_path_arg = NULL,
    *cache_limit_arg = NULL, *document_root_arg = NULL,
    *default_command_version_arg = NULL,
    *default_match_escalation_threshold_arg = NULL,
    *input_fd_arg = NULL, *output_fd_arg = NULL,
    *working_directory_arg = NULL;
  const char *config_path = NULL;
  int exit_code = EXIT_SUCCESS;
  int i;
  int flags = 0;
  uint32_t cache_limit = 0;
  static grn_str_getopt_opt opts[] = {
    {'p', "port", NULL, 0, GETOPT_OP_NONE},
    {'e', "encoding", NULL, 0, GETOPT_OP_NONE},
    {'t', "max-threads", NULL, 0, GETOPT_OP_NONE},
    {'h', "help", NULL, ACTION_USAGE, GETOPT_OP_UPDATE},
    {'c', NULL, NULL, FLAG_MODE_CLIENT, GETOPT_OP_ON},
    {'d', NULL, NULL, FLAG_MODE_DAEMON, GETOPT_OP_ON},
    {'s', NULL, NULL, FLAG_MODE_SERVER, GETOPT_OP_ON},
    {'l', "log-level", NULL, 0, GETOPT_OP_NONE},
    {'i', "server-id", NULL, 0, GETOPT_OP_NONE},
    {'n', NULL, NULL, FLAG_NEW_DB, GETOPT_OP_ON},
    {'\0', "protocol", NULL, 0, GETOPT_OP_NONE},
    {'\0', "version", NULL, ACTION_VERSION, GETOPT_OP_UPDATE},
    {'\0', "log-path", NULL, 0, GETOPT_OP_NONE},
    {'\0', "query-log-path", NULL, 0, GETOPT_OP_NONE},
    {'\0', "pid-path", NULL, 0, GETOPT_OP_NONE},
    {'\0', "config-path", NULL, 0, GETOPT_OP_NONE},
    {'\0', "show-config", NULL, ACTION_SHOW_CONFIG, GETOPT_OP_UPDATE},
    {'\0', "cache-limit", NULL, 0, GETOPT_OP_NONE},
    {'\0', "file", NULL, 0, GETOPT_OP_NONE},
    {'\0', "document-root", NULL, 0, GETOPT_OP_NONE},
    {'\0', "default-command-version", NULL, 0, GETOPT_OP_NONE},
    {'\0', "default-match-escalation-threshold", NULL, 0, GETOPT_OP_NONE},
    {'\0', "bind-address", NULL, 0, GETOPT_OP_NONE},
    {'\0', "input-fd", NULL, 0, GETOPT_OP_NONE},
    {'\0', "output-fd", NULL, 0, GETOPT_OP_NONE},
    {'\0', "working-directory", NULL, 0, GETOPT_OP_NONE},
    {'\0', NULL, NULL, 0, 0}
  };
  opts[0].arg = &port_arg;
  opts[1].arg = &encoding_arg;
  opts[2].arg = &max_num_threads_arg;
  opts[7].arg = &log_level_arg;
  opts[8].arg = &hostname_arg;
  opts[10].arg = &protocol_arg;
  opts[12].arg = &log_path_arg;
  opts[13].arg = &query_log_path_arg;
  opts[14].arg = &pid_file_path;
  opts[15].arg = &config_path;
  opts[17].arg = &cache_limit_arg;
  opts[18].arg = &input_path;
  opts[19].arg = &document_root_arg;
  opts[20].arg = &default_command_version_arg;
  opts[21].arg = &default_match_escalation_threshold_arg;
  opts[22].arg = &bind_address_arg;
  opts[23].arg = &input_fd_arg;
  opts[24].arg = &output_fd_arg;
  opts[25].arg = &working_directory_arg;

  reset_ready_notify_pipe();

  init_default_settings();

  /* only for parsing --config-path. */
  i = grn_str_getopt(argc, argv, opts, &flags);
  if (i < 0) {
    show_usage(stderr);
    return EXIT_FAILURE;
  }

  if (config_path) {
    const config_file_status status = config_file_load(config_path, opts, &flags);
    if (status == CONFIG_FILE_FOPEN_ERROR) {
      fprintf(stderr, "%s: can't open config file: %s (%s)\n",
              argv[0], config_path, strerror(errno));
      return EXIT_FAILURE;
    } else if (status != CONFIG_FILE_SUCCESS) {
      fprintf(stderr, "%s: failed to parse config file: %s (%s)\n",
              argv[0], config_path,
              (status == CONFIG_FILE_FORMAT_ERROR) ? "Invalid format" : strerror(errno));
      return EXIT_FAILURE;
    }
  } else if (*default_config_path) {
    const config_file_status status =
        config_file_load(default_config_path, opts, &flags);
    if (status != CONFIG_FILE_SUCCESS && status != CONFIG_FILE_FOPEN_ERROR) {
      fprintf(stderr, "%s: failed to parse config file: %s (%s)\n",
              argv[0], default_config_path,
              (status == CONFIG_FILE_FORMAT_ERROR) ? "Invalid format" : strerror(errno));
      return EXIT_FAILURE;
    }
  }

  if (working_directory_arg) {
    if (chdir(working_directory_arg) == -1) {
      fprintf(stderr, "%s: failed to change directory: %s: %s\n",
              argv[0], working_directory_arg, strerror(errno));
      return EXIT_FAILURE;
    }
  }

  /* ignore mode option in config file */
  flags = (flags == ACTION_ERROR) ? 0 : (flags & ~ACTION_MASK);

  i = grn_str_getopt(argc, argv, opts, &flags);
  if (i < 0) { flags = ACTION_ERROR; }
  switch (flags & ACTION_MASK) {
  case ACTION_VERSION :
    show_version();
    return EXIT_SUCCESS;
  case ACTION_USAGE :
    show_usage(output);
    return EXIT_SUCCESS;
  case ACTION_SHOW_CONFIG :
    show_config(output, opts, flags & ~ACTION_MASK);
    return EXIT_SUCCESS;
  case ACTION_ERROR :
    show_usage(stderr);
    return EXIT_FAILURE;
  }

  if (port_arg) {
    const char * const end = port_arg + strlen(port_arg);
    const char *rest = NULL;
    const int value = grn_atoi(port_arg, end, &rest);
    if (rest != end || value <= 0 || value > 65535) {
      fprintf(stderr, "invalid port number: <%s>\n", port_arg);
      return EXIT_FAILURE;
    }
    port = value;
  } else {
    if (protocol_arg) {
      if (*protocol_arg == 'h' || *protocol_arg == 'H') {
        port = default_http_port;
      }
    }
  }

  if (encoding_arg) {
    switch (*encoding_arg) {
    case 'n' :
    case 'N' :
      encoding = GRN_ENC_NONE;
      break;
    case 'e' :
    case 'E' :
      encoding = GRN_ENC_EUC_JP;
      break;
    case 'u' :
    case 'U' :
      encoding = GRN_ENC_UTF8;
      break;
    case 's' :
    case 'S' :
      encoding = GRN_ENC_SJIS;
      break;
    case 'l' :
    case 'L' :
      encoding = GRN_ENC_LATIN1;
      break;
    case 'k' :
    case 'K' :
      encoding = GRN_ENC_KOI8R;
      break;
    default:
      encoding = GRN_ENC_DEFAULT;
      break;
    }
  } else {
    encoding = GRN_ENC_DEFAULT;
  }

  if (!grn_document_root) {
    grn_document_root = default_document_root;
  }

  if (protocol_arg) {
    switch (*protocol_arg) {
    case 'g' :
    case 'G' :
      do_client = g_client;
      do_server = g_server;
      break;
    case 'h' :
    case 'H' :
      do_client = g_client;
      do_server = h_server;
      break;
    case 'm' :
    case 'M' :
      do_client = g_client;
      do_server = g_server;
      break;
    default :
      do_client = g_client;
      do_server = g_server;
      break;
    }
  } else {
    do_client = g_client;
    do_server = g_server;
  }

  if (log_path_arg) {
    grn_default_logger_set_path(log_path_arg);
  }

  if (query_log_path_arg) {
    grn_default_query_logger_set_path(query_log_path_arg);
  }

  if (log_level_arg) {
    const char * const end = log_level_arg + strlen(log_level_arg);
    const char *rest = NULL;
    const int value = grn_atoi(log_level_arg, end, &rest);
    if (end != rest || value < 0 || value > 9) {
      fprintf(stderr, "invalid log level: <%s>\n", log_level_arg);
      return EXIT_FAILURE;
    }
    log_level = value;
  } else {
    log_level = default_log_level;
  }
  grn_default_logger_set_max_level(log_level);

  if (max_num_threads_arg) {
    const char * const end = max_num_threads_arg + strlen(max_num_threads_arg);
    const char *rest = NULL;
    const uint32_t value = grn_atoui(max_num_threads_arg, end, &rest);
    if (end != rest || value < 1 || value > 100) {
      fprintf(stderr, "invalid max number of threads: <%s>\n",
              max_num_threads_arg);
      return EXIT_FAILURE;
    }
    max_nfthreads = value;
  } else {
    max_nfthreads = default_max_num_threads;
  }

  if (input_path) {
    if (!freopen(input_path, "r", stdin)) {
      fprintf(stderr, "can't open input file: %s (%s)\n",
              input_path, strerror(errno));
      return EXIT_FAILURE;
    }
    batchmode = GRN_TRUE;
  } else {
    if (input_fd_arg) {
      const char * const end = input_fd_arg + strlen(input_fd_arg);
      const char *rest = NULL;
      const int input_fd = grn_atoi(input_fd_arg, end, &rest);
      if (rest != end || input_fd == 0) {
        fprintf(stderr, "invalid input FD: <%s>\n", input_fd_arg);
        return EXIT_FAILURE;
      }
      if (dup2(input_fd, STDIN_FILENO) == -1) {
        fprintf(stderr, "can't open input FD: %d (%s)\n",
                input_fd, strerror(errno));
        return EXIT_FAILURE;
      }
      batchmode = GRN_TRUE;
    } else {
      if (argc - i > 1) {
        batchmode = GRN_TRUE;
      } else {
        batchmode = !isatty(0);
      }
    }
  }

  if (output_fd_arg) {
    const char * const end = output_fd_arg + strlen(output_fd_arg);
    const char *rest = NULL;
    const int output_fd = grn_atoi(output_fd_arg, end, &rest);
    if (rest != end || output_fd == 0) {
      fprintf(stderr, "invalid output FD: <%s>\n", output_fd_arg);
      return EXIT_FAILURE;
    }
    output = fdopen(output_fd, "w");
    if (!output) {
      fprintf(stderr, "can't open output FD: %d (%s)\n",
              output_fd, strerror(errno));
      return EXIT_FAILURE;
    }
  }


  if (bind_address_arg) {
    const size_t bind_address_length = strlen(bind_address_arg);
    if (bind_address_length > HOST_NAME_MAX) {
      fprintf(stderr, "too long bind address: %s (%u bytes):"
                      " must not be longer than %u bytes\n",
              bind_address_arg, (unsigned int)bind_address_length, HOST_NAME_MAX);
      return EXIT_FAILURE;
    }
    strcpy(bind_address, bind_address_arg);
  } else {
    strcpy(bind_address, default_bind_address);
  }

  if (hostname_arg) {
    const size_t hostname_length = strlen(hostname_arg);
    if (hostname_length > HOST_NAME_MAX) {
      fprintf(stderr, "too long hostname: %s (%u bytes):"
                      " must not be longer than %u bytes\n",
              hostname_arg, (unsigned int)hostname_length, HOST_NAME_MAX);
      return EXIT_FAILURE;
    }
    strcpy(hostname, hostname_arg);
  } else {
    strcpy(hostname, default_hostname);
  }

  if (document_root_arg) {
    grn_document_root = document_root_arg;
  }

  if (default_command_version_arg) {
    const char * const end = default_command_version_arg
        + strlen(default_command_version_arg);
    const char *rest = NULL;
    const int value = grn_atoi(default_command_version_arg, end, &rest);
    if (end != rest || value < GRN_COMMAND_VERSION_MIN ||
        value > GRN_COMMAND_VERSION_MAX) {
      fprintf(stderr, "invalid command version: <%s>\n",
              default_command_version_arg);
      return EXIT_FAILURE;
    }
    switch (value) {
    case 1 :
      default_command_version = GRN_COMMAND_VERSION_1;
      break;
    case 2 :
      default_command_version = GRN_COMMAND_VERSION_2;
      break;
    default :
      fprintf(stderr, "invalid command version: <%s>\n",
              default_command_version_arg);
      return EXIT_FAILURE;
    }
  } else {
    default_command_version = default_default_command_version;
  }

  if (default_match_escalation_threshold_arg) {
    const char * const end = default_match_escalation_threshold_arg
        + strlen(default_match_escalation_threshold_arg);
    const char *rest = NULL;
    const int64_t value = grn_atoll(default_match_escalation_threshold_arg, end, &rest);
    if (end != rest) {
      fprintf(stderr, "invalid match escalation threshold: <%s>\n",
              default_match_escalation_threshold_arg);
      return EXIT_FAILURE;
    }
    default_match_escalation_threshold = value;
  } else {
    default_match_escalation_threshold = default_default_match_escalation_threshold;
  }

  if (cache_limit_arg) {
    const char * const end = cache_limit_arg + strlen(cache_limit_arg);
    const char *rest = NULL;
    const uint32_t value = grn_atoui(cache_limit_arg, end, &rest);
    if (end != rest) {
      fprintf(stderr, "invalid --cache-limit value: <%s>\n", cache_limit_arg);
      return EXIT_FAILURE;
    }
    cache_limit = value;
  }

#ifdef GRN_WITH_LIBEDIT
  if (!batchmode) {
    line_editor_init(argc, argv);
  }
#endif
  if (grn_init()) { return EXIT_FAILURE; }

  grn_set_default_encoding(encoding);

  if (default_command_version_arg) {
    grn_set_default_command_version(default_command_version);
  }

  if (default_match_escalation_threshold_arg) {
    grn_set_default_match_escalation_threshold(default_match_escalation_threshold);
  }

  grn_set_segv_handler();
  grn_set_int_handler();
  grn_set_term_handler();

  if (cache_limit_arg) {
    grn_cache *cache;
    cache = grn_cache_current_get(&grn_gctx);
    grn_cache_set_max_n_entries(&grn_gctx, cache, cache_limit);
  }

  newdb = (flags & FLAG_NEW_DB);
  is_daemon_mode = (flags & FLAG_MODE_DAEMON);
  if (flags & FLAG_MODE_CLIENT) {
    exit_code = do_client(argc - i, argv + i);
  } else if (is_daemon_mode || (flags & FLAG_MODE_SERVER)) {
    exit_code = do_server(argc > i ? argv[i] : NULL);
  } else {
    exit_code = do_alone(argc - i, argv + i);
  }

#ifdef GRN_WITH_LIBEDIT
  if (!batchmode) {
    line_editor_fin();
  }
#endif
  if (output != stdout) {
    fclose(output);
  }
  grn_fin();
  return exit_code;
}
