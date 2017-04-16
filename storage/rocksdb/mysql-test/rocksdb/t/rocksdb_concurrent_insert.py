"""
This script tests concurrent inserts on a given table.
Example Usage (in Mysql Test Framework):

  CREATE TABLE t1 (a INT) ENGINE=rocksdb;

  let $exec = python suite/rocksdb/t/rocksdb_concurrent_insert.py \
                     root 127.0.0.1 $MASTER_MYPORT test t1 100 4;
  exec $exec;

"""
import cStringIO
import hashlib
import MySQLdb
import os
import random
import signal
import sys
import threading
import time
import string

def get_insert(table_name, idx):
  return """INSERT INTO %s (a) VALUES (%d)""" % (table_name, idx)

class Inserter(threading.Thread):
  Instance = None
  def __init__(self, con, table_name, num_inserts):
    threading.Thread.__init__(self)
    self.finished = False
    self.num_inserts = num_inserts
    con.autocommit(False)
    self.con = con
    self.rand = random.Random()
    self.exception = None
    self.table_name = table_name
    Inserter.Instance = self
    self.start()
  def run(self):
    try:
      self.runme()
    except Exception, e:
      self.exception = traceback.format_exc()
      print "caught (%s)" % e
    finally:
      self.finish()
  def runme(self):
    cur = self.con.cursor()
    for i in xrange(self.num_inserts):
      try:
        cur.execute(get_insert(self.table_name, i))
        r = self.rand.randint(1,10)
        if r < 4:
          self.con.commit()
      except:
        cur = self.con.cursor()
    try:
      self.con.commit()
    except Exception, e:
      self.exception = traceback.format_exc()
      print "caught (%s)" % e
      pass
  def finish(self):
    self.finished = True

if __name__ == '__main__':
  if len(sys.argv) != 8:
    print "Usage: rocksdb_concurrent_insert.py user host port db_name " \
          "table_name num_inserts num_threads"
    sys.exit(1)

  user = sys.argv[1]
  host = sys.argv[2]
  port = int(sys.argv[3])
  db = sys.argv[4]
  table_name = sys.argv[5]
  num_inserts = int(sys.argv[6])
  num_workers = int(sys.argv[7])

  worker_failed = False
  workers = []
  for i in xrange(num_workers):
    inserter = Inserter(
      MySQLdb.connect(user=user, host=host, port=port, db=db), table_name,
      num_inserts)
    workers.append(inserter)

  for w in workers:
    w.join()
    if w.exception:
      print "Worker hit an exception:\n%s\n" % w.exception
      worker_failed = True

  if worker_failed:
    sys.exit(1)
