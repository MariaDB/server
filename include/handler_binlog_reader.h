#ifndef HANDLER_BINLOG_READER_INCLUDED
#define HANDLER_BINLOG_READER_INCLUDED

/* Copyright (c) 2025, Kristian Nielsen.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


class String;
class THD;
struct slave_connection_state;
struct rpl_binlog_state_base;


/*
  Class for reading a binlog implemented in an engine.
*/
class handler_binlog_reader {
public:
  /*
    Approximate current position (from which next call to read_binlog_data()
    will need to read). Updated by the engine. Used to know which binlog files
    the active dump threads are currently reading from, to avoid purging
    actively used binlogs.
  */
  uint64_t cur_file_no;
  uint64_t cur_file_pos;

private:
  /* Position and length of any remaining data in buf[]. */
  uint32_t buf_data_pos;
  uint32_t buf_data_remain;
  /* Buffer used when reading data out via read_binlog_data(). */
  static constexpr size_t BUF_SIZE= 32768;
  uchar *buf;

public:
  handler_binlog_reader()
    : cur_file_no(~(uint64_t)0), cur_file_pos(~(uint64_t)0),
      buf_data_pos(0), buf_data_remain(0)
  {
    buf= (uchar *)my_malloc(PSI_INSTRUMENT_ME, BUF_SIZE, MYF(0));
  }
  virtual ~handler_binlog_reader() {
    my_free(buf);
  };
  virtual int read_binlog_data(uchar *buf, uint32_t len) = 0;
  virtual bool data_available()= 0;
  /*
    Wait for data to be available to read, for kill, or for timeout.
    Returns true in case of timeout reached, false otherwise.
    Caller should check for kill before calling again (to avoid busy-loop).
  */
  virtual bool wait_available(THD *thd, const struct timespec *abstime) = 0;
  /*
    This initializes the current read position to the point of the slave GTID
    position passed in as POS. It is permissible to start at a position a bit
    earlier in the binlog, only cost is the extra read cost of reading not
    needed event data.

    If position is found, must return the corresponding binlog state in the
    STATE output parameter and initialize cur_file_no and cur_file_pos members.

    Returns:
     -1  Error
      0  The requested GTID position not found, needed binlogs have been purged
      1  Ok, position found and returned.
  */
  virtual int init_gtid_pos(THD *thd, slave_connection_state *pos,
                            rpl_binlog_state_base *state) = 0;
  /*
    Initialize to a legacy-type position (filename, offset). This mostly to
    support legacy SHOW BINLOG EVENTS.
  */
  virtual int init_legacy_pos(THD *thd, const char *filename,
                              ulonglong offset) = 0;
  /*
    Can be called after init_gtid_pos() or init_legacy_pos() to make the reader
    stop (return EOF) at the end of the binlog file. Used for SHOW BINLOG
    EVENTS, which has a file-based interface based on legacy file name.
  */
  virtual void enable_single_file() = 0;
  int read_log_event(String *packet, uint32_t ev_offset, size_t max_allowed);
};

#endif  /* HANDLER_BINLOG_READER_INCLUDED */
