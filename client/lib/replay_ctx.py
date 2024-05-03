from binlog_types import GTID
import json
import pprint

class GtidState(dict):
  def __str__(self):
    return json.dumps(self)

class ReplayContext:
  def __init__(self):
    self.gtid_state= GtidState()
    self.last_gtid_processed= GtidState()

  def new_gtid(self, gtid):
    if (gtid.domain_id not in self.gtid_state or gtid.seq_no > self.gtid_state[gtid.domain_id].seq_no):
      self.gtid_state[gtid.domain_id]= gtid
    self.last_gtid_processed[gtid.domain_id]= gtid

  def __str__(self):
    repr= "Replay Context:\n\tState: "
    for key in self.gtid_state.keys():
      repr+= "\n\t\t" + str(key) + " -> " + str(self.gtid_state[key])
    repr+= "\n\tLast GTID Processed: "
    for key in self.last_gtid_processed.keys():
      repr+= "\n\t\t" + str(key) + " -> " + str(self.last_gtid_processed[key])
    return repr
