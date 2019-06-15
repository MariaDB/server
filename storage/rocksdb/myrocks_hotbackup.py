#!@PYTHON_SHEBANG@

from __future__ import division
from optparse import OptionParser
import collections
import signal
import os
import stat
import sys
import re
import commands
import subprocess
import logging
import logging.handlers
import time
import datetime
import shutil
import traceback
import tempfile

import MySQLdb
import MySQLdb.connections
from MySQLdb import OperationalError, ProgrammingError

logger = None
opts = None
rocksdb_files = ['MANIFEST', 'CURRENT', 'OPTIONS']
rocksdb_data_suffix = '.sst'
rocksdb_wal_suffix = '.log'
exclude_files = ['master.info', 'relay-log.info', 'worker-relay-log.info',
                 'auto.cnf', 'gaplock.log', 'ibdata', 'ib_logfile', '.trash']
wdt_bin = 'wdt'

def is_manifest(fname):
  for m in rocksdb_files:
    if fname.startswith(m):
      return True
  return False

class Writer(object):
  a = None
  def __init__(self):
    a = None

class StreamWriter(Writer):
  stream_cmd= ''

  def __init__(self, stream_option, direct = 0):
    super(StreamWriter, self).__init__()
    if stream_option == 'tar':
      self.stream_cmd= 'tar chf -'
    elif stream_option == 'xbstream':
      self.stream_cmd= 'xbstream -c'
      if direct:
        self.stream_cmd = self.stream_cmd + ' -d'
    else:
      raise Exception("Only tar or xbstream is supported as streaming option.")

  def write(self, file_name):
    rc= os.system(self.stream_cmd + " " + file_name)
    if (rc != 0):
      raise Exception("Got error on stream write: " + str(rc) + " " + file_name)


class MiscFilesProcessor():
  datadir = None
  wildcard = r'.*\.[frm|MYD|MYI|MAD|MAI|MRG|TRG|TRN|ARM|ARZ|CSM|CSV|opt|par]'
  regex = None
  start_backup_time = None
  skip_check_frm_timestamp = None

  def __init__(self, datadir, skip_check_frm_timestamp, start_backup_time):
    self.datadir = datadir
    self.regex = re.compile(self.wildcard)
    self.skip_check_frm_timestamp = skip_check_frm_timestamp
    self.start_backup_time = start_backup_time

  def process_db(self, db):
    # do nothing
    pass

  def process_file(self, path):
    # do nothing
    pass

  def check_frm_timestamp(self, fname, path):
    if not self.skip_check_frm_timestamp and fname.endswith('.frm'):
      if os.path.getmtime(path) > self.start_backup_time:
        logger.error('FRM file %s was updated after starting backups. '
                     'Schema could have changed and the resulting copy may '
                     'not be valid. Aborting. '
                     '(backup time: %s, file modifled time: %s)',
                     path, datetime.datetime.fromtimestamp(self.start_backup_time).strftime('%Y-%m-%d %H:%M:%S'),
                     datetime.datetime.fromtimestamp(os.path.getmtime(path)).strftime('%Y-%m-%d %H:%M:%S'))
        raise Exception("Inconsistent frm file timestamp");

  def process(self):
    os.chdir(self.datadir)
    for db in self.get_databases():
      logger.info("Starting MySQL misc file traversal from database %s..", db)
      self.process_db(db)
      for f in self.get_files(db):
        if self.match(f):
          rel_path = os.path.join(db, f)
          self.check_frm_timestamp(f, rel_path)
          self.process_file(rel_path)
    logger.info("Traversing misc files from data directory..")
    for f in self.get_files(""):
      should_skip = False
      for e in exclude_files:
        if f.startswith(e) or f.endswith(e):
          logger.info("Skipping %s", f)
          should_skip = True
          break
      if not should_skip:
        self.process_file(f)

  def match(self, filename):
    if self.regex.match(filename):
      return True
    else:
      return False

  def get_databases(self):
    dbs = []
    dirs = [ d for d in os.listdir(self.datadir) \
            if not os.path.isfile(os.path.join(self.datadir,d))]
    for db in dirs:
      if not db.startswith('.') and not self._is_socket(db) and not db == "#rocksdb":
        dbs.append(db)
    return dbs

  def get_files(self, db):
    dbdir = self.datadir + "/" + db
    return [ f for f in os.listdir(dbdir) \
            if os.path.isfile(os.path.join(dbdir,f))]

  def _is_socket(self, item):
      mode = os.stat(os.path.join(self.datadir, item)).st_mode
      if stat.S_ISSOCK(mode):
        return True
      return False


class MySQLBackup(MiscFilesProcessor):
  writer = None

  def __init__(self, datadir, writer, skip_check_frm_timestamp, start_backup_time):
    MiscFilesProcessor.__init__(self, datadir, skip_check_frm_timestamp, start_backup_time)
    self.writer = writer

  def process_file(self, fname):    # overriding base class
    self.writer.write(fname)


class MiscFilesLinkCreator(MiscFilesProcessor):
  snapshot_dir = None

  def __init__(self, datadir, snapshot_dir, skip_check_frm_timestamp, start_backup_time):
    MiscFilesProcessor.__init__(self, datadir, skip_check_frm_timestamp, start_backup_time)
    self.snapshot_dir = snapshot_dir

  def process_db(self, db):
    snapshot_sub_dir = os.path.join(self.snapshot_dir, db)
    os.makedirs(snapshot_sub_dir)

  def process_file(self, path):
    dst_path = os.path.join(self.snapshot_dir, path)
    os.link(path, dst_path)


# RocksDB backup
class RocksDBBackup():
  source_dir = None
  writer = None
  # sst files sent in this backup round
  sent_sst = {}
  # target sst files in this backup round
  target_sst = {}
  # sst files sent in all backup rounds
  total_sent_sst= {}
  # sum of sst file size sent in this backup round
  sent_sst_size = 0
  # sum of target sst file size in this backup round
  # if sent_sst_size becomes equal to target_sst_size,
  # it means the backup round finished backing up all sst files
  target_sst_size = 0
  # sum of all sst file size sent all backup rounds
  total_sent_sst_size= 0
  # sum of all target sst file size from all backup rounds
  total_target_sst_size = 0
  show_progress_size_interval= 1073741824 # 1GB
  wal_files= []
  manifest_files= []
  finished= False

  def __init__(self, source_dir, writer, prev):
    self.source_dir = source_dir
    self.writer = writer
    os.chdir(self.source_dir)
    self.init_target_files(prev)

  def init_target_files(self, prev):
    sst = {}
    self.sent_sst = {}
    self.target_sst= {}
    self.total_sent_sst = {}
    self.sent_sst_size = 0
    self.target_sst_size = 0
    self.total_sent_sst_size= 0
    self.total_target_sst_size= 0
    self.wal_files= []
    self.manifest_files= []

    for f in os.listdir(self.source_dir):
      if f.endswith(rocksdb_data_suffix):
        # exactly the same file (same size) was sent in previous backup rounds
        if prev is not None and f in prev.total_sent_sst and int(os.stat(f).st_size) == prev.total_sent_sst[f]:
          continue
        sst[f]= int(os.stat(f).st_size)
        self.target_sst_size = self.target_sst_size + os.stat(f).st_size
      elif is_manifest(f):
        self.manifest_files.append(f)
      elif f.endswith(rocksdb_wal_suffix):
        self.wal_files.append(f)
    self.target_sst= collections.OrderedDict(sorted(sst.items()))

    if prev is not None:
      self.total_sent_sst = prev.total_sent_sst
      self.total_sent_sst_size = prev.total_sent_sst_size
      self.total_target_sst_size = self.target_sst_size + prev.total_sent_sst_size
    else:
      self.total_target_sst_size = self.target_sst_size

  def do_backup_single(self, fname):
    self.writer.write(fname)
    os.remove(fname)

  def do_backup_sst(self, fname, size):
    self.do_backup_single(fname)
    self.sent_sst[fname]= size
    self.total_sent_sst[fname]= size
    self.sent_sst_size = self.sent_sst_size + size
    self.total_sent_sst_size = self.total_sent_sst_size + size

  def do_backup_manifest(self):
    for f in self.manifest_files:
      self.do_backup_single(f)

  def do_backup_wal(self):
    for f in self.wal_files:
      self.do_backup_single(f)

  # this is the last snapshot round. backing up all the rest files
  def do_backup_final(self):
    logger.info("Backup WAL..")
    self.do_backup_wal()
    logger.info("Backup Manifest..")
    self.do_backup_manifest()
    self.do_cleanup()
    self.finished= True

  def do_cleanup(self):
    shutil.rmtree(self.source_dir)
    logger.info("Cleaned up checkpoint from %s", self.source_dir)

  def do_backup_until(self, time_limit):
    logger.info("Starting backup from snapshot: target files %d", len(self.target_sst))
    start_time= time.time()
    last_progress_time= start_time
    progress_size= 0
    for fname, size in self.target_sst.iteritems():
      self.do_backup_sst(fname, size)
      progress_size= progress_size + size
      elapsed_seconds = time.time() - start_time
      progress_seconds = time.time() - last_progress_time

      if self.should_show_progress(size):
        self.show_progress(progress_size, progress_seconds)
        progress_size=0
        last_progress_time= time.time()

      if elapsed_seconds > time_limit and self.has_sent_all_sst() is False:
        logger.info("Snapshot round finished. Elapsed Time: %5.2f.  Remaining sst files: %d",
                    elapsed_seconds, len(self.target_sst) - len(self.sent_sst))
        self.do_cleanup()
        break;
    if self.has_sent_all_sst():
      self.do_backup_final()

    return self

  def should_show_progress(self, size):
    if int(self.total_sent_sst_size/self.show_progress_size_interval) > int((self.total_sent_sst_size-size)/self.show_progress_size_interval):
      return True
    else:
      return False

  def show_progress(self, size, seconds):
    logger.info("Backup Progress: %5.2f%%   Sent %6.2f GB of %6.2f GB data, Transfer Speed: %6.2f MB/s",
                self.total_sent_sst_size*100/self.total_target_sst_size,
                self.total_sent_sst_size/1024/1024/1024,
                self.total_target_sst_size/1024/1024/1024,
                size/seconds/1024/1024)

  def print_backup_report(self):
    logger.info("Sent %6.2f GB of sst files, %d files in total.",
                self.total_sent_sst_size/1024/1024/1024,
                len(self.total_sent_sst))

  def has_sent_all_sst(self):
    if self.sent_sst_size == self.target_sst_size:
      return True
    return False


class MySQLUtil:
  @staticmethod
  def connect(user, password, port, socket=None):
    if socket:
      dbh = MySQLdb.Connect(user=user,
                            passwd=password,
                            unix_socket=socket)
    else:
      dbh = MySQLdb.Connect(user=user,
                            passwd=password,
                            port=port,
                            host="127.0.0.1")
    return dbh

  @staticmethod
  def create_checkpoint(dbh, checkpoint_dir):
    sql = ("SET GLOBAL rocksdb_create_checkpoint='{0}'"
           .format(checkpoint_dir))
    cur= dbh.cursor()
    cur.execute(sql)
    cur.close()

  @staticmethod
  def get_datadir(dbh):
    sql = "SELECT @@datadir"
    cur = dbh.cursor()
    cur.execute(sql)
    row = cur.fetchone()
    return row[0]

  @staticmethod
  def is_directio_enabled(dbh):
    sql = "SELECT @@global.rocksdb_use_direct_reads"
    cur = dbh.cursor()
    cur.execute(sql)
    row = cur.fetchone()
    return row[0]

class BackupRunner:
  datadir = None
  start_backup_time = None

  def __init__(self, datadir):
    self.datadir = datadir
    self.start_backup_time = time.time()

  def start_backup_round(self, backup_round, prev_backup):
    def signal_handler(*args):
      logger.info("Got signal. Exit")
      if b is not None:
        logger.info("Cleaning up snapshot directory..")
        b.do_cleanup()
      sys.exit(1)

    b = None
    try:
      signal.signal(signal.SIGINT, signal_handler)
      w = None
      if not opts.output_stream:
        raise Exception("Currently only streaming backup is supported.")

      snapshot_dir = opts.checkpoint_directory + '/' + str(backup_round)
      dbh = MySQLUtil.connect(opts.mysql_user,
                              opts.mysql_password,
                              opts.mysql_port,
                              opts.mysql_socket)
      direct = MySQLUtil.is_directio_enabled(dbh)
      logger.info("Direct I/O: %d", direct)

      w = StreamWriter(opts.output_stream, direct)

      if not self.datadir:
        self.datadir = MySQLUtil.get_datadir(dbh)
        logger.info("Set datadir: %s", self.datadir)
      logger.info("Creating checkpoint at %s", snapshot_dir)
      MySQLUtil.create_checkpoint(dbh, snapshot_dir)
      logger.info("Created checkpoint at %s", snapshot_dir)
      b = RocksDBBackup(snapshot_dir, w, prev_backup)
      return b.do_backup_until(opts.checkpoint_interval)
    except Exception as e:
      logger.error(e)
      logger.error(traceback.format_exc())
      if b is not None:
        logger.info("Cleaning up snapshot directory.")
        b.do_cleanup()
      sys.exit(1)

  def backup_mysql(self):
    try:
      w = None
      if opts.output_stream:
        w = StreamWriter(opts.output_stream)
      else:
        raise Exception("Currently only streaming backup is supported.")
      b = MySQLBackup(self.datadir, w, opts.skip_check_frm_timestamp,
                      self.start_backup_time)
      logger.info("Taking MySQL misc backups..")
      b.process()
      logger.info("MySQL misc backups done.")
    except Exception as e:
      logger.error(e)
      logger.error(traceback.format_exc())
      sys.exit(1)


class WDTBackup:
  datadir = None
  start_backup_time = None

  def __init__(self, datadir):
    self.datadir = datadir
    self.start_backup_time = time.time()

  def cleanup(self, snapshot_dir, server_log):
    if server_log:
      server_log.seek(0)
      logger.info("WDT server log:")
      logger.info(server_log.read())
      server_log.close()
    if snapshot_dir:
      logger.info("Cleaning up snapshot dir %s", snapshot_dir)
      shutil.rmtree(snapshot_dir)

  def backup_with_timeout(self, backup_round):
    def signal_handler(*args):
      logger.info("Got signal. Exit")
      self.cleanup(snapshot_dir, server_log)
      sys.exit(1)

    logger.info("Starting backup round %d", backup_round)
    snapshot_dir = None
    server_log = None
    try:
      signal.signal(signal.SIGINT, signal_handler)
      # create rocksdb snapshot
      snapshot_dir = os.path.join(opts.checkpoint_directory, str(backup_round))
      dbh = MySQLUtil.connect(opts.mysql_user,
                              opts.mysql_password,
                              opts.mysql_port,
                              opts.mysql_socket)
      logger.info("Creating checkpoint at %s", snapshot_dir)
      MySQLUtil.create_checkpoint(dbh, snapshot_dir)
      logger.info("Created checkpoint at %s", snapshot_dir)

      # get datadir if not provided
      if not self.datadir:
        self.datadir = MySQLUtil.get_datadir(dbh)
        logger.info("Set datadir: %s", self.datadir)

      # create links for misc files
      link_creator = MiscFilesLinkCreator(self.datadir, snapshot_dir,
                                          opts.skip_check_frm_timestamp,
                                          self.start_backup_time)
      link_creator.process()

      current_path = os.path.join(opts.backupdir, "CURRENT")

      # construct receiver cmd, using the data directory as recovery-id.
      # we delete the current file because it is not append-only, therefore not
      # resumable.
      remote_cmd = (
                "ssh {0} rm -f {1}; "
                "{2} -directory {3} -enable_download_resumption "
                "-recovery_id {4} -start_port 0 -abort_after_seconds {5} {6}"
          ).format(opts.destination,
                   current_path,
                   wdt_bin,
                   opts.backupdir,
                   self.datadir,
                   opts.checkpoint_interval,
                   opts.extra_wdt_receiver_options)
      logger.info("WDT remote cmd %s", remote_cmd)
      server_log = tempfile.TemporaryFile()
      remote_process = subprocess.Popen(remote_cmd.split(),
                                        stdout=subprocess.PIPE,
                                        stderr=server_log)
      wdt_url = remote_process.stdout.readline().strip()
      if not wdt_url:
        raise Exception("Unable to get connection url from wdt receiver")
      sender_cmd = (
                "{0} -connection_url \'{1}\' -directory {2} -app_name=myrocks "
                "-avg_mbytes_per_sec {3} "
                "-enable_download_resumption -abort_after_seconds {4} {5}"
          ).format(wdt_bin,
                   wdt_url,
                   snapshot_dir,
                   opts.avg_mbytes_per_sec,
                   opts.checkpoint_interval,
                   opts.extra_wdt_sender_options)
      sender_status = os.system(sender_cmd) >> 8
      remote_status = remote_process.wait()
      self.cleanup(snapshot_dir, server_log)
      # TODO: handle retryable and non-retyable errors differently
      return (sender_status == 0 and remote_status == 0)

    except Exception as e:
      logger.error(e)
      logger.error(traceback.format_exc())
      self.cleanup(snapshot_dir, server_log)
      sys.exit(1)


def backup_using_wdt():
  if not opts.destination:
    logger.error("Must provide remote destination when using WDT")
    sys.exit(1)

  # TODO: detect whether WDT is installed
  logger.info("Backing up myrocks to %s using WDT", opts.destination)
  wdt_backup = WDTBackup(opts.datadir)
  finished = False
  backup_round = 1
  while not finished:
    start_time = time.time()
    finished = wdt_backup.backup_with_timeout(backup_round)
    end_time = time.time()
    duration_seconds = end_time - start_time
    if (not finished) and (duration_seconds < opts.checkpoint_interval):
      # round finished before timeout
      sleep_duration = (opts.checkpoint_interval - duration_seconds)
      logger.info("Sleeping for %f seconds", sleep_duration)
      time.sleep(sleep_duration)

    backup_round = backup_round + 1
  logger.info("Finished myrocks backup using WDT")


def init_logger():
  global logger
  logger = logging.getLogger('myrocks_hotbackup')
  logger.setLevel(logging.INFO)
  h1= logging.StreamHandler(sys.stderr)
  f = logging.Formatter("%(asctime)s.%(msecs)03d %(levelname)s %(message)s",
                        "%Y-%m-%d %H:%M:%S")
  h1.setFormatter(f)
  logger.addHandler(h1)

backup_wdt_usage = ("Backup using WDT: myrocks_hotbackup "
                "--user=root --password=pw --stream=wdt "
                "--checkpoint_dir=<directory where temporary backup hard links "
                "are created> --destination=<remote host name> --backup_dir="
                "<remote directory name>. This has to be executed at the src "
                "host.")
backup_usage= "Backup: set -o pipefail; myrocks_hotbackup --user=root --password=pw --port=3306 --checkpoint_dir=<directory where temporary backup hard links are created> | ssh -o NoneEnabled=yes remote_server 'tar -xi -C <directory on remote server where backups will be sent>' . You need to execute backup command on a server where you take backups."
move_back_usage= "Move-Back: myrocks_hotbackup --move_back --datadir=<dest mysql datadir> --rocksdb_datadir=<dest rocksdb datadir> --rocksdb_waldir=<dest rocksdb wal dir> --backup_dir=<where backup files are stored> . You need to execute move-back command on a server where backup files are sent."


def parse_options():
  global opts
  parser = OptionParser(usage = "\n\n" + backup_usage + "\n\n" + \
          backup_wdt_usage + "\n\n" + move_back_usage)
  parser.add_option('-i', '--interval', type='int', dest='checkpoint_interval',
                    default=300,
                    help='Number of seconds to renew checkpoint')
  parser.add_option('-c', '--checkpoint_dir', type='string', dest='checkpoint_directory',
                    default='/data/mysql/backup/snapshot',
                    help='Local directory name where checkpoints will be created.')
  parser.add_option('-d', '--datadir', type='string', dest='datadir',
                    default=None,
                    help='backup mode: src MySQL datadir. move_back mode: dest MySQL datadir')
  parser.add_option('-s', '--stream', type='string', dest='output_stream',
                    default='tar',
                    help='Setting streaming backup options. Currently tar, WDT '
                    'and xbstream are supported. Default is tar')
  parser.add_option('--destination', type='string', dest='destination',
                    default='',
                    help='Remote server name. Only used for WDT mode so far.')
  parser.add_option('--avg_mbytes_per_sec', type='int',
                    dest='avg_mbytes_per_sec',
                    default=500,
                    help='Average backup rate in MBytes/sec. WDT only.')
  parser.add_option('--extra_wdt_sender_options', type='string',
                    dest='extra_wdt_sender_options',
                    default='',
                    help='Extra options for WDT sender')
  parser.add_option('--extra_wdt_receiver_options', type='string',
                    dest='extra_wdt_receiver_options',
                    default='',
                    help='Extra options for WDT receiver')
  parser.add_option('-u', '--user', type='string', dest='mysql_user',
                    default='root',
                    help='MySQL user name')
  parser.add_option('-p', '--password', type='string', dest='mysql_password',
                    default='',
                    help='MySQL password name')
  parser.add_option('-P', '--port', type='int', dest='mysql_port',
                    default=3306,
                    help='MySQL port number')
  parser.add_option('-S', '--socket', type='string', dest='mysql_socket',
                    default=None,
                    help='MySQL socket path. Takes precedence over --port.')
  parser.add_option('-m', '--move_back', action='store_true', dest='move_back',
                    default=False,
                    help='Moving MyRocks backup files to proper locations.')
  parser.add_option('-r', '--rocksdb_datadir', type='string', dest='rocksdb_datadir',
                    default=None,
                    help='RocksDB target data directory where backup data files will be moved. Must be empty.')
  parser.add_option('-w', '--rocksdb_waldir', type='string', dest='rocksdb_waldir',
                    default=None,
                    help='RocksDB target data directory where backup wal files will be moved. Must be empty.')
  parser.add_option('-b', '--backup_dir', type='string', dest='backupdir',
                    default=None,
                    help='backup mode for WDT: Remote directory to store '
                    'backup. move_back mode: Locations where backup '
                    'files are stored.')
  parser.add_option('-f', '--skip_check_frm_timestamp',
                    dest='skip_check_frm_timestamp',
                    action='store_true', default=False,
                    help='skipping to check if frm files are updated after starting backup.')
  parser.add_option('-D', '--debug_signal_file', type='string', dest='debug_signal_file',
                    default=None,
                    help='debugging purpose: waiting until the specified file is created')

  opts, args = parser.parse_args()


def create_moveback_dir(directory):
  if not os.path.exists(directory):
    os.makedirs(directory)
  else:
    for f in os.listdir(directory):
      logger.error("Directory %s has file or directory %s!", directory, f)
      raise

def print_move_back_usage():
  logger.warning(move_back_usage)

def move_back():
  if opts.rocksdb_datadir is None or opts.rocksdb_waldir is None or opts.backupdir is None or opts.datadir is None:
    print_move_back_usage()
    sys.exit()
  create_moveback_dir(opts.datadir)
  create_moveback_dir(opts.rocksdb_datadir)
  create_moveback_dir(opts.rocksdb_waldir)

  os.chdir(opts.backupdir)
  for f in os.listdir(opts.backupdir):
    if os.path.isfile(os.path.join(opts.backupdir,f)):
      if f.endswith(rocksdb_wal_suffix):
        shutil.move(f, opts.rocksdb_waldir)
      elif f.endswith(rocksdb_data_suffix) or is_manifest(f):
        shutil.move(f, opts.rocksdb_datadir)
      else:
        shutil.move(f, opts.datadir)
    else: #directory
      if f.endswith('.rocksdb'):
        continue
      shutil.move(f, opts.datadir)

def start_backup():
  logger.info("Starting backup.")
  runner = BackupRunner(opts.datadir)
  b = None
  backup_round= 1
  while True:
    b = runner.start_backup_round(backup_round, b)
    backup_round = backup_round + 1
    if b.finished is True:
      b.print_backup_report()
      logger.info("RocksDB Backup Done.")
      break
  if opts.debug_signal_file:
    while not os.path.exists(opts.debug_signal_file):
      logger.info("Waiting until %s is created..", opts.debug_signal_file)
      time.sleep(1)
  runner.backup_mysql()
  logger.info("All Backups Done.")


def main():
  parse_options()
  init_logger()

  if opts.move_back is True:
    move_back()
  elif opts.output_stream == 'wdt':
    backup_using_wdt()
  else:
    start_backup()

if __name__ == "__main__":
  main()
