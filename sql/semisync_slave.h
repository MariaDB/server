/* Copyright (c) 2006 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#ifndef SEMISYNC_SLAVE_H
#define SEMISYNC_SLAVE_H

#include "semisync.h"
#include "my_global.h"
#include "sql_priv.h"
#include "rpl_mi.h"
#include "mysql.h"

class Master_info;

/**
   The extension class for the slave of semi-synchronous replication
*/
class Repl_semi_sync_slave
  :public Repl_semi_sync_base {
public:
 Repl_semi_sync_slave() :m_slave_enabled(false) {}
  ~Repl_semi_sync_slave() {}

  void set_trace_level(unsigned long trace_level) {
    m_trace_level = trace_level;
  }

  /* Initialize this class after MySQL parameters are initialized. this
   * function should be called once at bootstrap time.
   */
  int init_object();

  bool get_slave_enabled() {
    return m_slave_enabled;
  }

  void set_slave_enabled(bool enabled) {
    m_slave_enabled = enabled;
  }

  bool is_delay_master(){
    return m_delay_master;
  }

  void set_delay_master(bool enabled) {
    m_delay_master = enabled;
  }

  void set_kill_conn_timeout(unsigned int timeout) {
    m_kill_conn_timeout = timeout;
  }

  /* A slave reads the semi-sync packet header and separate the metadata
   * from the payload data.
   *
   * Input:
   *  header      - (IN)  packet header pointer
   *  total_len   - (IN)  total packet length: metadata + payload
   *  semi_flags  - (IN)  store flags: SEMI_SYNC_SLAVE_DELAY_SYNC and
                          SEMI_SYNC_NEED_ACK
   *  payload     - (IN)  payload: the replication event
   *  payload_len - (IN)  payload length
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int slave_read_sync_header(const uchar *header, unsigned long total_len,
                             int *semi_flags,
                             const uchar **payload, unsigned long *payload_len);

  /* A slave replies to the master indicating its replication process.  It
   * indicates that the slave has received all events before the specified
   * binlog position.
   */
  int slave_reply(Master_info* mi);
  int slave_start(Master_info *mi);
  int slave_stop(Master_info *mi);
  int request_transmit(Master_info*);
  void kill_connection(MYSQL *mysql);
  int reset_slave(Master_info *mi);

private:
  /* True when init_object has been called */
  bool m_init_done;
  bool m_slave_enabled;        /* semi-sycn is enabled on the slave */
  bool m_delay_master;
  unsigned int m_kill_conn_timeout;
};


/* System and status variables for the slave component */
extern my_bool rpl_semi_sync_slave_enabled;
extern my_bool rpl_semi_sync_slave_status;
extern ulong rpl_semi_sync_slave_trace_level;
extern Repl_semi_sync_slave repl_semisync_slave;

extern char rpl_semi_sync_slave_delay_master;
extern unsigned int rpl_semi_sync_slave_kill_conn_timeout;
extern unsigned long long rpl_semi_sync_slave_send_ack;

#endif /* SEMISYNC_SLAVE_H */
