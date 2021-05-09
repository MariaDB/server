/* Copyright (c) 2018 MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

/* Accepting connections on Windows */

#include <my_global.h>
#include <sql_class.h>
#include <sql_connect.h>
#include <mysqld.h>
#include <mswsock.h>
#include <mysql/psi/mysql_socket.h>
#include <sddl.h>
#include <vector>
#include <handle_connections_win.h>

/* From mysqld.cc */
extern HANDLE hEventShutdown;
extern Dynamic_array<MYSQL_SOCKET> listen_sockets;
#ifdef HAVE_POOL_OF_THREADS
extern PTP_CALLBACK_ENVIRON get_threadpool_win_callback_environ();
extern void tp_win_callback_prolog();
#else
#define get_threadpool_win_callback_environ() 0
#define tp_win_callback_prolog() do{}while(0)
#endif
static SECURITY_ATTRIBUTES pipe_security;

/**
  Abstract base class for accepting new connection,
  asynchronously (i.e the accept() operation can be posted,
  and result is retrieved later) , and creating a new connection.
*/

struct Listener
{
  /** Windows handle of the Listener.
   Subclasses would use SOCKET or named pipe handle
  */
  HANDLE m_handle;
  /** Required for all async IO*/
  OVERLAPPED m_overlapped;

  /** Create new listener
  @param handle - @see m_handle
  @param wait_handle - usually, event handle or INVALID_HANDLE_VALUE 
                       @see wait_handle
  */
  Listener(HANDLE handle, HANDLE wait_handle):
    m_handle(handle), m_overlapped()
  {
      m_overlapped.hEvent= wait_handle;
  }

  /**
    if not NULL, this handle can be be used in WaitForSingle/MultipleObject(s).
    This handle will be closed when object is destroyed.

    If NULL, the completion notification happens in threadpool.
  */
  HANDLE wait_handle()
  {
    return m_overlapped.hEvent;
  }

  /* Start waiting for new client connection. */
  virtual void begin_accept()= 0;

  /**
   Completion callback,called whenever IO posted by begin_accept is finisjed
   Listener needs to create a new THD then (or, call scheduler so it creates one)

   @param success - whether IO completed successfull
  */
  virtual void completion_callback(bool success)= 0;

  /**
   Completion callback for Listener, that uses events for waiting
   to IO. Not suitable for threadpool etc. Retrieves the status of
   completed IO from the OVERLAPPED structure
  */
  void completion_callback()
  {
    DBUG_ASSERT(wait_handle() && (wait_handle() != INVALID_HANDLE_VALUE));
    DWORD bytes;
    return completion_callback(
      GetOverlappedResult(wait_handle(), &m_overlapped, &bytes, FALSE));
  }

  /** Cancel an in-progress IO. Useful for threadpool-bound IO */
  void cancel()
  {
    CancelIoEx(m_handle, &m_overlapped);
  }

  /* Destructor. Closes wait handle, if it was passed in constructor */
  virtual ~Listener()
  {
    if (m_overlapped.hEvent)
      CloseHandle(m_overlapped.hEvent);
  };
};

/* Winsock extension finctions. */
static LPFN_ACCEPTEX my_AcceptEx;
static LPFN_GETACCEPTEXSOCKADDRS my_GetAcceptExSockaddrs;

/**
  Listener that handles socket connections.
  Can be threadpool-bound (i.e the completion is executed in threadpool thread),
  or use events for waits.

  Threadpool-bound listener should be used with theradpool scheduler, for better
  performance.
*/
struct Socket_Listener: public Listener
{
  /** Client socket passed to AcceptEx() call.*/
  SOCKET m_client_socket;

  /** Listening socket. */
  MYSQL_SOCKET m_listen_socket;

  /** Buffer for sockaddrs passed to AcceptEx()/GetAcceptExSockaddrs() */
  char m_buffer[2 * sizeof(sockaddr_storage) + 32];

  /* Threadpool IO struct.*/
  PTP_IO m_tp_io;

  /**
    Callback for  Windows threadpool's StartThreadpoolIo() function.
  */
  static void CALLBACK tp_accept_completion_callback(
    PTP_CALLBACK_INSTANCE, PVOID context, PVOID , ULONG io_result,
    ULONG_PTR, PTP_IO io)
  {
    tp_win_callback_prolog();
    Listener *listener= (Listener *)context;

    if (io_result == ERROR_OPERATION_ABORTED)
    {
      /* ERROR_OPERATION_ABORTED  caused by CancelIoEx()*/
      CloseThreadpoolIo(io);
      delete listener;
      return;
    }
    listener->completion_callback(io_result == 0);
  }

  /**
  Constructor
  @param listen_socket - listening socket
  @PTP_CALLBACK_ENVIRON callback_environ  - threadpool environment, or NULL
    if threadpool is not used for completion callbacks.
  */
  Socket_Listener(MYSQL_SOCKET listen_socket, PTP_CALLBACK_ENVIRON callback_environ) :
    Listener((HANDLE)listen_socket.fd,0),
    m_client_socket(INVALID_SOCKET),
    m_listen_socket(listen_socket)
  {
    if (callback_environ)
    {
      /* Accept executed in threadpool. */
      m_tp_io= CreateThreadpoolIo(m_handle,
        tp_accept_completion_callback, this, callback_environ);
    }
    else
    {
      /* Completion signaled via event. */
      m_tp_io= 0;
      m_overlapped.hEvent= CreateEvent(0, FALSE , FALSE, 0);
    }
  }

  /*
  Use AcceptEx to asynchronously wait for new connection;
  */
  void begin_accept()
  {
retry :
    m_client_socket= socket(m_listen_socket.address_family, SOCK_STREAM,
                            IPPROTO_TCP);
    if (m_client_socket == INVALID_SOCKET)
    {
      sql_perror("socket() call failed.");
      unireg_abort(1);
    }

    DWORD bytes_received;
    if (m_tp_io)
      StartThreadpoolIo(m_tp_io);

    BOOL ret= my_AcceptEx(
      (SOCKET)m_handle,
      m_client_socket,
      m_buffer,
      0,
      sizeof(sockaddr_storage) + 16,
      sizeof(sockaddr_storage) + 16,
      &bytes_received,
      &m_overlapped);

    DWORD last_error=  ret? 0: WSAGetLastError();
    if (last_error == WSAECONNRESET || last_error == ERROR_NETNAME_DELETED)
    {
      if (m_tp_io)
        CancelThreadpoolIo(m_tp_io);
      closesocket(m_client_socket);
      goto retry;
    }

    if (ret || last_error == ERROR_IO_PENDING || abort_loop)
      return;

    sql_print_error("my_AcceptEx failed, last error %u", last_error);
    abort();
  }

  /* Create new socket connection.*/
  void completion_callback(bool success)
  {
    if (!success)
    {
      /* my_AcceptEx() returned error */
      closesocket(m_client_socket);
      begin_accept();
      return;
    }

    MYSQL_SOCKET s_client{m_client_socket};

#ifdef HAVE_PSI_SOCKET_INTERFACE
    /* Parse socket addresses buffer filled by AcceptEx(),
       only needed for PSI instrumentation. */
    sockaddr *local_addr, *remote_addr;
    int local_addr_len, remote_addr_len;

    my_GetAcceptExSockaddrs(m_buffer,
      0, sizeof(sockaddr_storage) + 16, sizeof(sockaddr_storage) + 16,
      &local_addr, &local_addr_len, &remote_addr, &remote_addr_len);

    s_client.m_psi= PSI_SOCKET_CALL(init_socket)
      (key_socket_client_connection, (const my_socket*)&m_listen_socket.fd,
       remote_addr, remote_addr_len);
#endif

    /* Start accepting new connection. After this point, do not use
    any member data, they could be used by a different (threadpool) thread. */
    begin_accept();

    /* Some chores post-AcceptEx() that we need to create a normal socket.*/
    if (setsockopt(s_client.fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
      (char *)&m_listen_socket.fd, sizeof(m_listen_socket.fd)))
    {
      if (!abort_loop)
      {
        sql_perror("setsockopt(SO_UPDATE_ACCEPT_CONTEXT) failed.");
        abort();
      }
    }

    /* Create a new connection.*/
    handle_accepted_socket(s_client, m_listen_socket);
  }

  ~Socket_Listener()
  {
    if (m_client_socket != INVALID_SOCKET)
      closesocket(m_client_socket);
  }

  /*
    Retrieve the pointer to the Winsock extension functions
    AcceptEx and GetAcceptExSockaddrs.
  */
  static void init_winsock_extensions()
  {
    if (listen_sockets.size() == 0) {
      /* --skip-networking was used*/
      return;
    }

    SOCKET s= mysql_socket_getfd(listen_sockets.at(0));
    GUID guid_AcceptEx= WSAID_ACCEPTEX;
    GUID guid_GetAcceptExSockaddrs= WSAID_GETACCEPTEXSOCKADDRS;

    GUID *guids[]= { &guid_AcceptEx, &guid_GetAcceptExSockaddrs };
    void *funcs[]= { &my_AcceptEx, &my_GetAcceptExSockaddrs };
    DWORD bytes;
    for (int i= 0; i < array_elements(guids); i++)
    {
      if (WSAIoctl(s,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        guids[i], sizeof(GUID),
        funcs[i], sizeof(void *),
        &bytes, 0, 0) == -1)
      {
        sql_print_error("WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER) failed");
        unireg_abort(1);
      }
    }
  }
};

/*
  Create a security descriptor for pipe.
  - Use low integrity level, so that it is possible to connect
  from any process.
  - Give current user read/write access to pipe.
  - Give Everyone read/write access to pipe minus FILE_CREATE_PIPE_INSTANCE
*/
static void init_pipe_security_descriptor()
{
#define SDDL_FMT "S:(ML;; NW;;; LW) D:(A;; 0x%08x;;; WD)(A;; FRFW;;; %s)"
#define EVERYONE_PIPE_ACCESS_MASK                                             \
  (FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES | READ_CONTROL |      \
   SYNCHRONIZE | FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES)

#ifndef SECURITY_MAX_SID_STRING_CHARACTERS
/* Old SDK does not have this constant */
#define SECURITY_MAX_SID_STRING_CHARACTERS 187
#endif

  /*
    Figure out SID of the user that runs the server, then create SDDL string
    for pipe permissions, and convert it to the security descriptor.
  */
  char sddl_string[sizeof(SDDL_FMT) + 8 + SECURITY_MAX_SID_STRING_CHARACTERS];
  struct
  {
    TOKEN_USER token_user;
    BYTE buffer[SECURITY_MAX_SID_SIZE];
  } token_buffer;
  HANDLE token;
  DWORD tmp;

  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    goto fail;

  if (!GetTokenInformation(token, TokenUser, &token_buffer,
                           (DWORD) sizeof(token_buffer), &tmp))
    goto fail;

  CloseHandle(token);

  char *current_user_string_sid;
  if (!ConvertSidToStringSid(token_buffer.token_user.User.Sid,
                             &current_user_string_sid))
    goto fail;

  snprintf(sddl_string, sizeof(sddl_string), SDDL_FMT,
          (unsigned int)EVERYONE_PIPE_ACCESS_MASK,
           current_user_string_sid);
  LocalFree(current_user_string_sid);

  if (ConvertStringSecurityDescriptorToSecurityDescriptor(sddl_string,
      SDDL_REVISION_1, &pipe_security.lpSecurityDescriptor, 0))
    return;

fail:
  sql_perror("Can't start server : Initialize security descriptor");
  unireg_abort(1);
}

/**
  Pipe Listener.
  Only event notification mode is implemented, no threadpool
*/
struct Pipe_Listener : public Listener
{
  PTP_CALLBACK_ENVIRON m_tp_env;
  Pipe_Listener():
    Listener(create_named_pipe(), CreateEvent(0, FALSE, FALSE, 0)),
    m_tp_env(get_threadpool_win_callback_environ())
  {
  }

  /*
  Creates local named pipe instance \\.\pipe\$socket for named pipe connection.
  */
  static HANDLE create_named_pipe()
  {
    static bool first_instance= true;
    static char pipe_name[512];
    DWORD open_mode= PIPE_ACCESS_DUPLEX |
      FILE_FLAG_OVERLAPPED;

    if (first_instance)
    {
      snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\%s", mysqld_unix_port);
      open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
      init_pipe_security_descriptor();
      pipe_security.nLength= sizeof(SECURITY_ATTRIBUTES);
      pipe_security.bInheritHandle= FALSE;
    }
    HANDLE pipe_handle= CreateNamedPipe(pipe_name,
      open_mode,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      PIPE_UNLIMITED_INSTANCES,
      (int)global_system_variables.net_buffer_length,
      (int)global_system_variables.net_buffer_length,
      NMPWAIT_USE_DEFAULT_WAIT,
      &pipe_security);
    if (pipe_handle == INVALID_HANDLE_VALUE)
    {
      sql_perror("Create named pipe failed");
      sql_print_error("Aborting");
      unireg_abort(1);
    }
    first_instance= false;
    return pipe_handle;
  }

  static void create_pipe_connection(HANDLE pipe)
  {
    if (auto connect= new CONNECT(pipe))
      create_new_thread(connect);
    else
    {
      CloseHandle(pipe);
      statistic_increment(aborted_connects, &LOCK_status);
      statistic_increment(connection_errors_internal, &LOCK_status);
    }
  }

  /* Threadpool callback.*/
  static void CALLBACK tp_create_pipe_connection(
  PTP_CALLBACK_INSTANCE,void *Context)
  {
    tp_win_callback_prolog();
    create_pipe_connection(Context);
  }

  void begin_accept()
  {
    BOOL connected= ConnectNamedPipe(m_handle, &m_overlapped);
    if (connected)
    {
      /*  Overlapped ConnectNamedPipe should return zero. */
      sql_perror("Overlapped ConnectNamedPipe() already connected.");
      abort();
    }
    DWORD last_error= GetLastError();
    switch (last_error)
    {
      case ERROR_PIPE_CONNECTED:
        /* Client is already connected, so signal an event.*/
        {
          /*
            Cleanup overlapped (so that subsequent  GetOverlappedResult()
            does not show results of previous IO
          */
          HANDLE e= m_overlapped.hEvent;
          memset(&m_overlapped, 0, sizeof(m_overlapped));
          m_overlapped.hEvent = e;
        }
        if (!SetEvent(m_overlapped.hEvent))
        {
          sql_perror("SetEvent() failed for connected pipe.");
          abort();
        }
        break;
      case ERROR_IO_PENDING:
        break;
      default:
        sql_perror("ConnectNamedPipe() failed.");
        abort();
        break;
    }
  }

  void completion_callback(bool success)
  {
    if (!success)
    {
#ifdef DBUG_OFF
      sql_print_warning("ConnectNamedPipe completed with %u", GetLastError());
#endif
      CloseHandle(m_handle);
      m_handle= create_named_pipe();
      begin_accept();
      return;
    }
    HANDLE pipe= m_handle;
    m_handle= create_named_pipe();
    begin_accept();
    // If threadpool is on, create connection in threadpool thread
    if (!m_tp_env || !TrySubmitThreadpoolCallback(tp_create_pipe_connection, pipe, m_tp_env))
      create_pipe_connection(pipe);
  }

  ~Pipe_Listener()
  {
    if (m_handle != INVALID_HANDLE_VALUE)
    {
      CloseHandle(m_handle);
    }
  }

  static void cleanup()
  {
    LocalFree(pipe_security.lpSecurityDescriptor);
  }
};

 /* The shutdown event, which is set whenever*/
static void create_shutdown_event()
{
  char shutdown_event_name[40];
  sprintf_s(shutdown_event_name, "MySQLShutdown%u", GetCurrentProcessId());
  if (!(hEventShutdown= CreateEvent(0, FALSE, FALSE, shutdown_event_name)))
  {
    sql_print_error("Can't create shutdown event, Windows error %u", GetLastError());
    unireg_abort(1);
  }
}

/**
  Accept new client connections on Windows.

  Since we deal with pipe and sockets, they cannot be put into a select/loop.
  But we can use asynchronous IO, and WaitForMultipleObject() loop.

  In addition, for slightly better performance, if we're using threadpool,
  socket connections are accepted directly in the threadpool.

  The mode of operation is therefore

  1. There is WaitForMultipleObject() loop that waits for shutdown notification
  (hEventShutdown),and possibly pipes and sockets(e.g if threadpool is not used)
  This loop ends when shutdown notification is detected.

  2. If threadpool is used, new socket connections are accepted there.
*/


#define NUM_PIPE_LISTENERS 24
#define SHUTDOWN_IDX 0
#define LISTENER_START_IDX 1

static std::vector<Listener *> all_listeners;
static std::vector<HANDLE> wait_events;

void network_init_win()
{
  Socket_Listener::init_winsock_extensions();

  /* Listen for TCP connections on "extra-port" (no threadpool).*/
  for (uint i= 0 ; i < listen_sockets.elements() ; i++)
  {
    MYSQL_SOCKET *sock= listen_sockets.get_pos(i);
    if (sock->is_extra_port)
      all_listeners.push_back(new Socket_Listener(*sock, 0));
  }

  /* Listen for named pipe connections */
  if (mysqld_unix_port[0] && !opt_bootstrap && opt_enable_named_pipe)
  {
    /*
      Use several listeners for pipe, to reduce ERROR_PIPE_BUSY on client side.
    */
    for (int i= 0; i < NUM_PIPE_LISTENERS; i++)
      all_listeners.push_back(new Pipe_Listener());
  }

  for (uint i= 0 ; i < listen_sockets.elements() ; i++)
  {
    MYSQL_SOCKET *sock= listen_sockets.get_pos(i);
    if (sock->is_extra_port)
      continue;
    /* Wait for TCP connections.*/
    SetFileCompletionNotificationModes((HANDLE) sock->fd,
                                       FILE_SKIP_SET_EVENT_ON_HANDLE);
    all_listeners.push_back(
      new Socket_Listener(*sock, get_threadpool_win_callback_environ()));
  }

  if (all_listeners.size() == 0 && !opt_bootstrap)
  {
    sql_print_error("Either TCP connections or named pipe connections must be enabled.");
    unireg_abort(1);
  }
}

void handle_connections_win()
{
  int n_waits;

  create_shutdown_event();
  wait_events.push_back(hEventShutdown);
  n_waits= 1;

  for (size_t i= 0; i < all_listeners.size(); i++)
  {
    HANDLE wait_handle= all_listeners[i]->wait_handle();
    if (wait_handle)
    {
      DBUG_ASSERT((i == 0) || (all_listeners[i - 1]->wait_handle() != 0));
      wait_events.push_back(wait_handle);
    }
    all_listeners[i]->begin_accept();
  }

  mysqld_win_set_startup_complete();

  // WaitForMultipleObjects can't wait on more than MAXIMUM_WAIT_OBJECTS
  // handles simultaneously. Since MAXIMUM_WAIT_OBJECTS is only 64, there is
  // a theoretical possiblity of exceeding that limit on installations where
  // host name resolves to a lot of addresses.
  if (wait_events.size() > MAXIMUM_WAIT_OBJECTS)
  {
    sql_print_warning(
      "Too many wait events (%lu). Some connection listeners won't be handled. "
      "Try to switch \"thread-handling\" to \"pool-of-threads\" and/or disable "
      "\"extra-port\".", static_cast<ulong>(wait_events.size()));
    wait_events.resize(MAXIMUM_WAIT_OBJECTS);
  }

  for (;;)
  {
    DBUG_ASSERT(wait_events.size() <= MAXIMUM_WAIT_OBJECTS);
    DWORD idx = WaitForMultipleObjects((DWORD)wait_events.size(),
                                       wait_events.data(), FALSE, INFINITE);
    DBUG_ASSERT((int)idx >= 0 && (int)idx < (int)wait_events.size());

    if (idx == SHUTDOWN_IDX)
      break;

    all_listeners[idx - LISTENER_START_IDX]->completion_callback();
  }

  mysqld_win_initiate_shutdown();

  /* Cleanup */
  for (size_t i= 0; i < all_listeners.size(); i++)
  {
    Listener *listener= all_listeners[i];
    if (listener->wait_handle())
      delete listener;
    else
      // Threadpool-bound listener will be deleted in threadpool
      // Do not call destructor, because callback maybe running.
      listener->cancel();
  }
  Pipe_Listener::cleanup();
}
