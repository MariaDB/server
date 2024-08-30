#ifndef STORAGE_PERFSCHEMA_RPL_GTID_INCLUDED
#define STORAGE_PERFSCHEMA_RPL_GTID_INCLUDED

struct TABLE;

#include "../../sql/rpl_gtid.h"

class Gtid_specification: public rpl_gtid
{
public:
  size_t to_string(char *buf)
  {
    return my_snprintf(buf, GTID_MAX_STR_LENGTH, "%u-%u-%llu",
                       domain_id, server_id, seq_no);
  }
};
#endif
