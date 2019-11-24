import cStringIO
import array
import hashlib
import MySQLdb
from MySQLdb.constants import CR
from MySQLdb.constants import ER
from collections import deque
import os
import random
import signal
import sys
import threading
import time
import string
import traceback
import logging
import argparse

# This is a generic load_generator for mysqld which persists across server
# restarts and attempts to verify both committed and uncommitted transactions
# are persisted correctly.
#
# The table schema used should look something like:
#
# CREATE TABLE t1(id INT PRIMARY KEY,
#                 thread_id INT NOT NULL,
#                 request_id BIGINT UNSIGNED NOT NULL,
#                 update_count INT UNSIGNED NOT NULL DEFAULT 0,
#                 zero_sum INT DEFAULT 0,
#                 msg VARCHAR(1024),
#                 msg_length int,
#                 msg_checksum varchar(128),
#                 KEY msg_i(msg(255), zero_sum))
# ENGINE=RocksDB DEFAULT CHARSET=latin1 COLLATE=latin1_bin;
#
#   zero_sum should always sum up to 0 regardless of when the transaction tries
#   to process the transaction. Each transaction always maintain this sum to 0.
#
#   request_id should be unique across transactions. It is used during
#   transaction verification and is monotonically increasing..
#
# Several threads are spawned at the start of the test to populate the table.
# Once the table is populated, both loader and checker threads are created.
#
# The row id space is split into two sections: exclusive and shared. Each
# loader thread owns some part of the exclusive section which it maintains
# complete information on insert/updates/deletes. Since this section is only
# modified by one thread, the thread can maintain an accurate picture of all
# changes. The shared section contains rows which multiple threads can
# update/delete/insert.  For checking purposes, the request_id is used to
# determine if a row is consistent with a committed transaction.
#
# Each loader thread's transaction consists of selecting some number of rows
# randomly. The thread can choose to delete the row, update the row or insert
# the row if it doesn't exist.  The state of rows that are owned by the loader
# thread are tracked within the thread's id_map. This map contains the row id
# and the request_id of the latest update. For indicating deleted rows, the
# -request_id marker is used. Thus, at any point in time, the thread's id_map
# should reflect the exact state of the rows that are owned.
#
# The loader thread also maintains the state of older transactions that were
# successfully processed in addition to the current transaction, which may or
# may not be committed. Each transaction state consists of the row id, and the
# request_id. Again, -request_id is used to indicate a delete. For committed
# transactions, the thread can verify the request_id of the row is larger than
# what the thread has recorded. For uncommitted transactions, the thread would
# verify the request_id of the row does not match that of the transaction. To
# determine whether or not a transaction succeeded in case of a crash right at
# commit, each thread always includes a particular row in the transaction which
# it could use to check the request id against.
#
# Checker threads run continuously to verify the checksums on the rows and to
# verify the zero_sum column sums up to zero at any point in time. The checker
# threads run both point lookups and range scans for selecting the rows.

class ValidateError(Exception):
  """Raised when validation fails"""
  pass

class TestError(Exception):
  """Raised when the test cannot make forward progress"""
  pass

CHARS = string.letters + string.digits
OPTIONS = {}

# max number of rows per transaction
MAX_ROWS_PER_REQ = 10

# global variable checked by threads to determine if the test is stopping
TEST_STOP = False
LOADERS_READY = 0

# global monotonically increasing request id counter
REQUEST_ID = 1
REQUEST_ID_LOCK = threading.Lock()

INSERT_ID_SET = set()

def get_next_request_id():
  global REQUEST_ID
  with REQUEST_ID_LOCK:
    REQUEST_ID += 1
    return REQUEST_ID

# given a percentage value, rolls a 100-sided die and return whether the
# given value is above or equal to the die roll
#
# passing 0 should always return false and 100 should always return true
def roll_d100(p):
  assert p >= 0 and p <= 100
  return p >= random.randint(1, 100)

def sha1(x):
  return hashlib.sha1(str(x)).hexdigest()

def is_connection_error(exc):
  error_code = exc.args[0]
  return (error_code == MySQLdb.constants.CR.CONNECTION_ERROR or
          error_code == MySQLdb.constants.CR.CONN_HOST_ERROR or
          error_code == MySQLdb.constants.CR.SERVER_LOST or
          error_code == MySQLdb.constants.CR.SERVER_GONE_ERROR or
          error_code == MySQLdb.constants.ER.QUERY_INTERRUPTED or
          error_code == MySQLdb.constants.ER.SERVER_SHUTDOWN)

def is_deadlock_error(exc):
  error_code = exc.args[0]
  return (error_code == MySQLdb.constants.ER.LOCK_DEADLOCK or
          error_code == MySQLdb.constants.ER.LOCK_WAIT_TIMEOUT)

# should be deterministic given an idx
def gen_msg(idx, thread_id, request_id):
  random.seed(idx);
  # field length is 1024 bytes, but 32 are reserved for the tid and req tag
  blob_length = random.randint(1, 1024 - 32)

  if roll_d100(50):
    # blob that cannot be compressed (well, compresses to 85% of original size)
    msg = ''.join([random.choice(CHARS) for x in xrange(blob_length)])
  else:
    # blob that can be compressed
    msg = random.choice(CHARS) * blob_length

  # append the thread_id and request_id to the end of the msg
  return ''.join([msg, ' tid: %d req: %d' % (thread_id, request_id)])

def execute(cur, stmt):
  ROW_COUNT_ERROR = 18446744073709551615L
  logging.debug("Executing %s" % stmt)
  cur.execute(stmt)
  if cur.rowcount < 0 or cur.rowcount == ROW_COUNT_ERROR:
    raise MySQLdb.OperationalError(MySQLdb.constants.CR.CONNECTION_ERROR,
                                   "Possible connection error, rowcount is %d"
                                   % cur.rowcount)

def wait_for_workers(workers, min_active = 0):
  logging.info("Waiting for %d workers", len(workers))
  # min_active needs to include the current waiting thread
  min_active += 1

  # polling here allows this thread to be responsive to keyboard interrupt
  # exceptions, otherwise a user hitting ctrl-c would see the load_generator as
  # hanging and unresponsive
  try:
    while threading.active_count() > min_active:
      time.sleep(1)
  except KeyboardInterrupt, e:
    os._exit(1)

  num_failures = 0
  for w in workers:
    w.join()
    if w.exception:
      logging.error(w.exception)
      num_failures += 1

  return num_failures

# base class for worker threads and contains logic for handling reconnecting to
# the mysqld server during connection failure
class WorkerThread(threading.Thread):
  def __init__(self, name):
    threading.Thread.__init__(self)
    self.name = name
    self.exception = None
    self.con = None
    self.cur = None
    self.isolation_level = None
    self.start_time = time.time()
    self.total_time = 0

  def run(self):
    global TEST_STOP

    try:
      logging.info("Started")
      self.runme()
      logging.info("Completed successfully")
    except Exception, e:
      self.exception = traceback.format_exc()
      logging.error(self.exception)
      TEST_STOP = True
    finally:
      self.total_time = time.time() - self.start_time
      logging.info("Total run time: %.2f s" % self.total_time)
      self.finish()

  def reconnect(self, timeout=900):
    global TEST_STOP

    self.con = None
    SECONDS_BETWEEN_RETRY = 10
    attempts = 1
    logging.info("Attempting to connect to MySQL Server")
    while not self.con and timeout > 0 and not TEST_STOP:
      try:
        self.con = MySQLdb.connect(user=OPTIONS.user, host=OPTIONS.host,
                                   port=OPTIONS.port, db=OPTIONS.db)
        if self.con:
          self.con.autocommit(False)
          self.cur = self.con.cursor()
          self.set_isolation_level(self.isolation_level)
          logging.info("Connection successful after attempt %d" % attempts)
          break
      except MySQLdb.Error, e:
        logging.debug(traceback.format_exc())
      time.sleep(SECONDS_BETWEEN_RETRY)
      timeout -= SECONDS_BETWEEN_RETRY
      attempts += 1
    return self.con is None

  def get_isolation_level(self):
    execute(self.cur, "SELECT @@SESSION.tx_isolation")
    if self.cur.rowcount != 1:
      raise TestError("Unable to retrieve tx_isolation")
    return self.cur.fetchone()[0]

  def set_isolation_level(self, isolation_level, persist = False):
    if isolation_level is not None:
      execute(self.cur, "SET @@SESSION.tx_isolation = '%s'" % isolation_level)
      if self.cur.rowcount != 0:
        raise TestError("Unable to set the isolation level to %s")

    if isolation_level is None or persist:
      self.isolation_level = isolation_level

# periodically kills the server
class ReaperWorker(WorkerThread):
  def __init__(self):
    WorkerThread.__init__(self, 'reaper')
    self.start()
    self.kills = 0

  def finish(self):
    logging.info('complete with %d kills' % self.kills)
    if self.con:
      self.con.close()

  def get_server_pid(self):
    execute(self.cur, "SELECT @@pid_file")
    if self.cur.rowcount != 1:
      raise TestError("Unable to retrieve pid_file")
    return int(open(self.cur.fetchone()[0]).read())

  def runme(self):
    global TEST_STOP
    time_remain = random.randint(10, 30)
    while not TEST_STOP:
      if time_remain > 0:
        time_remain -= 1
        time.sleep(1)
        continue
      if self.reconnect():
        raise Exception("Unable to connect to MySQL server")
      logging.info('killing server...')
      with open(OPTIONS.expect_file, 'w+') as expect_file:
        expect_file.write('restart')
      os.kill(self.get_server_pid(), signal.SIGTERM)
      self.kills += 1
      time_remain = random.randint(0, 30) + OPTIONS.reap_delay;

# runs initially to populate the table with the given number of rows
class PopulateWorker(WorkerThread):
  def __init__(self, thread_id, start_id, num_to_add):
    WorkerThread.__init__(self, 'populate-%d' % thread_id)
    self.thread_id = thread_id
    self.start_id = start_id
    self.num_to_add = num_to_add
    self.table = OPTIONS.table
    self.start()

  def finish(self):
    if self.con:
      self.con.commit()
      self.con.close()

  def runme(self):
    if self.reconnect():
      raise Exception("Unable to connect to MySQL server")

    stmt = None
    for i in xrange(self.start_id, self.start_id + self.num_to_add):
      stmt = gen_insert(self.table, i, 0, 0, 0)
      execute(self.cur, stmt)
      if i % 101 == 0:
        self.con.commit()
        check_id(self.con.insert_id())
    self.con.commit()
    check_id(self.con.insert_id())
    logging.info("Inserted %d rows starting at id %d" %
                 (self.num_to_add, self.start_id))

def check_id(id):
  if id == 0:
    return
  if id in INSERT_ID_SET:
    raise Exception("Duplicate auto_inc id %d" % id)
  INSERT_ID_SET.add(id)

def populate_table(num_records):

  logging.info("Populate_table started for %d records" % num_records)
  if num_records == 0:
    return False

  num_workers = min(10, num_records / 100)
  workers = []

  N = num_records / num_workers
  start_id = 0
  for i in xrange(num_workers):
     workers.append(PopulateWorker(i, start_id, N))
     start_id += N
  if num_records > start_id:
    workers.append(PopulateWorker(num_workers, start_id,
                   num_records - start_id))

  # Wait for the populate threads to complete
  return wait_for_workers(workers) > 0

def gen_insert(table, idx, thread_id, request_id, zero_sum):
  msg = gen_msg(idx, thread_id, request_id)
  return ("INSERT INTO %s (id, thread_id, request_id, zero_sum, "
          "msg, msg_length, msg_checksum) VALUES (%d,%d,%d,%d,'%s',%d,'%s')"
           % (table, idx, thread_id, request_id,
              zero_sum, msg, len(msg), sha1(msg)))

def gen_update(table, idx, thread_id, request_id, zero_sum):
  msg = gen_msg(idx, thread_id, request_id)
  return ("UPDATE %s SET thread_id = %d, request_id = %d, "
          "update_count = update_count + 1, zero_sum = zero_sum + (%d), "
          "msg = '%s', msg_length = %d, msg_checksum = '%s' WHERE id = %d "
          % (table, thread_id, request_id, zero_sum, msg, len(msg),
             sha1(msg), idx))

def gen_delete(table, idx):
    return "DELETE FROM %s WHERE id = %d" % (table, idx)

def gen_insert_on_dup(table, idx, thread_id, request_id, zero_sum):
  msg = gen_msg(idx, thread_id, request_id)
  msg_checksum = sha1(msg)
  return ("INSERT INTO %s (id, thread_id, request_id, zero_sum, "
          "msg, msg_length, msg_checksum) VALUES (%d,%d,%d,%d,'%s',%d,'%s') "
          "ON DUPLICATE KEY UPDATE "
          "thread_id=%d, request_id=%d, "
          "update_count=update_count+1, "
          "zero_sum=zero_sum + (%d), msg='%s', msg_length=%d, "
          "msg_checksum='%s'" %
          (table, idx, thread_id, request_id,
           zero_sum, msg, len(msg), msg_checksum, thread_id, request_id,
           zero_sum, msg, len(msg), msg_checksum))

# Each loader thread owns a part of the id space which it maintains inventory
# for. The loader thread generates inserts, updates and deletes for the table.
# The latest successful transaction and the latest open transaction are kept to
# verify after a disconnect that the rows were recovered properly.
class LoadGenWorker(WorkerThread):
  TXN_UNCOMMITTED = 0
  TXN_COMMIT_STARTED = 1
  TXN_COMMITTED = 2

  def __init__(self, thread_id):
    WorkerThread.__init__(self, 'loader-%02d' % thread_id)
    self.thread_id = thread_id
    self.rand = random.Random()
    self.rand.seed(thread_id)
    self.loop_num = 0

    # id_map contains the array of id's owned by this worker thread. It needs
    # to be offset by start_id for the actual id
    self.id_map = array.array('l')
    self.start_id = thread_id * OPTIONS.ids_per_loader
    self.num_id = OPTIONS.ids_per_loader
    self.start_share_id = OPTIONS.num_loaders * OPTIONS.ids_per_loader
    self.max_id = OPTIONS.max_id
    self.table = OPTIONS.table
    self.num_requests = OPTIONS.num_requests

    # stores information about the latest series of successful transactions
    #
    # each transaction is simply a map of id -> request_id
    # deleted rows are indicated by -request_id
    self.prev_txn = deque()
    self.cur_txn = None
    self.cur_txn_state = None

    self.start()

  def finish(self):
    if self.total_time:
      req_per_sec = self.loop_num / self.total_time
    else:
      req_per_sec = -1
    logging.info("total txns: %d, txn/s: %.2f rps" %
                 (self.loop_num, req_per_sec))

  # constructs the internal hash map of the ids owned by this thread and
  # the request id of each id
  def populate_id_map(self):
    logging.info("Populating id map")

    REQ_ID_COL = 0
    stmt = "SELECT request_id FROM %s WHERE id = %d"

    # the start_id is used for tracking active transactions, so the row needs
    # to exist
    idx = self.start_id
    execute(self.cur, stmt % (self.table, idx))
    if self.cur.rowcount > 0:
      request_id = self.cur.fetchone()[REQ_ID_COL]
    else:
      request_id = get_next_request_id()
      execute(self.cur, gen_insert(self.table, idx, self.thread_id,
                                   request_id, 0))
      self.con.commit()
      check_id(self.con.insert_id())

    self.id_map.append(request_id)

    self.cur_txn = {idx:request_id}
    self.cur_txn_state = self.TXN_COMMITTED
    for i in xrange(OPTIONS.committed_txns):
      self.prev_txn.append(self.cur_txn)

    # fetch the rest of the row for the id space owned by this thread
    for idx in xrange(self.start_id + 1, self.start_id + self.num_id):
      execute(self.cur, stmt % (self.table, idx))
      if self.cur.rowcount == 0:
        # Negative number is used to indicated a missing row
        self.id_map.append(-1)
      else:
        res = self.cur.fetchone()
        self.id_map.append(res[REQ_ID_COL])

    self.con.commit()

  def apply_cur_txn_changes(self):
    # apply the changes to the id_map
    for idx in self.cur_txn:
      if idx < self.start_id + self.num_id:
        assert idx >= self.start_id
        self.id_map[idx - self.start_id] = self.cur_txn[idx]
    self.cur_txn_state = self.TXN_COMMITTED

    self.prev_txn.append(self.cur_txn)
    self.prev_txn.popleft()

  def verify_txn(self, txn, committed):
    request_id = txn[self.start_id]
    if not committed:
      # if the transaction was not committed, then there should be no rows
      # in the table that have this request_id
      cond = '='
      # it is possible the start_id used to track this transaction is in
      # the process of being deleted
      if request_id < 0:
        request_id = -request_id
    else:
      # if the transaction was committed, then no rows modified by this
      # transaction should have a request_id less than this transaction's id
      cond = '<'
    stmt = ("SELECT COUNT(*) FROM %s WHERE id IN (%s) AND request_id %s %d" %
            (self.table, ','.join(str(x) for x in txn), cond, request_id))
    execute(self.cur, stmt)
    if (self.cur.rowcount != 1):
      raise TestError("Unable to retrieve results for query '%s'" % stmt)
    count = self.cur.fetchone()[0]
    if (count > 0):
      raise TestError("Expected '%s' to return 0 rows, but %d returned "
                      "instead" % (stmt, count))
    self.con.commit()

  def verify_data(self):
    # if the state of the current transaction is unknown (i.e. a commit was
    # issued, but the connection failed before, check the start_id row to
    # determine if it was committed
    request_id = self.cur_txn[self.start_id]
    if self.cur_txn_state == self.TXN_COMMIT_STARTED:
      assert request_id >= 0
      idx = self.start_id
      stmt = "SELECT id, request_id FROM %s where id = %d" % (self.table, idx)
      execute(self.cur, stmt)
      if (self.cur.rowcount == 0):
        raise TestError("Fetching start_id %d via '%s' returned no data! "
                        "This row should never be deleted!" % (idx, stmt))
      REQUEST_ID_COL = 1
      res = self.cur.fetchone()
      if res[REQUEST_ID_COL] == self.cur_txn[idx]:
        self.apply_cur_txn_changes()
      else:
        self.cur_txn_state = self.TXN_UNCOMMITTED
      self.con.commit()

    # if the transaction was not committed, verify there are no rows at this
    # request id
    #
    # however, if the transaction was committed, then verify none of the rows
    # have a request_id below the request_id recorded by the start_id row.
    if self.cur_txn_state == self.TXN_UNCOMMITTED:
      self.verify_txn(self.cur_txn, False)

    # verify all committed transactions
    for txn in self.prev_txn:
      self.verify_txn(txn, True)

    # verify the rows owned by this worker matches the request_id at which
    # they were set.
    idx = self.start_id
    max_map_id = self.start_id + self.num_id
    row_count = 0
    ID_COL = 0
    REQ_ID_COL = ID_COL + 1

    while idx < max_map_id:
      if (row_count == 0):
        num_rows_to_check = random.randint(50, 100)
        execute(self.cur,
          "SELECT id, request_id FROM %s where id >= %d and id < %d "
          "ORDER BY id LIMIT %d"
          % (self.table, idx, max_map_id, num_rows_to_check))

        # prevent future queries from being issued since we've hit the end of
        # the rows that exist in the table
        row_count = self.cur.rowcount if self.cur.rowcount != 0 else -1

      # determine the id of the next available row in the table
      if (row_count > 0):
        res = self.cur.fetchone()
        assert idx <= res[ID_COL]
        next_id = res[ID_COL]
        row_count -= 1
      else:
        next_id = max_map_id

      # rows up to the next id don't exist within the table, verify our
      # map has them as removed
      while idx < next_id:
        # see if the latest transaction may have modified this id. If so, use
        # that value.
        if self.id_map[idx - self.start_id] >= 0:
          raise ValidateError("Row id %d was not found in table, but "
                              "id_map has it at request_id %d" %
                              (idx, self.id_map[idx - self.start_id]))
        idx += 1

      if idx == max_map_id:
        break

      if (self.id_map[idx - self.start_id] != res[REQ_ID_COL]):
        raise ValidateError("Row id %d has req id %d, but %d is the "
                            "expected value!" %
                            (idx, res[REQ_ID_COL],
                             self.id_map[idx - self.start_id]))
      idx += 1

    self.con.commit()
    logging.debug("Verified data successfully")

  def execute_one(self):
    # select a number of rows; perform an insert; update or delete operation on
    # them
    num_rows = random.randint(1, MAX_ROWS_PER_REQ)
    ids = array.array('L')

    # allocate at least one row in the id space owned by this worker
    idx = random.randint(self.start_id, self.start_id + self.num_id - 1)
    ids.append(idx)

    for i in xrange(1, num_rows):
      # The valid ranges for ids is from start_id to start_id + num_id and from
      # start_share_id to max_id. The randint() uses the range from
      # start_share_id to max_id + num_id - 1. start_share_id to max_id covers
      # the shared range. The exclusive range is covered by max_id to max_id +
      # num_id - 1. If any number lands in this >= max_id section, it is
      # remapped to start_id and used for selecting a row in the exclusive
      # section.
      idx = random.randint(self.start_share_id, self.max_id + self.num_id - 1)
      if idx >= self.max_id:
        idx -= self.max_id - self.start_id
      if ids.count(idx) == 0:
        ids.append(idx)

    # perform a read of these rows
    ID_COL = 0
    ZERO_SUM_COL = ID_COL + 1

    # For repeatable-read isolation levels on MyRocks, during the lock
    # acquisition part of this transaction, it is possible the selected rows
    # conflict with another thread's transaction. This results in a deadlock
    # error that requires the whole transaction to be rolled back because the
    # transaction's current snapshot will always be reading an older version of
    # the row. MyRocks will prevent any updates to this row until the
    # snapshot is released and re-acquired.
    NUM_RETRIES = 100
    for i in xrange(NUM_RETRIES):
      ids_found = {}
      try:
        for idx in ids:
          stmt = ("SELECT id, zero_sum FROM %s WHERE id = %d "
                  "FOR UPDATE" % (self.table, idx))
          execute(self.cur, stmt)
          if self.cur.rowcount > 0:
            res = self.cur.fetchone()
            ids_found[res[ID_COL]] = res[ZERO_SUM_COL]
        break
      except MySQLdb.OperationalError, e:
        if not is_deadlock_error(e):
          raise e

      # if a deadlock occurred, rollback the transaction and wait a short time
      # before retrying.
      logging.debug("%s generated deadlock, retry %d of %d" %
                    (stmt, i, NUM_RETRIES))
      self.con.rollback()
      time.sleep(0.2)

    if i == NUM_RETRIES - 1:
      raise TestError("Unable to acquire locks after a number of retries "
                      "for query '%s'" % stmt)

    # ensure that the zero_sum column remains summed up to zero at the
    # end of this operation
    current_sum = 0

    # all row locks acquired at this point, so allocate a request_id
    request_id = get_next_request_id()
    self.cur_txn = {self.start_id:request_id}
    self.cur_txn_state = self.TXN_UNCOMMITTED

    for idx in ids:
      stmt = None
      zero_sum = self.rand.randint(-1000, 1000)
      action = self.rand.randint(0, 3)
      is_delete = False

      if idx in ids_found:
        # for each row found, determine if it should be updated or deleted
        if action == 0:
          stmt = gen_delete(self.table, idx)
          is_delete = True
          current_sum -= ids_found[idx]
        else:
          stmt = gen_update(self.table, idx, self.thread_id, request_id,
                            zero_sum)
          current_sum += zero_sum
      else:
        # if it does not exist, then determine if an insert should happen
        if action <= 1:
          stmt = gen_insert(self.table, idx, self.thread_id, request_id,
                            zero_sum)
          current_sum += zero_sum

      if stmt is not None:
        # mark in self.cur_txn what these new changes will be
        if is_delete:
          self.cur_txn[idx] = -request_id
        else:
          self.cur_txn[idx] = request_id
        execute(self.cur, stmt)
        if self.cur.rowcount == 0:
          raise TestError("Executing %s returned row count of 0!" % stmt)

    # the start_id row is used to determine if this transaction has been
    # committed if the connect fails and it is used to adjust the zero_sum
    # correctly
    idx = self.start_id
    ids.append(idx)
    self.cur_txn[idx] = request_id
    stmt = gen_insert_on_dup(self.table, idx, self.thread_id, request_id,
                             -current_sum)
    execute(self.cur, stmt)
    if self.cur.rowcount == 0:
      raise TestError("Executing '%s' returned row count of 0!" % stmt)

    # 90% commit, 10% rollback
    if roll_d100(90):
      self.con.rollback()
      logging.debug("request %s was rolled back" % request_id)
    else:
      self.cur_txn_state = self.TXN_COMMIT_STARTED
      self.con.commit()
      check_id(self.con.insert_id())
      if not self.con.get_server_info():
        raise MySQLdb.OperationalError(MySQLdb.constants.CR.CONNECTION_ERROR,
                                       "Possible connection error on commit")
      self.apply_cur_txn_changes()

    self.loop_num += 1
    if self.loop_num % 1000 == 0:
      logging.info("Processed %d transactions so far" % self.loop_num)

  def runme(self):
    global TEST_STOP, LOADERS_READY

    self.start_time = time.time()
    if self.reconnect():
      raise Exception("Unable to connect to MySQL server")

    self.populate_id_map()
    self.verify_data()

    logging.info("Starting load generator")
    reconnected = False
    LOADERS_READY += 1

    while self.loop_num < self.num_requests and not TEST_STOP:
      try:
        # verify our data on each reconnect and also on ocassion
        if reconnected or random.randint(1, 500) == 1:
          self.verify_data()
          reconnected = False

        self.execute_one()
        self.loop_num += 1
      except MySQLdb.OperationalError, e:
        if not is_connection_error(e):
          raise e
        if self.reconnect():
          raise Exception("Unable to connect to MySQL server")
        reconnected = True
    return

# the checker thread is running read only transactions to verify the row
# checksums match the message.
class CheckerWorker(WorkerThread):
  def __init__(self, thread_id):
    WorkerThread.__init__(self, 'checker-%02d' % thread_id)
    self.thread_id = thread_id
    self.rand = random.Random()
    self.rand.seed(thread_id)
    self.max_id = OPTIONS.max_id
    self.table = OPTIONS.table
    self.loop_num = 0
    self.start()

  def finish(self):
    logging.info("total loops: %d" % self.loop_num)

  def check_zerosum(self):
    # two methods for checking zero sum
    #   1. request the server to do it (90% of the time for now)
    #   2. read all rows and calculate directly
    if roll_d100(90):
      stmt = "SELECT SUM(zero_sum) FROM %s" % self.table
      if roll_d100(50):
        stmt += " FORCE INDEX(msg_i)"
      execute(self.cur, stmt)

      if self.cur.rowcount != 1:
        raise ValidateError("Error with query '%s'" % stmt)
      res = self.cur.fetchone()[0]
      if res != 0:
        raise ValidateError("Expected zero_sum to be 0, but %d returned "
                            "instead" % res)
    else:
      cur_isolation_level = self.get_isolation_level()
      self.set_isolation_level('REPEATABLE-READ')
      num_rows_to_check = random.randint(500, 1000)
      idx = 0
      sum = 0

      stmt = "SELECT id, zero_sum FROM %s where id >= %d ORDER BY id LIMIT %d"
      ID_COL = 0
      ZERO_SUM_COL = 1

      while idx < self.max_id:
        execute(self.cur, stmt % (self.table, idx, num_rows_to_check))
        if self.cur.rowcount == 0:
          break

        for i in xrange(self.cur.rowcount - 1):
          sum += self.cur.fetchone()[ZERO_SUM_COL]

        last_row = self.cur.fetchone()
        idx = last_row[ID_COL] + 1
        sum += last_row[ZERO_SUM_COL]

      if sum != 0:
        raise TestError("Zero sum column expected to total 0, but sum is %d "
                        "instead!" % sum)
      self.set_isolation_level(cur_isolation_level)

  def check_rows(self):
    class id_range():
      def __init__(self, min_id, min_inclusive, max_id, max_inclusive):
        self.min_id = min_id if min_inclusive else min_id + 1
        self.max_id = max_id if max_inclusive else max_id - 1
      def count(self, idx):
        return idx >= self.min_id and idx <= self.max_id

    stmt = ("SELECT id, msg, msg_length, msg_checksum FROM %s WHERE " %
            self.table)

    # two methods for checking rows
    #  1. pick a number of rows at random
    #  2. range scan
    if roll_d100(90):
      ids = []
      for i in xrange(random.randint(1, MAX_ROWS_PER_REQ)):
        ids.append(random.randint(0, self.max_id - 1))
      stmt += "id in (%s)" % ','.join(str(x) for x in ids)
    else:
      id1 = random.randint(0, self.max_id - 1)
      id2 = random.randint(0, self.max_id - 1)
      min_inclusive = random.randint(0, 1)
      cond1 = '>=' if min_inclusive else '>'
      max_inclusive = random.randint(0, 1)
      cond2 = '<=' if max_inclusive else '<'
      stmt += ("id %s %d AND id %s %d" %
               (cond1, min(id1, id2), cond2, max(id1, id2)))
      ids = id_range(min(id1, id2), min_inclusive, max(id1, id2), max_inclusive)

    execute(self.cur, stmt)

    ID_COL = 0
    MSG_COL = ID_COL + 1
    MSG_LENGTH_COL = MSG_COL + 1
    MSG_CHECKSUM_COL = MSG_LENGTH_COL + 1

    for row in self.cur.fetchall():
      idx = row[ID_COL]
      msg = row[MSG_COL]
      msg_length = row[MSG_LENGTH_COL]
      msg_checksum = row[MSG_CHECKSUM_COL]
      if ids.count(idx) < 1:
        raise ValidateError(
            "id %d returned from database, but query was '%s'" % (idx, stmt))
      if (len(msg) != msg_length):
        raise ValidateError(
            "id %d contains msg_length %d, but msg '%s' is only %d "
            "characters long" % (idx, msg_length, msg, len(msg)))
      if (sha1(msg) != msg_checksum):
        raise ValidateError("id %d has checksum '%s', but expected checksum "
                            "is '%s'" % (idx, msg_checksum, sha1(msg)))

  def runme(self):
    global TEST_STOP

    self.start_time = time.time()
    if self.reconnect():
      raise Exception("Unable to connect to MySQL server")
    logging.info("Starting checker")

    while not TEST_STOP:
      try:
        # choose one of three options:
        #   1. compute zero_sum across all rows is 0
        #   2. read a number of rows and verify checksums
        if roll_d100(25):
          self.check_zerosum()
        else:
          self.check_rows()

        self.con.commit()
        self.loop_num += 1
        if self.loop_num % 10000 == 0:
          logging.info("Processed %d transactions so far" % self.loop_num)
      except MySQLdb.OperationalError, e:
        if not is_connection_error(e):
          raise e
        if self.reconnect():
          raise Exception("Unable to reconnect to MySQL server")

if  __name__ == '__main__':
  parser = argparse.ArgumentParser(description='Concurrent load generator.')

  parser.add_argument('-C, --committed-txns', dest='committed_txns',
                      default=3, type=int,
                      help="number of committed txns to verify")

  parser.add_argument('-c, --num-checkers', dest='num_checkers', type=int,
                      default=4,
                      help="number of reader/checker threads to test with")

  parser.add_argument('-d, --db', dest='db', default='test',
                      help="mysqld server database to test with")

  parser.add_argument('-H, --host', dest='host', default='127.0.0.1',
                      help="mysqld server host ip address")

  parser.add_argument('-i, --ids-per-loader', dest='ids_per_loader',
                      type=int, default=100,
                      help="number of records which each loader owns "
                           "exclusively, up to max-id / 2 / num-loaders")

  parser.add_argument('-L, --log-file', dest='log_file', default=None,
                      help="log file for output")

  parser.add_argument('-l, --num-loaders', dest='num_loaders', type=int,
                      default=16,
                      help="number of loader threads to test with")

  parser.add_argument('-m, --max-id', dest='max_id', type=int, default=1000,
                      help="maximum number of records which the table "
                           "extends to, must be larger than ids_per_loader * "
                           "num_loaders")

  parser.add_argument('-n, --num-records', dest='num_records', type=int,
                      default=0,
                      help="number of records to populate the table with")

  parser.add_argument('-P, --port', dest='port', default=3307, type=int,
                      help='mysqld server host port')

  parser.add_argument('-r, --num-requests', dest='num_requests', type=int,
                      default=100000000,
                      help="number of requests issued per worker thread")

  parser.add_argument('-T, --truncate', dest='truncate', action='store_true',
                      help="truncates or creates the table before the test")

  parser.add_argument('-t, --table', dest='table', default='t1',
                      help="mysqld server table to test with")

  parser.add_argument('-u, --user', dest='user', default='root',
                      help="user to log into the mysql server")

  parser.add_argument('-v, --verbose', dest='verbose', action='store_true',
                      help="enable debug logging")

  parser.add_argument('-E, --expect-file', dest='expect_file', default=None,
                      help="expect file for server restart")

  parser.add_argument('-D, --reap-delay', dest='reap_delay', type=int,
                      default=0,
                      help="seconds to sleep after each server reap")

  OPTIONS = parser.parse_args()

  if OPTIONS.verbose:
    log_level = logging.DEBUG
  else:
    log_level = logging.INFO

  logging.basicConfig(level=log_level,
                      format='%(asctime)s %(threadName)s [%(levelname)s] '
                             '%(message)s',
                      datefmt='%Y-%m-%d %H:%M:%S',
                      filename=OPTIONS.log_file)

  logging.info("Command line given: %s" % ' '.join(sys.argv))

  if (OPTIONS.max_id < 0 or OPTIONS.ids_per_loader <= 0 or
      OPTIONS.max_id < OPTIONS.ids_per_loader * OPTIONS.num_loaders):
    logging.error("ids-per-loader must be larger tha 0 and max-id must be "
                  "larger than ids_per_loader * num_loaders")
    exit(1)

  logging.info("Using table %s.%s for test" % (OPTIONS.db, OPTIONS.table))

  if OPTIONS.truncate:
    logging.info("Truncating table")
    con = MySQLdb.connect(user=OPTIONS.user, host=OPTIONS.host,
                          port=OPTIONS.port, db=OPTIONS.db)
    if not con:
      raise TestError("Unable to connect to mysqld server to create/truncate "
                      "table")
    cur = con.cursor()
    cur.execute("SELECT COUNT(*) FROM INFORMATION_SCHEMA.tables WHERE "
                         "table_schema = '%s' AND table_name = '%s'" %
                         (OPTIONS.db, OPTIONS.table))
    if cur.rowcount != 1:
      logging.error("Unable to retrieve information about table %s "
                    "from information_schema!" % OPTIONS.table)
      exit(1)

    if cur.fetchone()[0] == 0:
      logging.info("Table %s not found, creating a new one" % OPTIONS.table)
      cur.execute("CREATE TABLE %s (id INT PRIMARY KEY, "
                  "thread_id INT NOT NULL, "
                  "request_id BIGINT UNSIGNED NOT NULL, "
                  "update_count INT UNSIGNED NOT NULL DEFAULT 0, "
                  "zero_sum INT DEFAULT 0, "
                  "msg VARCHAR(1024), "
                  "msg_length int, "
                  "msg_checksum varchar(128), "
                  "KEY msg_i(msg(255), zero_sum)) "
                  "ENGINE=RocksDB DEFAULT CHARSET=latin1 COLLATE=latin1_bin" %
                  OPTIONS.table)
    else:
      logging.info("Table %s found, truncating" % OPTIONS.table)
      cur.execute("TRUNCATE TABLE %s" % OPTIONS.table)
    con.commit()

  if populate_table(OPTIONS.num_records):
    logging.error("Populate table returned an error")
    exit(1)

  logging.info("Starting %d loaders" % OPTIONS.num_loaders)
  loaders = []
  for i in xrange(OPTIONS.num_loaders):
    loaders.append(LoadGenWorker(i))

  logging.info("Starting %d checkers" % OPTIONS.num_checkers)
  checkers = []
  for i in xrange(OPTIONS.num_checkers):
    checkers.append(CheckerWorker(i))

  while LOADERS_READY < OPTIONS.num_loaders:
    time.sleep(0.5)

  if OPTIONS.expect_file and OPTIONS.reap_delay > 0:
    logging.info('Starting reaper')
    checkers.append(ReaperWorker())

  workers_failed = 0
  workers_failed += wait_for_workers(loaders, len(checkers))

  if TEST_STOP:
    logging.error("Detected test failure, aborting")
    os._exit(1)

  TEST_STOP = True

  workers_failed += wait_for_workers(checkers)

  if workers_failed > 0:
    logging.error("Test detected %d failures, aborting" % workers_failed)
    sys.exit(1)

  logging.info("Test completed successfully")
  sys.exit(0)
