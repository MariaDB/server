"""
This script stress tests deadlock detection.

Usage: rocksdb_deadlock_stress.py user host port db_name table_name
       num_iters num_threads
"""
import cStringIO
import hashlib
import MySQLdb
from MySQLdb.constants import ER
import os
import random
import signal
import sys
import threading
import time
import string
import traceback

def is_deadlock_error(exc):
    error_code = exc.args[0]
    return (error_code == MySQLdb.constants.ER.LOCK_DEADLOCK)

def get_query(table_name, idx):
  # Let's assume that even indexes will always be acquireable, to make
  # deadlock detection more interesting.
  if idx % 2 == 0:
    return """SELECT * from %s WHERE a = %d LOCK IN SHARE MODE""" % (table_name, idx)
  else:
    r = random.randint(1, 3);
    if r == 1:
      return """SELECT * from %s WHERE a = %d FOR UPDATE""" % (table_name, idx)
    elif r == 2:
      return """INSERT INTO %s VALUES (%d, 1)
                ON DUPLICATE KEY UPDATE b=b+1""" % (table_name, idx)
    else:
      return """DELETE from %s WHERE a = %d""" % (table_name, idx)

class Worker(threading.Thread):
  def __init__(self, con, table_name, num_iters):
    threading.Thread.__init__(self)
    self.con = con
    self.table_name = table_name
    self.num_iters = num_iters
    self.exception = None
    self.start()
  def run(self):
    try:
      self.runme()
    except Exception, e:
      self.exception = traceback.format_exc()
  def runme(self):
    cur = self.con.cursor()
    for x in xrange(self.num_iters):
      try:
        for i in random.sample(xrange(100), 10):
          cur.execute(get_query(self.table_name, i))
        self.con.commit()
      except MySQLdb.OperationalError, e:
        self.con.rollback()
        cur = self.con.cursor()
        if not is_deadlock_error(e):
          raise e

if __name__ == '__main__':
  if len(sys.argv) != 8:
    print "Usage: rocksdb_deadlock_stress.py user host port db_name " \
          "table_name num_iters num_threads"
    sys.exit(1)

  user = sys.argv[1]
  host = sys.argv[2]
  port = int(sys.argv[3])
  db = sys.argv[4]
  table_name = sys.argv[5]
  num_iters = int(sys.argv[6])
  num_workers = int(sys.argv[7])

  worker_failed = False
  workers = []
  for i in xrange(num_workers):
    w = Worker(
      MySQLdb.connect(user=user, host=host, port=port, db=db), table_name,
      num_iters)
    workers.append(w)

  for w in workers:
    w.join()
    if w.exception:
      print "Worker hit an exception:\n%s\n" % w.exception
      worker_failed = True

  if worker_failed:
    sys.exit(1)
