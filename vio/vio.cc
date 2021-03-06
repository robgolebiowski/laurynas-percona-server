/* Copyright (c) 2000, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/*
  Note that we can't have assertion on file descriptors;  The reason for
  this is that during mysql shutdown, another thread can close a file
  we are working on.  In this case we should just return read errors from
  the file descriptior.
*/

#include "vio_priv.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/service_mysql_alloc.h"

#ifdef HAVE_OPENSSL
PSI_memory_key key_memory_vio_ssl_fd;
#endif

PSI_memory_key key_memory_vio;
PSI_memory_key key_memory_vio_read_buffer;

#ifdef HAVE_PSI_INTERFACE
static PSI_memory_info all_vio_memory[]=
{
#ifdef HAVE_OPENSSL
  {&key_memory_vio_ssl_fd, "ssl_fd", 0},
#endif

  {&key_memory_vio, "vio", 0},
  {&key_memory_vio_read_buffer, "read_buffer", 0},
};

void init_vio_psi_keys()
{
  const char* category= "vio";
  int count;

  count= array_elements(all_vio_memory);
  mysql_memory_register(category, all_vio_memory, count);
}
#endif

Vio *internal_vio_create(uint flags);
void internal_vio_delete(Vio *vio);

#ifdef _WIN32

/**
  Stub io_wait method that defaults to indicate that
  requested I/O event is ready.

  Used for named pipe and shared memory VIO types.

  @param vio      Unused.
  @param event    Unused.
  @param timeout  Unused.

  @retval 1       The requested I/O event has occurred.
*/

static int no_io_wait(Vio *vio MY_ATTRIBUTE((unused)),
                      enum enum_vio_io_event event MY_ATTRIBUTE((unused)),
                      int timeout MY_ATTRIBUTE((unused)))
{
  return 1;
}

#endif

extern "C" {
static my_bool has_no_data(Vio *vio MY_ATTRIBUTE((unused)))
{
  return FALSE;
}
} // extern "C"

st_vio::st_vio(uint flags)
{
  mysql_socket= MYSQL_INVALID_SOCKET;
  local= sockaddr_storage();
  remote= sockaddr_storage();
#ifdef USE_PPOLL_IN_VIO
  sigemptyset(&signal_mask);
#elif defined(HAVE_KQUEUE)
  kq_fd= -1;
#endif
  if (flags & VIO_BUFFERED_READ)
    read_buffer= (char*) my_malloc(key_memory_vio_read_buffer,
                                   VIO_READ_BUFFER_SIZE, MYF(MY_WME));
}


st_vio::~st_vio()
{
  my_free(read_buffer);
  read_buffer= nullptr;
#ifdef HAVE_KQUEUE
  if (kq_fd != -1)
    close(kq_fd);
#endif
}

/*
 * Helper to fill most of the Vio* with defaults.
 */

static bool vio_init(Vio *vio, enum enum_vio_type type,
                     my_socket sd, uint flags)
{
  DBUG_PRINT("enter vio_init", ("type: %d sd: %d  flags: %d", type, sd, flags));

  mysql_socket_setfd(&vio->mysql_socket, sd);

#ifdef HAVE_KQUEUE
  DBUG_ASSERT(type == VIO_TYPE_TCPIP || type == VIO_TYPE_SOCKET
              || type == VIO_TYPE_SSL);
  vio->kq_fd= kqueue();
  if (vio->kq_fd == -1)
  {
    DBUG_PRINT("vio_init", ("kqueue failed with errno: %d", errno));
    return true;
  }
#endif

  vio->localhost= flags & VIO_LOCALHOST;
  vio->type= type;

#ifdef _WIN32
  if (type == VIO_TYPE_NAMEDPIPE)
  {
    vio->viodelete	=vio_delete;
    vio->vioerrno	=vio_errno;
    vio->read           =vio_read_pipe;
    vio->write          =vio_write_pipe;
    vio->fastsend	=vio_fastsend;
    vio->viokeepalive	=vio_keepalive;
    vio->should_retry	=vio_should_retry;
    vio->was_timeout    =vio_was_timeout;
    vio->vioshutdown	=vio_shutdown_pipe;
    vio->peer_addr	=vio_peer_addr;
    vio->io_wait        =no_io_wait;
    vio->is_connected   =vio_is_connected_pipe;
    vio->has_data       =has_no_data;
    return false;
  }
#ifndef EMBEDDED_LIBRARY
  if (type == VIO_TYPE_SHARED_MEMORY)
  {
    vio->viodelete	=vio_delete_shared_memory;
    vio->vioerrno	=vio_errno;
    vio->read           =vio_read_shared_memory;
    vio->write          =vio_write_shared_memory;
    vio->fastsend	=vio_fastsend;
    vio->viokeepalive	=vio_keepalive;
    vio->should_retry	=vio_should_retry;
    vio->was_timeout    =vio_was_timeout;
    vio->vioshutdown	=vio_shutdown_shared_memory;
    vio->peer_addr	=vio_peer_addr;
    vio->io_wait        =no_io_wait;
    vio->is_connected   =vio_is_connected_shared_memory;
    vio->has_data       =has_no_data;
    return false;
  }
#endif /* !EMBEDDED_LIBRARY */
#endif /* _WIN32 */
#ifdef HAVE_OPENSSL
  if (type == VIO_TYPE_SSL)
  {
    vio->viodelete	=vio_ssl_delete;
    vio->vioerrno	=vio_errno;
    vio->read		=vio_ssl_read;
    vio->write		=vio_ssl_write;
    vio->fastsend	=vio_fastsend;
    vio->viokeepalive	=vio_keepalive;
    vio->should_retry	=vio_should_retry;
    vio->was_timeout    =vio_was_timeout;
    vio->vioshutdown	=vio_ssl_shutdown;
    vio->peer_addr	=vio_peer_addr;
    vio->io_wait        =vio_io_wait;
    vio->is_connected   =vio_is_connected;
    vio->has_data       =vio_ssl_has_data;
    vio->timeout        =vio_socket_timeout;
    return false;
  }
#endif /* HAVE_OPENSSL */
  vio->viodelete        =vio_delete;
  vio->vioerrno         =vio_errno;
  vio->read=            vio->read_buffer ? vio_read_buff : vio_read;
  vio->write            =vio_write;
  vio->fastsend         =vio_fastsend;
  vio->viokeepalive     =vio_keepalive;
  vio->should_retry     =vio_should_retry;
  vio->was_timeout      =vio_was_timeout;
  vio->vioshutdown      =vio_shutdown;
  vio->peer_addr        =vio_peer_addr;
  vio->io_wait          =vio_io_wait;
  vio->is_connected     =vio_is_connected;
  vio->timeout          =vio_socket_timeout;
  vio->has_data         =vio->read_buffer ? vio_buff_has_data : has_no_data;

  return false;
}


/**
  Reinitialize an existing Vio object.

  @remark Used to rebind an initialized socket-based Vio object
          to another socket-based transport type. For example,
          rebind a TCP/IP transport to SSL.

  @remark If new socket handle passed to vio_reset() is not equal
          to the socket handle stored in Vio then socket handle will
          be closed before storing new value. If handles are equal
          then old socket is not closed. This is important for
          vio_reset() usage in ssl_do().

  @remark If any error occurs then Vio members won't be altered thus
          preserving socket handle stored in Vio and not taking
          ownership over socket handle passed as parameter.

  @param vio    A VIO object.
  @param type   A socket-based transport type.
  @param sd     The socket.
  @param ssl    An optional SSL structure.
  @param flags  Flags passed to new_vio.

  @return Return value is zero on success.
*/

my_bool vio_reset(Vio* vio, enum enum_vio_type type,
                  my_socket sd, void *ssl MY_ATTRIBUTE((unused)), uint flags)
{
  int ret= FALSE;
  Vio new_vio(flags);
  DBUG_ENTER("vio_reset");

  /* The only supported rebind is from a socket-based transport type. */
  DBUG_ASSERT(vio->type == VIO_TYPE_TCPIP || vio->type == VIO_TYPE_SOCKET);

  if (vio_init(&new_vio, type, sd, flags))
    DBUG_RETURN(TRUE);

  /* Preserve perfschema info for this connection */
  new_vio.mysql_socket.m_psi= vio->mysql_socket.m_psi;

#ifdef HAVE_OPENSSL
  new_vio.ssl_arg= ssl;
#endif

  /*
    Propagate the timeout values. Necessary to also propagate
    the underlying proprieties associated with the timeout,
    such as the socket blocking mode.
  */
  if (vio->read_timeout >= 0)
    ret|= vio_timeout(&new_vio, 0, vio->read_timeout / 1000);

  if (vio->write_timeout >= 0)
    ret|= vio_timeout(&new_vio, 1, vio->write_timeout / 1000);

  if (!ret)
  {
    /*
      vio_reset() succeeded
      free old resources and then overwrite VIO structure
    */

    /*
      Close socket only when it is not equal to the new one.
    */
    if (sd != mysql_socket_getfd(vio->mysql_socket))
    {
      if (vio->inactive == false)
        vio->vioshutdown(vio);
    }
#ifdef HAVE_KQUEUE
    else
      close(vio->kq_fd);
#endif
    /*
      Overwrite existing Vio structure
      Note that the assignment operator is disabled: *vio= new_vio; 
      TODO: consider having a separate function for this,
            and consider memberwise assignment rather than memcpy!
    */
    vio->~st_vio();
    memcpy(vio, &new_vio, sizeof(*vio));
    // Disable DTOR actions.
    new_vio.read_buffer= nullptr;
#ifdef HAVE_KQUEUE
    new_vio.kq_fd= -1;
#endif
  }

  DBUG_RETURN(ret);
}


Vio *internal_vio_create(uint flags)
{
  void *rawmem= my_malloc(key_memory_vio, sizeof(st_vio), MYF(MY_WME));
  if (rawmem == nullptr)
    return nullptr;
  return new (rawmem) st_vio(flags);
}

/* Create a new VIO for socket or TCP/IP connection. */

Vio *mysql_socket_vio_new(MYSQL_SOCKET mysql_socket,
                          enum_vio_type type, uint flags)
{
  Vio *vio;
  my_socket sd= mysql_socket_getfd(mysql_socket);
  DBUG_ENTER("mysql_socket_vio_new");
  DBUG_PRINT("enter", ("sd: %d", sd));


  if ((vio= internal_vio_create(flags)))
  {
    if (vio_init(vio, type, sd, flags))
    {
      internal_vio_delete(vio);
      DBUG_RETURN(nullptr);
    }
    vio->mysql_socket= mysql_socket;
  }
  DBUG_RETURN(vio);
}

/* Open the socket or TCP/IP connection and read the fnctl() status */

Vio *vio_new(my_socket sd, enum enum_vio_type type, uint flags)
{
  Vio *vio;
  MYSQL_SOCKET mysql_socket= MYSQL_INVALID_SOCKET;
  DBUG_ENTER("vio_new");
  DBUG_PRINT("enter", ("sd: %d", sd));

  mysql_socket_setfd(&mysql_socket, sd);
  vio = mysql_socket_vio_new(mysql_socket, type, flags);

  DBUG_RETURN(vio);
}

#ifdef _WIN32

Vio *vio_new_win32pipe(HANDLE hPipe)
{
  Vio *vio;
  DBUG_ENTER("vio_new_handle");
  if ((vio= internal_vio_create(VIO_LOCALHOST)))
  {
    if (vio_init(vio, VIO_TYPE_NAMEDPIPE, 0, VIO_LOCALHOST))
    {
      internal_vio_delete(vio);
      DBUG_RETURN(nullptr);
    }

    /* Create an object for event notification. */
    vio->overlapped.hEvent= CreateEvent(NULL, FALSE, FALSE, NULL);
    if (vio->overlapped.hEvent == NULL)
    {
      internal_vio_delete(vio);
      DBUG_RETURN(NULL);
    }
    vio->hPipe= hPipe;
  }
  DBUG_RETURN(vio);
}

#ifndef EMBEDDED_LIBRARY
Vio *vio_new_win32shared_memory(HANDLE handle_file_map, HANDLE handle_map,
                                HANDLE event_server_wrote, HANDLE event_server_read,
                                HANDLE event_client_wrote, HANDLE event_client_read,
                                HANDLE event_conn_closed)
{
  Vio *vio;
  DBUG_ENTER("vio_new_win32shared_memory");
  if ((vio = internal_vio_create(VIO_LOCALHOST)))
  {
    if (vio_init(vio, VIO_TYPE_SHARED_MEMORY, 0, VIO_LOCALHOST))
    {
      internal_vio_delete(vio);
      DBUG_RETURN(nullptr);
    }
    vio->handle_file_map= handle_file_map;
    vio->handle_map= reinterpret_cast<char*>(handle_map);
    vio->event_server_wrote= event_server_wrote;
    vio->event_server_read= event_server_read;
    vio->event_client_wrote= event_client_wrote;
    vio->event_client_read= event_client_read;
    vio->event_conn_closed= event_conn_closed;
    vio->shared_memory_remain= 0;
    vio->shared_memory_pos= reinterpret_cast<char*>(handle_map);
  }
  DBUG_RETURN(vio);
}
#endif
#endif


/**
  Set timeout for a network send or receive operation.

  @note A non-infinite timeout causes the socket to be
          set to non-blocking mode. On infinite timeouts,
          the socket is set to blocking mode.

  @note A negative timeout means an infinite timeout.

  @param vio      A VIO object.
  @param which    Whether timeout is for send (1) or receive (0).
  @param timeout_sec  Timeout interval in seconds.

  @return FALSE on success, TRUE otherwise.
*/

int vio_timeout(Vio *vio, uint which, int timeout_sec)
{
  int timeout_ms;
  my_bool old_mode;

  /*
    Vio timeouts are measured in milliseconds. Check for a possible
    overflow. In case of overflow, set to infinite.
  */
  if (timeout_sec > INT_MAX/1000)
    timeout_ms= -1;
  else
    timeout_ms= (int) (timeout_sec * 1000);

  /* Deduce the current timeout status mode. */
  old_mode= vio->write_timeout < 0 && vio->read_timeout < 0;

  if (which)
    vio->write_timeout= timeout_ms;
  else
    vio->read_timeout= timeout_ms;

  /* VIO-specific timeout handling. Might change the blocking mode. */
  return vio->timeout ? vio->timeout(vio, which, old_mode) : 0;
}


void internal_vio_delete(Vio *vio)
{
  if (!vio)
    return; /* It must be safe to delete null pointers. */
  if (vio->inactive == false)
    vio->vioshutdown(vio);
  vio->~st_vio();
  my_free(vio);
}

void vio_delete(Vio* vio)
{
  internal_vio_delete(vio);
}


/*
  Cleanup memory allocated by vio or the
  components below it when application finish

*/
void vio_end(void)
{
#if defined(HAVE_YASSL)
  yaSSL_CleanUp();
#elif defined(HAVE_OPENSSL)
  vio_ssl_end();
#endif
}

struct vio_string
{
  const char * m_str;
  int m_len;
};
typedef struct vio_string vio_string;

/**
  Names for each VIO TYPE.
  Indexed by enum_vio_type.
  If you add more, please update audit_log.cc
*/
static const vio_string vio_type_names[] =
{
  { "", 0},
  { C_STRING_WITH_LEN("TCP/IP") },
  { C_STRING_WITH_LEN("Socket") },
  { C_STRING_WITH_LEN("Named Pipe") },
  { C_STRING_WITH_LEN("SSL/TLS") },
  { C_STRING_WITH_LEN("Shared Memory") },
  { C_STRING_WITH_LEN("Internal") },
  { C_STRING_WITH_LEN("Plugin") }
};

void get_vio_type_name(enum enum_vio_type vio_type, const char ** str, int * len)
{
  int index;

  if ((vio_type >= FIRST_VIO_TYPE) && (vio_type <= LAST_VIO_TYPE))
  {
    index= vio_type;
  }
  else
  {
    index= 0;
  }
  *str= vio_type_names[index].m_str;
  *len= vio_type_names[index].m_len;
  return;
}

