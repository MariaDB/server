/*
  Copyright (c) 2026 MariaDB

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA.
*/

#include <cstdint>
#include <optional> // Remote_event_stream::Connection_options::ssl_options
#include <string_view> // Return type of Remote_event_stream::next()

using MYSQL= struct st_mysql;
using MYSQL_RES_P= struct st_mysql_res *;


struct Rpl_slave_connection
{
  ///@deprecated won't need this with @ref Semi_sync_graceful_killer gone
  struct Connection_options
  {
    char *host, *user, *password;
    unsigned int port;
    struct SSL_options
    {
      const char
        *ssl_ca, *ssl_capath,
        *ssl_cert,
        *ssl_crl, *ssl_crlpath,
        *ssl_key,
        *ssl_cipher;
      bool ssl_verify_server_cert;
    };
    std::optional<SSL_options> ssl_options; ///< Disable SSL with std::nullopt
    const char *charset_name;
    /// In case the master asks for an external authentication plugin
    const char *plugin_dir;
    void operator ()(MYSQL *connector) const;
  };

  operator bool() { return connector; }
  ///@deprecated TODO: hide this behind reusable helpers @{
    ///@return `false` on success, or `true` on error
    bool connect(unsigned long flags= 0);
    bool real_query(const char *query, size_t strlen);
  /// }@
protected:
  /**TODO:
    Replace this opaque pointer with `std::optional<MYSQL>`
    when `../sql-common` is completely phased out.
  */
  MYSQL *connector;
  /**
    @note This does not invoke the setup methods; do so before calling next().
      (TODO: clean up the rest of `sql/slave.cc`'s `handle_slave_io()` &
       sub-procedures so they don't have to be manual.)
    @post operator bool() is `true` on success or `false` on OOM.
  */
  Rpl_slave_connection(const Connection_options &options, uint32_t timeout);
  ~Rpl_slave_connection();
};


struct Remote_event_stream: Rpl_slave_connection
{
  bool do_reconnect= false;
  Remote_event_stream(const Connection_options &options,
                      uint32_t timeout, unsigned long max_allowed_packet);

  /** Ordered setup commands and error status getters
    @deprecated TODO: hide these behind helper
    methods of protected Chain of Command classes @{
  */
    unsigned int errnum();
    const char *errmsg();
    /**
      (Re)connect to the remote
      @return `false` on success, or `true` on error
    */
    bool connect(bool compress= false);
    ///@deprecated MySQL 4.x and before are long EOL.
    unsigned long master_version();
    MYSQL_RES_P store_result();
    static char **fetch_row(MYSQL_RES_P query_result);
    static void free_result(MYSQL_RES_P query_result);
    /**TODO:
      merge `register_slave_on_master()` & `request_dump()`
      in `sql/slave.cc` and `dump_remote_log_entries()` in
      `client/mysqlbinlog.cc` with Connector/C's mariadb_rpl_open()
      @return `false` on success, or `true` on error
    */
    bool send_command(
      int command, const unsigned char *args, size_t strlen, bool skip_check);
  /// }@

  unsigned long thread_id(); /// for Semi_sync_graceful_killer
  /** @return a connector-managed string, which can be
    * the next event
    * an EOF packet
    * an empty string, which represents error
  */
  std::string_view next();
  /**
    @return `false` on success, or `true` on error
    @note Acknowledgement comes after the caller has safely recorded the
          event from next(); this method is therefore separate from next().
  */
  bool semisync_ack(const std::string_view log_name, uint64_t next_pos);
  /**
    Force-close the stream. It remains existing for recovery,
    but all ongoing and subsequent operations will error.
    @note Call this from another thread _with additional mutexing_
  */
  void abort();
};


///@deprecated obsolete; remove in MDEV-39583
struct Semi_sync_graceful_killer: Rpl_slave_connection
{
  Semi_sync_graceful_killer(
    const Connection_options &options, uint32_t timeout);
};
