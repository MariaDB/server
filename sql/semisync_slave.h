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

/**
   The extension class for the slave of semi-synchronous replication
*/
class ReplSemiSyncSlave
  :public ReplSemiSyncBase {
public:
 ReplSemiSyncSlave()
   :slave_enabled_(false)
  {}
  ~ReplSemiSyncSlave() {}

  void setTraceLevel(unsigned long trace_level) {
    trace_level_ = trace_level;
  }

  /* Initialize this class after MySQL parameters are initialized. this
   * function should be called once at bootstrap time.
   */
  int initObject();

  bool getSlaveEnabled() {
    return slave_enabled_;
  }
  void setSlaveEnabled(bool enabled) {
    run_hooks_enabled= !enabled;  // plugin "dynamic" hooks not to run when semisync ON
    slave_enabled_ = enabled;
  }
 
  bool isDelayMaster(){
    return delay_master_;
  }

  void setDelayMaster(bool enabled) {
    delay_master_ = enabled;
  }

  void setKillConnTimeout(unsigned int timeout) {
    kill_conn_timeout_ = timeout;
  }

  /* A slave reads the semi-sync packet header and separate the metadata
   * from the payload data.
   *
   * Input:
   *  header      - (IN)  packet header pointer
   *  total_len   - (IN)  total packet length: metadata + payload

MERGE>  TODO: integrate with  MDEV-162
   *  semi_flags  - (IN)  store flags: SEMI_SYNC_SLAVE_DELAY_SYNC and SEMI_SYNC_NEED_ACK

   *  payload     - (IN)  payload: the replication event
   *  payload_len - (IN)  payload length
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int slaveReadSyncHeader(const char *header, unsigned long total_len, int *semi_flags,
                          const char **payload, unsigned long *payload_len);

  /* A slave replies to the master indicating its replication process.  It
   * indicates that the slave has received all events before the specified
   * binlog position.
   */
  int slaveReply(Master_info* mi);
  int slaveStart(Master_info *mi);
  int slaveStop(Master_info *mi);
  int requestTransmit(Master_info*);
  void killConnection(MYSQL *mysql);
  int resetSlave(Master_info *mi);

private:
  /* True when initObject has been called */
  bool init_done_;
  bool slave_enabled_;        /* semi-sycn is enabled on the slave */
  bool delay_master_;
  unsigned int kill_conn_timeout_;
};


/* System and status variables for the slave component */
extern my_bool rpl_semi_sync_slave_enabled;
extern my_bool rpl_semi_sync_slave_status;
extern ulong rpl_semi_sync_slave_trace_level;
extern ReplSemiSyncSlave repl_semisync_slave;

extern char rpl_semi_sync_slave_delay_master;
extern unsigned int rpl_semi_sync_slave_kill_conn_timeout;
extern unsigned long long rpl_semi_sync_slave_send_ack;

int semi_sync_slave_init();
void semi_sync_slave_deinit();

#endif /* SEMISYNC_SLAVE_H */
