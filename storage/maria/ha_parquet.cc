#include "ha_parquet"

#define MYSQL_SERVER 1

#include <my_global.h>
#include <m_string.h>
#include "maria_def.h"
#include "sql_class.h"
#include <mysys_err.h>
#include <libmarias3/marias3.h>
#include <discover.h>
#include "ha_s3.h"
#include "s3_func.h"
#include "aria_backup.h"

#define DEFAULT_AWS_HOST_NAME "s3.amazonaws.com"


handlerton *parquet_hton= 0;



static int ha_parquet_init(void *p)
{

}


