from enum import Enum

class ClientState:
  def __init__(self, warnings, errors):
    self.warnings = warnings
    self.errors = errors

class GTID:
  def __init__(self, domain_id, server_id, seq_no):
    self.domain_id = domain_id
    self.server_id = server_id
    self.seq_no = seq_no

  def __str__(self):
    return "{0}-{1}-{2}".format(self.domain_id, self.server_id, self.seq_no)

  def print_self(self):
    print('I am {0}'.format(self))

class EventInfo(GTID):
  def __init__(self, gtid, log_file, log_pos, ev_type):
    self.gtid = gtid
    self.log_file = log_file
    self.log_pos = log_pos
    self.ev_type = ev_type

class BinlogEventType(Enum):
  QUERY_EVENT=2
  ROTATE_EVENT=4
  FORMAT_DESCRIPTION=15
  XID_EVENT=16
  TABLE_MAP=19
  WRITE_ROWS=30
  UPDATE_ROWS=31
  DELETE_ROWS=32
  XA_PREPARE=38
  BINLOG_CHECKPOINT=161
  GTID=162
  GTID_LIST=163
  START_ENCRYPTION=164
  QUERY_COMPRESSED=165
  WRITE_ROWS_COMPRESSED=169
  UPDATE_ROWS_COMPRESSED=170
  DELETE_ROWS_COMPRESSED=171


class Event:
  def __init__(self, log_file, log_pos, ev_type, ev_bytes):
    self.ev_type= ev_type
    self.log_file = log_file
    self.log_pos = log_pos
    self.ev_bytes = ev_bytes

class RotateEvent(Event):
  def __init__(self):
    Event.__init__(self, BinlogEventType.ROTATE_EVENT)

class FormatDescriptonEvent(Event):
  def __init__(self):
    Event.__init__(self, BinlogEventType.FORMAT_DESCRIPTION)

class GtidListEvent(Event):
  def __init__(self, gtids):
    Event.__init__(self, BinlogEventType.GTID_LIST)
    self.gtids = gtids

class TrxEvent(Event):
  def __init__(self, log_file, log_pos, ev_type, ev_bytes, gtid):
    Event.__init__(self, log_file, log_pos, ev_type, ev_bytes)
    self.gtid= gtid

  def __str__(self):
    return "Event exists! to {0}".format(self.gtid)


#
#
# next_event(Event event, TransactionInfo trx_info):
#