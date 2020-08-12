/* Copyright (C) 2007-2015 Arjen G Lentz & Antony T Curtis for Open Query
   Copyright (C) 2013-2015 Andrew McDonnell
   Copyright (C) 2014 Sergei Golubchik
   Portions of this file copyright (C) 2000-2006 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* ======================================================================
   Open Query Graph Computation Engine, based on a concept by Arjen Lentz
   v3 implementation by Antony Curtis, Arjen Lentz, Andrew McDonnell
   For more information, documentation, support, enhancement engineering,
   see http://openquery.com/graph or contact graph@openquery.com
   ======================================================================
*/

/*
   Changelog since 10.0.13
   -----------------------
   * Removed compatibility hacks for 5.5.32 and 10.0.4.
     I expect no issues building oqgraph into Mariadb 5.5.40 but I think the better approach is maintain a separate fork / patches.
   * Added status variable to report if verbose debug is on
   * Fixed handling of connection thread changed, the apparent root cause of
     MDEV-6282, MDEV-6345 and MDEV-6784

*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation                          // gcc: Class implementation
#endif

#include <my_global.h>
#define MYSQL_SERVER 1                          // to have THD
/* For the moment, include code to deal with integer latches.
 * I have wrapped it with this #ifdef to make it easier to find and remove in the future.
 */
#define RETAIN_INT_LATCH_COMPATIBILITY          // for the time being, recognise integer latches to simplify upgrade.

#include <mysql/plugin.h>
#include <mysql_version.h>
#include "ha_oqgraph.h"
#include "graphcore.h"
#include <sql_error.h>
#include <sql_class.h>
#include <table.h>
#include <field.h>
#include <key.h>
#include <unireg.h>
#include <my_dbug.h>

// Uncomment this for extra debug, but expect a performance hit in large queries
//#define VERBOSE_DEBUG
#ifdef VERBOSE_DEBUG
#else
#undef DBUG_PRINT
#define DBUG_PRINT(x,y)
#endif

#ifdef RETAIN_INT_LATCH_COMPATIBILITY
/* In normal operation, no new tables using an integer latch can be created,
 * but they can still be used if they already exist, to allow for upgrades.
 *
 * However to ensure the legacy function is properly tested, we add a
 * server variable "oggraph_allow_create_integer_latch" which if set to TRUE
 * allows new engine tables to be created with integer latches.
 */

static my_bool g_allow_create_integer_latch = FALSE;
#endif

using namespace open_query;

// Table of varchar latch operations.
// In the future this needs to be refactactored to live somewhere else
struct oqgraph_latch_op_table { const char *key; int latch; };
static const oqgraph_latch_op_table latch_ops_table[] = {
  { "", oqgraph::NO_SEARCH } ,  // suggested by Arjen, use empty string instead of no_search
  { "dijkstras", oqgraph::DIJKSTRAS } ,
  { "breadth_first", oqgraph::BREADTH_FIRST } ,
  { "leaves", oqgraph::LEAVES },
  { NULL, -1 }
};

static uint32 findLongestLatch() {
  int len = 0;
  for (const oqgraph_latch_op_table* k=latch_ops_table; k && k->key; k++) {
    int s = strlen(k->key);
    if (s > len) {
      len = s;
    }
  }
  return len;
}

const char *oqlatchToCode(int latch) {
  for (const oqgraph_latch_op_table* k=latch_ops_table; k && k->key; k++) {
    if (k->latch == latch) {
      return k->key;
    }
  }
  return "unknown";
}

struct ha_table_option_struct
{
  const char *table_name;
  const char *origid; // name of the origin id column
  const char *destid; // name of the target id column
  const char *weight; // name of the weight column (optional)
};

static const ha_create_table_option oqgraph_table_option_list[]=
{
  HA_TOPTION_STRING("data_table", table_name),
  HA_TOPTION_STRING("origid", origid),
  HA_TOPTION_STRING("destid", destid),
  HA_TOPTION_STRING("weight", weight),
  HA_TOPTION_END
};

static bool oqgraph_init_done= 0;

static handler* oqgraph_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       MEM_ROOT *mem_root)
{
  DBUG_PRINT( "oq-debug", ("oqgraph_create_handler"));
  return new (mem_root) ha_oqgraph(hton, table);
}

#define OQGRAPH_CREATE_TABLE                              \
"         CREATE TABLE oq_graph (                        "\
"           latch VARCHAR(32) NULL,                      "\
"           origid BIGINT UNSIGNED NULL,                 "\
"           destid BIGINT UNSIGNED NULL,                 "\
"           weight DOUBLE NULL,                          "\
"           seq BIGINT UNSIGNED NULL,                    "\
"           linkid BIGINT UNSIGNED NULL,                 "\
"           KEY (latch, origid, destid) USING HASH,      "\
"           KEY (latch, destid, origid) USING HASH       "\
"         )                                              "

#define append_opt(NAME,VAL)                              \
  if (share->option_struct->VAL)                          \
  {                                                       \
    const char *val= share->option_struct->VAL;           \
    sql.append(STRING_WITH_LEN(" " NAME "='"));           \
    sql.append_for_single_quote(val, strlen(val));        \
    sql.append('\'');                                     \
  }

int oqgraph_discover_table_structure(handlerton *hton, THD* thd,
                                     TABLE_SHARE *share, HA_CREATE_INFO *info)
{
  StringBuffer<1024> sql(system_charset_info);
  sql.copy(STRING_WITH_LEN(OQGRAPH_CREATE_TABLE), system_charset_info);
  append_opt("data_table", table_name);
  append_opt("origid", origid);
  append_opt("destid", destid);
  append_opt("weight", weight);

  return
    share->init_from_sql_statement_string(thd, true, sql.ptr(), sql.length());
}

int oqgraph_close_connection(handlerton *hton, THD *thd);

static int oqgraph_init(void *p)
{
  handlerton *hton= (handlerton *)p;
  DBUG_PRINT( "oq-debug", ("oqgraph_init"));

  hton->db_type= DB_TYPE_AUTOASSIGN;
  hton->create= oqgraph_create_handler;
  hton->flags= HTON_ALTER_NOT_SUPPORTED;
  // Prevent ALTER, because the core crashes when the user provides a
  // non-existing backing store field for ORIGID, etc
  // 'Fixes' bug 1134355
  // HTON_NO_FLAGS;

  hton->table_options= (ha_create_table_option*)oqgraph_table_option_list;

  hton->discover_table_structure= oqgraph_discover_table_structure;

  hton->close_connection = oqgraph_close_connection;
  hton->drop_table= [](handlerton *, const char*) { return 0; };

  oqgraph_init_done= TRUE;
  return 0;
}

static int oqgraph_fini(void *)
{
  DBUG_PRINT( "oq-debug", ("oqgraph_fini"));
  oqgraph_init_done= FALSE;
  return 0;
}

static int error_code(int res)
{
  switch (res)
  {
  case oqgraph::OK:
    return 0;
  case oqgraph::NO_MORE_DATA:
    return HA_ERR_END_OF_FILE;
  case oqgraph::EDGE_NOT_FOUND:
    return HA_ERR_KEY_NOT_FOUND;
  case oqgraph::INVALID_WEIGHT:
    return HA_ERR_AUTOINC_ERANGE;
  case oqgraph::DUPLICATE_EDGE:
    return HA_ERR_FOUND_DUPP_KEY;
  case oqgraph::CANNOT_ADD_VERTEX:
  case oqgraph::CANNOT_ADD_EDGE:
    return HA_ERR_RECORD_FILE_FULL;
  case oqgraph::MISC_FAIL:
  default:
    return HA_ERR_CRASHED_ON_USAGE;
  }
}

/**
 * Check if table complies with our designated structure
 *
 *    ColName    Type      Attributes
 *    =======    ========  =============
 *    latch     VARCHAR   NULL
 *    origid    BIGINT    UNSIGNED NULL
 *    destid    BIGINT    UNSIGNED NULL
 *    weight    DOUBLE    NULL
 *    seq       BIGINT    UNSIGNED NULL
 *    linkid    BIGINT    UNSIGNED NULL
 *    =================================
 *

  The latch may be a varchar of any length, however if it is too short to
  hold the longest latch value, table creation is aborted.

  CREATE TABLE foo (
    latch   VARCHAR(32)   NULL,
    origid  BIGINT    UNSIGNED NULL,
    destid  BIGINT    UNSIGNED NULL,
    weight  DOUBLE    NULL,
    seq     BIGINT    UNSIGNED NULL,
    linkid  BIGINT    UNSIGNED NULL,
    KEY (latch, origid, destid) USING HASH,
    KEY (latch, destid, origid) USING HASH
  ) ENGINE=OQGRAPH
    DATA_TABLE=bar
    ORIGID=src_id
    DESTID=tgt_id

 Previously latch could be an integer.
 We no longer allow new integer tables to be created, but we need to support
 them if in use and this module is upgraded.
 So when the table is opened we need to see whether latch is a varchar or
 integer and change behaviour accordingly.
 Note that if a table was constructed with varchar and an attempt is made to
 select with latch=(some integer number) then MYSQL will autocast
 and no data will be returned... so retaining compatibility does not and cannot
 extend to making old queries work with new style tables.

  This method is only called on table creation, so here we ensure new tables
  can only be created with varchar.

  This does present a small problem with regression testing;
  so we work around that by using an system variable to allow
  integer latch tables to be created.

 */
int ha_oqgraph::oqgraph_check_table_structure (TABLE *table_arg)
{
  // Changed from static so we can do decent error reporting.

  int i;
  struct { const char *colname; int coltype; } skel[] = {
    { "latch" , MYSQL_TYPE_VARCHAR },
    { "origid", MYSQL_TYPE_LONGLONG },
    { "destid", MYSQL_TYPE_LONGLONG },
    { "weight", MYSQL_TYPE_DOUBLE },
    { "seq"   , MYSQL_TYPE_LONGLONG },
    { "linkid", MYSQL_TYPE_LONGLONG },
  { NULL    , 0}
  };

  DBUG_ENTER("oqgraph_check_table_structure");

  DBUG_PRINT( "oq-debug", ("Checking structure."));

  Field **field= table_arg->field;
  for (i= 0; *field && skel[i].colname; i++, field++) {
    DBUG_PRINT( "oq-debug", ("Column %d: name='%s', expected '%s'; type=%d, expected %d.", i, (*field)->field_name.str, skel[i].colname, (*field)->type(), skel[i].coltype));
    bool badColumn = false;
    bool isLatchColumn = strcmp(skel[i].colname, "latch")==0;
    bool isStringLatch = true;

#ifdef RETAIN_INT_LATCH_COMPATIBILITY
    if (g_allow_create_integer_latch && isLatchColumn && ((*field)->type() == MYSQL_TYPE_SHORT))
    {
      DBUG_PRINT( "oq-debug", ("Allowing integer latch anyway!"));
      isStringLatch = false;
      /* Make a warning */
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
            ER_WARN_DEPRECATED_SYNTAX, ER(ER_WARN_DEPRECATED_SYNTAX),
            "latch SMALLINT UNSIGNED NULL", "'latch VARCHAR(32) NULL'");
    } else
#endif
    if (isLatchColumn && ((*field)->type() == MYSQL_TYPE_SHORT))
    {
      DBUG_PRINT( "oq-debug", ("Allowing integer no more!"));
      badColumn = true;
      push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Integer latch is not supported for new tables.", i);
    } else
    /* Check Column Type */
    if ((*field)->type() != skel[i].coltype) {
      badColumn = true;
      push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Column %d is wrong type.", i);
    }

    // Make sure latch column is large enough for all possible latch values
    if (isLatchColumn && isStringLatch) {
      if ((*field)->char_length() < findLongestLatch()) {
        badColumn = true;
        push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Column %d is too short.", i);
      }
    }

    if (!badColumn) if (skel[i].coltype != MYSQL_TYPE_DOUBLE && (!isLatchColumn || !isStringLatch)) {
      /* Check Is UNSIGNED */
      if ( (!((*field)->flags & UNSIGNED_FLAG ))) {
        badColumn = true;
        push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Column %d must be UNSIGNED.", i);
      }
    }
    /* Check THAT  NOT NULL isn't set */
    if (!badColumn) if ((*field)->flags & NOT_NULL_FLAG) {
      badColumn = true;
      push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Column %d must be NULL.", i);
    }
    /* Check the column name */
    if (!badColumn) if (strcmp(skel[i].colname,(*field)->field_name.str)) {
      badColumn = true;
      push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Column %d must be named '%s'.", i, skel[i].colname);
    }
    if (badColumn) {
      DBUG_RETURN(-1);
    }
  }

  if (skel[i].colname) {
    push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Not enough columns.");
    DBUG_RETURN(-1);
  }
  if (*field) {
    push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Too many columns.");
    DBUG_RETURN(-1);
  }

  if (!table_arg->key_info || !table_arg->s->keys) {
    push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "No valid key specification.");
    DBUG_RETURN(-1);
  }

  DBUG_PRINT( "oq-debug", ("Checking keys."));

  KEY *key= table_arg->key_info;
  for (uint i= 0; i < table_arg->s->keys; ++i, ++key)
  {
    Field **field= table_arg->field;
    /* check that the first key part is the latch and it is a hash key */
    if (!(field[0] == key->key_part[0].field &&
          HA_KEY_ALG_HASH == key->algorithm)) {
      push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Incorrect keys algorithm on key %d.", i);
      DBUG_RETURN(-1);
    }
    if (key->user_defined_key_parts == 3)
    {
      /* KEY (latch, origid, destid) USING HASH */
      /* KEY (latch, destid, origid) USING HASH */
      if (!(field[1] == key->key_part[1].field &&
            field[2] == key->key_part[2].field) &&
          !(field[1] == key->key_part[2].field &&
            field[2] == key->key_part[1].field))
      {
        push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Keys parts mismatch on key %d.", i);
        DBUG_RETURN(-1);
      }
    }
    else {
      push_warning_printf( current_thd, Sql_condition::WARN_LEVEL_WARN, HA_WRONG_CREATE_OPTION, "Too many key parts on key %d.", i);
      DBUG_RETURN(-1);
    }
  }

  DBUG_RETURN(0);
}

/*****************************************************************************
** OQGRAPH tables
*****************************************************************************/

int oqgraph_close_connection(handlerton *hton, THD *thd)
{
  DBUG_PRINT( "oq-debug", ("thd: 0x%lx; oqgraph_close_connection.", (long) thd));
  // close_thread_tables(thd); // maybe this?
  return 0;
}


ha_oqgraph::ha_oqgraph(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
  , have_table_share(false)
  , origid(NULL)
  , destid(NULL)
  , weight(NULL)
  , graph_share(0)
  , graph(0)
  , error_message("", 0, &my_charset_latin1)
{
}

ha_oqgraph::~ha_oqgraph()
{ }

static const char *ha_oqgraph_exts[] =
{
  NullS
};

const char **ha_oqgraph::bas_ext() const
{
  return ha_oqgraph_exts;
}

ulonglong ha_oqgraph::table_flags() const
{
  return (HA_NO_BLOBS | HA_NULL_IN_KEY |
          HA_REC_NOT_IN_SEQ | HA_CAN_INSERT_DELAYED |
          HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE);
}

ulong ha_oqgraph::index_flags(uint inx, uint part, bool all_parts) const
{
  return HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR;
}

bool ha_oqgraph::get_error_message(int error, String* buf)
{
  if (error < 0)
  {
    buf->append(error_message);
    buf->c_ptr_safe();
    error_message.length(0);
  }
  return false;
}

void ha_oqgraph::fprint_error(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  error_message.reserve(256);
  size_t len = error_message.length();
  len += vsnprintf(&error_message[len], 255, fmt, ap);
  error_message.length(len);
  va_end(ap);
}

/**
 * Check that the currently referenced OQGRAPH table definition, on entry to open(), has sane OQGRAPH options.
 * (This does not check the backing store, but the OQGRAPH virtual table options)
 *
 * @return true if OK, or false if an option is invalid.
 */
bool ha_oqgraph::validate_oqgraph_table_options() 
{
  // Note when called from open(), we should not expect this method to fail except in the case of bugs; the fact that it does is
  // could be construed as a bug. I think in practice however, this is because CREATE TABLE calls both create() and open(),
  // and it is possible to do something like ALTER TABLE x DESTID='y' to _change_ the options.
  // Thus we need to sanity check from open() until and unless we get around to extending ha_oqgraph to properly handle ALTER TABLE, 
  // after which we could change things to call this method from create() and the ALTER TABLE handling code instead.
  // It may still be sensible to call this from open() anyway, in case someone somewhere upgrades from a broken table definition...

  ha_table_option_struct *options = table->s->option_struct;
  // Catch cases where table was not constructed properly
  // Note - need to return -1 so our error text gets reported
  if (!options) {
    // This should only happen if there is a bug elsewhere in the storage engine, because ENGINE itself is an attribute
    fprint_error("Invalid OQGRAPH backing store (null attributes)");
  }
  else if (!options->table_name || !*options->table_name) {
    // The first condition indicates no DATA_TABLE option, the second if the user specified DATA_TABLE=''
    fprint_error("Invalid OQGRAPH backing store description (unspecified or empty data_table attribute)");
    // if table_name option is present but doesn't actually exist, we will fail later
  }
  else if (!options->origid || !*options->origid) {
    // The first condition indicates no ORIGID option, the second if the user specified ORIGID=''
    fprint_error("Invalid OQGRAPH backing store description (unspecified or empty origid attribute)");
    // if ORIGID option is present but doesn't actually exist, we will fail later
  }
  else if (!options->destid || !*options->destid) {
    // The first condition indicates no DESTID option, the second if the user specified DESTID=''
    fprint_error("Invalid OQGRAPH backing store description (unspecified or empty destid attribute)");
    // if DESTID option is present but doesn't actually exist, we will fail later
  } else {
    // weight is optional...
    return true;
  }
  // Fault
  return false;
}

/**
 * Open the OQGRAPH engine 'table'.
 *
 * An OQGRAPH table is effectively similar to a view over the underlying backing table, attribute 'data_table', but where the
 * result returned by a query depends on the value of the 'latch' column specified to the query.
 * Therefore, when mysqld opens us, we need to open the corresponding backing table 'data_table'.
 *
 * Conceptually, the backing store could be any kind of object having queryable semantics, including a SQL VIEW.
 * However, for that to work in practice would require us to hook into the right level of the MYSQL API.
 * Presently, only objects that can be opened using the internal mechanisms can be used: INNODB, MYISAM, etc.
 * The intention is to borrow from ha_connect and use the mysql client library to access the backing store.
 *
 */
int ha_oqgraph::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_oqgraph::open");
  DBUG_PRINT( "oq-debug", ("thd: 0x%lx; open(name=%s,mode=%d,test_if_locked=%u)", (long) current_thd, name, mode, test_if_locked));

  // So, we took a peek inside handler::ha_open() and learned a few things:
  // * this->table is set by handler::ha_open() before calling open().
  //   Note that from this we can only assume that MariaDB knows what it is doing and wont call open() other anything else
  //   relying on this-0>table, re-entrantly...
  // * this->table_share should never be set back to NULL, an assertion checks for this in ha_open() after open()
  // * this->table_share is initialised in the constructor of handler
  // * this->table_share is only otherwise changed by this->change_table_ptr())
  // We also discovered that an assertion is raised if table->s is not table_share before calling open())

  DBUG_ASSERT(!have_table_share);
  DBUG_ASSERT(graph == NULL);

  // Before doing anything, make sure we have DATA_TABLE, ORIGID and DESTID not empty
  if (!validate_oqgraph_table_options()) { DBUG_RETURN(-1); }

  ha_table_option_struct *options= table->s->option_struct;


  error_message.length(0);
  origid= destid= weight= 0;

  // Here we're abusing init_tmp_table_share() which is normally only works for thread-local shares.
  THD* thd = current_thd;
  init_tmp_table_share( thd, share, table->s->db.str, table->s->db.length, options->table_name, "");
  // because of that, we need to reinitialize the memroot (to reset MY_THREAD_SPECIFIC flag)
  DBUG_ASSERT(share->mem_root.used == NULL); // it's still empty
  init_sql_alloc(PSI_INSTRUMENT_ME, &share->mem_root, TABLE_ALLOC_BLOCK_SIZE, 0, MYF(0));

  // What I think this code is doing:
  // * Our OQGRAPH table is `database_blah/name`
  // * We point p --> /name (or if table happened to be simply `name`, to `name`, don't know if this is possible)
  // * plen seems to be then set to length of `database_blah/options_data_table_name`
  // * then we set share->normalized_path.str and share->path.str to `database_blah/options_data_table_name`
  // * I assume that this verbiage is needed so  the memory used by share->path.str is set in the share mem root
  // * because otherwise one could simply build the string more simply using malloc and pass it instead of "" above
  const char* p= strend(name)-1;
  while (p > name && *p != '\\' && *p != '/')
    --p;
  size_t tlen= strlen(options->table_name);
  size_t plen= (int)(p - name) + tlen + 1;

  share->path.str= (char*)alloc_root(&share->mem_root, plen + 1);
  strmov(strnmov((char*) share->path.str, name, (int)(p - name) + 1),
         options->table_name);
  DBUG_ASSERT(strlen(share->path.str) == plen);
  share->normalized_path.str= share->path.str;
  share->path.length= share->normalized_path.length= plen;

  DBUG_PRINT( "oq-debug", ("share:(normalized_path=%s,path.length=%zu)",
              share->normalized_path.str, share->path.length));

  int open_def_flags = 0;
  open_def_flags = GTS_TABLE;

  // We want to open the definition for the given backing table
  // Once can assume this loop exists because sometimes open_table_def() fails for a reason other than not exist
  // and not 'exist' is valid, because we use ha_create_table_from_engine() to force it to 'exist'
  // But, ha_create_table_from_engine() is removed in MariaDB 10.0.4 (?)
  // Looking inside most recent ha_create_table_from_engine(), it also calls open_table_def() so maybe this whole thing is redundant...
  // Or perhaps it is needed if the backing store is a temporary table or maybe if has no records as yet...?
  // Lets try without this, and see if all the tests pass...
  while (open_table_def(thd, share, open_def_flags))
  {
    open_table_error(share, OPEN_FRM_OPEN_ERROR, ENOENT);
    free_table_share(share);
    if (thd->is_error())
      DBUG_RETURN(thd->get_stmt_da()->sql_errno());
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  }


  if (int err= share->error)
  {
    open_table_error(share, share->error, share->open_errno);
    free_table_share(share);
    DBUG_RETURN(err);
  }

  if (share->is_view)
  {
    free_table_share(share);
    fprint_error("VIEWs are not supported for an OQGRAPH backing store.");
    DBUG_RETURN(-1);
  }

  if (enum open_frm_error err= open_table_from_share(thd, share,
                                                     &empty_clex_str,
                            (uint) (HA_OPEN_KEYFILE | HA_TRY_READ_ONLY),
                            EXTRA_RECORD,
                            thd->open_options, edges, FALSE))
  {
    open_table_error(share, err, EMFILE); // NOTE - EMFILE is probably bogus, it reports as too many open files (!)
    free_table_share(share);
    DBUG_RETURN(-1);
  }


  if (!edges->file)
  {
    fprint_error("Some error occurred opening table '%s'", options->table_name);
    free_table_share(share);
    DBUG_RETURN(-1);
  }

  edges->reginfo.lock_type= TL_READ;

  edges->tablenr= thd->current_tablenr++;
  edges->status= STATUS_NO_RECORD;
  edges->file->ft_handler= 0;
  edges->pos_in_table_list= 0;
  edges->clear_column_bitmaps();
  bfill(table->record[0], table->s->null_bytes, 255);
  bfill(table->record[1], table->s->null_bytes, 255);

  // We expect fields origid, destid and optionally weight
  origid= destid= weight= 0;

  for (Field **field= edges->field; *field; ++field)
  {
    if (strcmp(options->origid, (*field)->field_name.str))
      continue;
    if ((*field)->cmp_type() != INT_RESULT ||
        !((*field)->flags & NOT_NULL_FLAG))
    {
      fprint_error("Column '%s.%s' (origid) is not a not-null integer type",
          options->table_name, options->origid);
      closefrm(edges);
      free_table_share(share);
      DBUG_RETURN(-1);
    }
    origid = *field;
    break;
  }

  if (!origid) {
    fprint_error("Invalid OQGRAPH backing store ('%s.origid' attribute not set to a valid column of '%s')", p+1, options->table_name);
    closefrm(edges);
    free_table_share(share);
    DBUG_RETURN(-1);
  }


  for (Field **field= edges->field; *field; ++field)
  {
    if (strcmp(options->destid, (*field)->field_name.str))
      continue;
    if ((*field)->type() != origid->type() ||
        !((*field)->flags & NOT_NULL_FLAG))
    {
      fprint_error("Column '%s.%s' (destid) is not a not-null integer type or is a different type to origid attribute.",
          options->table_name, options->destid);
      closefrm(edges);
      free_table_share(share);
      DBUG_RETURN(-1);
    }
    destid = *field;
    break;
  }

  if (!destid) {
    fprint_error("Invalid OQGRAPH backing store ('%s.destid' attribute not set to a valid column of '%s')", p+1, options->table_name);
    closefrm(edges);
    free_table_share(share);
    DBUG_RETURN(-1);
  }

  // Make sure origid column != destid column
  if (strcmp( origid->field_name.str, destid->field_name.str)==0) {
    fprint_error("Invalid OQGRAPH backing store ('%s.destid' attribute set to same column as origid attribute)", p+1, options->table_name);
    closefrm(edges);
    free_table_share(share);
    DBUG_RETURN(-1);
  }

  for (Field **field= edges->field; options->weight && *field; ++field)
  {
    if (strcmp(options->weight, (*field)->field_name.str))
      continue;
    if ((*field)->result_type() != REAL_RESULT ||
        !((*field)->flags & NOT_NULL_FLAG))
    {
      fprint_error("Column '%s.%s' (weight) is not a not-null real type",
          options->table_name, options->weight);
      closefrm(edges);
      free_table_share(share);
      DBUG_RETURN(-1);
    }
    weight = *field;
    break;
  }

  if (!weight && options->weight) {
    fprint_error("Invalid OQGRAPH backing store ('%s.weight' attribute not set to a valid column of '%s')", p+1, options->table_name);
    closefrm(edges);
    free_table_share(share);
    DBUG_RETURN(-1);
  }

  if (!(graph_share = oqgraph::create(edges, origid, destid, weight)))
  {
    fprint_error("Unable to create graph instance.");
    closefrm(edges);
    free_table_share(share);
    DBUG_RETURN(-1);
  }
  ref_length= oqgraph::sizeof_ref;

  graph = oqgraph::create(graph_share);
  have_table_share = true;

  DBUG_RETURN(0);
}

int ha_oqgraph::close(void)
{
  DBUG_PRINT( "oq-debug", ("close()"));
  if (graph->get_thd() != current_thd) {
    DBUG_PRINT( "oq-debug", ("index_next_same g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }
  oqgraph::free(graph); graph= 0;
  oqgraph::free(graph_share); graph_share= 0;

  if (have_table_share)
  {
    if (edges->file)
      closefrm(edges);
    free_table_share(share);
    have_table_share = false;
  }
  return 0;
}

void ha_oqgraph::update_key_stats()
{
  DBUG_PRINT( "oq-debug", ("update_key_stats()"));
  for (uint i= 0; i < table->s->keys; i++)
  {
    KEY *key=table->key_info+i;
    if (!key->rec_per_key)
      continue;
    if (key->algorithm != HA_KEY_ALG_BTREE)
    {
      if (key->flags & HA_NOSAME)
        key->rec_per_key[key->user_defined_key_parts-1]= 1;
      else
      {
        //unsigned vertices= graph->vertices_count();
        //unsigned edges= graph->edges_count();
        //uint no_records= vertices ? 2 * (edges + vertices) / vertices : 2;
        //if (no_records < 2)
        uint
          no_records= 2;
        key->rec_per_key[key->user_defined_key_parts-1]= no_records;
      }
    }
  }
  /* At the end of update_key_stats() we can proudly claim they are OK. */
  //skey_stat_version= share->key_stat_version;
}


int ha_oqgraph::write_row(const byte * buf)
{
  return HA_ERR_TABLE_READONLY;
}

int ha_oqgraph::update_row(const uchar * old, const uchar * buf)
{
  return HA_ERR_TABLE_READONLY;
}

int ha_oqgraph::delete_row(const byte * buf)
{
  return HA_ERR_TABLE_READONLY;
}

int ha_oqgraph::index_read(byte * buf, const byte * key, uint key_len, enum ha_rkey_function find_flag)
{
  DBUG_ASSERT(inited==INDEX);
  // reset before we have a cursor, so the memory is not junk, avoiding the sefgault in position() when select with order by (bug #1133093)
  graph->init_row_ref(ref);
  return index_read_idx(buf, active_index, key, key_len, find_flag);
}

int ha_oqgraph::index_next_same(byte *buf, const byte *key, uint key_len)
{
  if (graph->get_thd() != current_thd) {
    DBUG_PRINT( "oq-debug", ("index_next_same g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }
  int res;
  open_query::row row;
  DBUG_ASSERT(inited==INDEX);
  if (!(res= graph->fetch_row(row)))
    res= fill_record(buf, row);
  return error_code(res);
}

#define LATCH_WAS CODE 0
#define LATCH_WAS_NUMBER 1

/**
 * This function parse the VARCHAR(n) latch specification into an integer operation specification compatible with
 * v1-v3 oqgraph::search().
 *
 * If the string contains a number, this is directly converted from a decimal integer.
 *
 * Otherwise, a lookup table is used to convert from a string constant.
 *
 * It is anticipated that this function (and this file and class oqgraph) will be refactored to do this in a nicer way.
 *
 * FIXME: For the time being, only handles latin1 character set.
 * @return false if parsing fails.
 */
static int parse_latch_string_to_legacy_int(const String& value, int &latch)
{
  // Attempt to parse as exactly an integer first.

  // Note: we are strict about not having whitespace, or garbage characters,
  // so that the query result gets returned properly:
  // Because of the way the result is built and used in fill_result,
  // we have to exactly return in the latch column what was in the latch= clause
  // otherwise the rows get filtered out by the query optimiser.

  // For the same reason, we cant simply treat latch='' as NO_SEARCH either.

  String latchValue = value;
  char *eptr;
  unsigned long int v = strtoul( latchValue.c_ptr_safe(), &eptr, 10);
  if (!*eptr) {
    // we had an unsigned number; remember 0 is valid too ('vertices' aka 'no_search'))
    if (v < oqgraph::NUM_SEARCH_OP) {
      latch = v;
      return true;
    }
    // fall through  and test as a string (although it is unlikely we might have an operator starting with a number)
  }

  const oqgraph_latch_op_table* entry = latch_ops_table;
  for ( ; entry->key ; entry++) {
    if (0 == strncmp(entry->key, latchValue.c_ptr_safe(), latchValue.length())) {
      latch = entry->latch;
      return true;
    }
  }
  return false;
}

int ha_oqgraph::index_read_idx(byte * buf, uint index, const byte * key,
                        uint key_len, enum ha_rkey_function find_flag)
{
  if (graph->get_thd() != current_thd) {
    DBUG_PRINT( "oq-debug", ("index_read_idx g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }

  Field **field= table->field;
  KEY *key_info= table->key_info + index;
  int res;
  VertexID orig_id, dest_id;
  int latch;
  VertexID *orig_idp=0, *dest_idp=0;
  int* latchp=0;
  open_query::row row;

  DBUG_PRINT("oq-debug", ("thd: 0x%lx; index_read_idx()", (long) current_thd));

  bmove_align(buf, table->s->default_values, table->s->reclength);
  key_restore(buf, (byte*) key, key_info, key_len);

  my_bitmap_map *old_map= dbug_tmp_use_all_columns(table, table->read_set);
  my_ptrdiff_t ptrdiff= buf - table->record[0];

  if (ptrdiff)
  {
    field[0]->move_field_offset(ptrdiff);
    field[1]->move_field_offset(ptrdiff);
    field[2]->move_field_offset(ptrdiff);
  }

  String latchFieldValue;
  if (!field[0]->is_null())
  {
#ifdef RETAIN_INT_LATCH_COMPATIBILITY
    if (field[0]->type() == MYSQL_TYPE_SHORT) {
      latch= (int) field[0]->val_int();
    } else
#endif
    {
      field[0]->val_str(&latchFieldValue, &latchFieldValue);
      if (!parse_latch_string_to_legacy_int(latchFieldValue, latch)) {
        // Invalid, so warn & fail
        push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN, ER_WRONG_ARGUMENTS, ER(ER_WRONG_ARGUMENTS), "OQGRAPH latch");
        if (ptrdiff) /* fixes debug build assert - should be a tidier way to do this */
        {
          field[0]->move_field_offset(-ptrdiff);
          field[1]->move_field_offset(-ptrdiff);
          field[2]->move_field_offset(-ptrdiff);
        }
        dbug_tmp_restore_column_map(table->read_set, old_map);
        return error_code(oqgraph::NO_MORE_DATA);
      }
    }
    latchp= &latch;
  }

  if (!field[1]->is_null())
  {
    orig_id= (VertexID) field[1]->val_int();
    orig_idp= &orig_id;
  }

  if (!field[2]->is_null())
  {
    dest_id= (VertexID) field[2]->val_int();
    dest_idp= &dest_id;
  }

  if (ptrdiff)
  {
    field[0]->move_field_offset(-ptrdiff);
    field[1]->move_field_offset(-ptrdiff);
    field[2]->move_field_offset(-ptrdiff);
  }
  dbug_tmp_restore_column_map(table->read_set, old_map);

  // Keep the latch around so we can use it in the query result later -
  // See fill_record().
  // at the moment our best option is to associate it with the graph
  // so we pass the string now.
  // In the future we should refactor parse_latch_string_to_legacy_int()
  // into oqgraph instead.
  if (latchp)
    graph->retainLatchFieldValue(latchFieldValue.c_ptr_safe());
  else
    graph->retainLatchFieldValue(NULL);


  DBUG_PRINT( "oq-debug", ("index_read_idx ::>> search(latch:%s,%ld,%ld)",
          oqlatchToCode(latch), orig_idp?(long)*orig_idp:-1, dest_idp?(long)*dest_idp:-1));

  res= graph->search(latchp, orig_idp, dest_idp);

  DBUG_PRINT( "oq-debug", ("search() = %d", res));

  if (!res && !(res= graph->fetch_row(row))) {
    res= fill_record(buf, row);
  }
  return error_code(res);
}

int ha_oqgraph::fill_record(byte *record, const open_query::row &row)
{
  Field **field= table->field;

  bmove_align(record, table->s->default_values, table->s->reclength);

  my_bitmap_map *old_map= dbug_tmp_use_all_columns(table, table->write_set);
  my_ptrdiff_t ptrdiff= record - table->record[0];

  if (ptrdiff)
  {
    field[0]->move_field_offset(ptrdiff);
    field[1]->move_field_offset(ptrdiff);
    field[2]->move_field_offset(ptrdiff);
    field[3]->move_field_offset(ptrdiff);
    field[4]->move_field_offset(ptrdiff);
    field[5]->move_field_offset(ptrdiff);
  }

  DBUG_PRINT( "oq-debug", ("fill_record() ::>> %s,%ld,%ld,%lf,%ld,%ld",
          row.latch_indicator ? oqlatchToCode((int)row.latch) : "-",
          row.orig_indicator ? (long)row.orig : -1,
          row.dest_indicator ? (long)row.dest : -1,
          row.weight_indicator ? (double)row.weight : -1,
          row.seq_indicator ? (long)row.seq : -1,
          row.link_indicator ? (long)row.link : -1));

  // just each field specifically, no sense iterating
  if (row.latch_indicator)
  {
    field[0]->set_notnull();
    // Convert the latch back to a varchar32
    if (field[0]->type() == MYSQL_TYPE_VARCHAR) {
      field[0]->store(row.latchStringValue, row.latchStringValueLen, &my_charset_latin1);
    }
#ifdef RETAIN_INT_LATCH_COMPATIBILITY
    else if (field[0]->type() == MYSQL_TYPE_SHORT) {
      field[0]->store((longlong) row.latch, 0);
    }
#endif

  }

  if (row.orig_indicator)
  {
    field[1]->set_notnull();
    field[1]->store((longlong) row.orig, 0);
  }

  if (row.dest_indicator)
  {
    field[2]->set_notnull();
    field[2]->store((longlong) row.dest, 0);
  }

  if (row.weight_indicator)
  {
    field[3]->set_notnull();
    field[3]->store((double) row.weight);
  }

  if (row.seq_indicator)
  {
    field[4]->set_notnull();
    field[4]->store((longlong) row.seq, 0);
  }

  if (row.link_indicator)
  {
    field[5]->set_notnull();
    field[5]->store((longlong) row.link, 0);
  }

  if (ptrdiff)
  {
    field[0]->move_field_offset(-ptrdiff);
    field[1]->move_field_offset(-ptrdiff);
    field[2]->move_field_offset(-ptrdiff);
    field[3]->move_field_offset(-ptrdiff);
    field[4]->move_field_offset(-ptrdiff);
    field[5]->move_field_offset(-ptrdiff);
  }
  dbug_tmp_restore_column_map(table->write_set, old_map);

  return 0;
}

int ha_oqgraph::rnd_init(bool scan)
{
  edges->file->info(HA_STATUS_VARIABLE|HA_STATUS_CONST); // Fix for bug 1195735, hang after truncate table - ensure we operate with up to date count
  edges->prepare_for_position();
  return error_code(graph->random(scan));
}

int ha_oqgraph::rnd_next(byte *buf)
{
  if (graph->get_thd() != current_thd) {
    DBUG_PRINT( "oq-debug", ("rnd_next g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }
  int res;
  open_query::row row = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

  if (!(res= graph->fetch_row(row)))
    res= fill_record(buf, row);
  return error_code(res);
}

int ha_oqgraph::rnd_pos(byte * buf, byte *pos)
{
  if (graph->get_thd() != current_thd) {
    DBUG_PRINT( "oq-debug", ("rnd_pos g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }
  int res;
  open_query::row row;
  if (!(res= graph->fetch_row(row, pos)))
    res= fill_record(buf, row);
  return error_code(res);
}

void ha_oqgraph::position(const byte *record)
{
  graph->row_ref((void*) ref); // Ref is aligned
}

int ha_oqgraph::cmp_ref(const byte *ref1, const byte *ref2)
{
  return memcmp(ref1, ref2, oqgraph::sizeof_ref);
}

int ha_oqgraph::info(uint flag)
{
  stats.records = graph->edges_count();

  /*
    If info() is called for the first time after open(), we will still
    have to update the key statistics. Hoping that a table lock is now
    in place.
  */
//  if (key_stat_version != share->key_stat_version)
  //  update_key_stats();
  return 0;
}

int ha_oqgraph::extra(enum ha_extra_function operation)
{
  if (graph->get_thd() != ha_thd()) {
    DBUG_PRINT( "oq-debug", ("rnd_pos g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }
  return edges->file->extra(operation);
}

int ha_oqgraph::delete_all_rows()
{
  return HA_ERR_TABLE_READONLY;
}

int ha_oqgraph::external_lock(THD *thd, int lock_type)
{
  // This method is also called to _unlock_ (lock_type == F_UNLCK)
  // Which means we need to release things before we let the underlying backing table lock go...
  if (lock_type == F_UNLCK) {
    // If we have an index open on the backing table, we need to close it out here
    // this means destroying any open cursor first.
    // Then we can let the unlock go through to the backing table
    graph->release_cursor();
  }

  return edges->file->ha_external_lock(thd, lock_type);
}


THR_LOCK_DATA **ha_oqgraph::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
  return edges->file->store_lock(thd, to, lock_type);
}

/*
  We have to ignore ENOENT entries as the HEAP table is created on open and
  not when doing a CREATE on the table.
*/

int ha_oqgraph::delete_table(const char *)
{
  DBUG_PRINT( "oq-debug", ("delete_table()"));
  return 0;
}

int ha_oqgraph::rename_table(const char *, const char *)
{
  DBUG_PRINT( "oq-debug", ("rename_table()"));
  return 0;
}


ha_rows ha_oqgraph::records_in_range(uint inx,
                                     const key_range *min_key,
                                     const key_range *max_key,
                                     page_range *pages)
{
  if (graph->get_thd() != current_thd) {
    DBUG_PRINT( "oq-debug", ("g->table->in_use: 0x%lx <-- current_thd 0x%lx", (long) graph->get_thd(), (long) current_thd));
    graph->set_thd(current_thd);
  }

  KEY *key=table->key_info+inx;
#ifdef VERBOSE_DEBUG
  {
    String temp;
    key->key_part[0].field->val_str(&temp);
    temp.c_ptr_safe();
    DBUG_PRINT( "oq-debug", ("thd: 0x%lx; records_in_range ::>> inx=%u", (long) current_thd, inx));
    DBUG_PRINT( "oq-debug", ("records_in_range ::>> key0=%s.", temp.c_ptr())); // for some reason when I had  ...inx=%u key=%s", inx, temp.c_ptr_safe()) it printed nothing ...
  }
#endif

  if (!min_key || !max_key ||
      min_key->length != max_key->length ||
      min_key->length < key->key_length - key->key_part[2].store_length ||
      min_key->flag != HA_READ_KEY_EXACT ||
      max_key->flag != HA_READ_AFTER_KEY)
  {
    if (min_key && min_key->length == key->key_part[0].store_length && !key->key_part[0].field->is_null()) /* ensure select * from x where latch is null is consistent with no latch */
    {
      // If latch is not null and equals 0, return # nodes

      // How to decode the key,  For VARCHAR(32), from empirical observation using the debugger
      // and information gleaned from:
      //   http://grokbase.com/t/mysql/internals/095h6ch1q7/parsing-key-information
      //   http://dev.mysql.com/doc/internals/en/support-for-indexing.html#parsing-key-information
      //   comments in opt_range.cc
      // POSSIBLY ONLY VALID FOR INNODB!

      // For a the following query:
      //     SELECT * FROM graph2 WHERE latch = 'breadth_first' AND origid = 123 AND weight = 1;
      // key->key_part[0].field->ptr  is the value of latch, which is a 1-byte string length followed by the value ('breadth_first')
      // key->key_part[2].field->ptr  is the value of origid (123)
      // key->key_part[1].field->ptr  is the value of destid which is not specified in the query so we ignore it in this case
      // so given this ordering we seem to be using the second key specified in create table (aka KEY (latch, destid, origid) USING HASH ))

      // min_key->key[0] is the 'null' bit and contains 0 in this instance
      // min_key->key[1..2] seems to be 16-bit string length
      // min_key->key[3..34] hold the varchar(32) value which is that specified in the query
      // min_key->key[35] is the null bit of origid
      // min_key->key[36..43] is the value in the query (123)

      // max_key->key[0] is the ;null' bit and contains 0 in this instance
      // max_key->key[1..2] seems to be 16-bit string length
      // max_key->key[3..34] hold the varchar(32) value which is that specified in the query
      // max_key->key[35] is the null bit of origid
      // max_key->key[36..43] is the value in the query (123)

      // But after knowing all that, all we care about is the latch value

      // First draft - ignore most of the stuff, but will likely break if query altered

      // It turns out there is a better way though, to access the string,
      // as demonstrated in key_unpack() of sql/key.cc
      String latchCode;
      int latch = -1;
      if (key->key_part[0].field->type() == MYSQL_TYPE_VARCHAR) {

        key->key_part[0].field->val_str(&latchCode);

        parse_latch_string_to_legacy_int( latchCode, latch);
      }

      // what if someone did something dumb, like mismatching the latches?

#ifdef RETAIN_INT_LATCH_COMPATIBILITY
      else if (key->key_part[0].field->type() == MYSQL_TYPE_SHORT) {
        // If not null, and zero ...
        // Note, the following code relies on the fact that the three bytes
        // at beginning of min_key just happen to be the null indicator and the
        // 16-bit value of the latch ...
        // this will fall through if the user alter-tabled to not null
        if (key->key_part[0].null_bit && !min_key->key[0] &&
          !min_key->key[1] && !min_key->key[2]) {
          latch = oqgraph::NO_SEARCH;
        }
      }
#endif
      if (latch != oqgraph::NO_SEARCH) {
        // Invalid key type...
        // Don't assert, in case the user used alter table on us
        return HA_POS_ERROR;    // Can only use exact keys
      }
      unsigned N = graph->vertices_count();
      DBUG_PRINT( "oq-debug", ("records_in_range ::>> N=%u (vertices)", N));
      return N;
    }
    return HA_POS_ERROR;        // Can only use exact keys
  }

  if (stats.records <= 1) {
    DBUG_PRINT( "oq-debug", ("records_in_range ::>> N=%u (stats)", (unsigned)stats.records));
    return stats.records;
  }

  /* Assert that info() did run. We need current statistics here. */
  //DBUG_ASSERT(key_stat_version == share->key_stat_version);
  //ha_rows result= key->rec_per_key[key->user_defined_key_parts-1];
  ha_rows result= 10;
  DBUG_PRINT( "oq-debug", ("records_in_range ::>> N=%u", (unsigned)result));

  return result;
}


int ha_oqgraph::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_oqgraph::create");
  DBUG_PRINT( "oq-debug", ("create(name=%s)", name));

  if (oqgraph_check_table_structure(table_arg)) {
    DBUG_RETURN(HA_WRONG_CREATE_OPTION);
  }

  DBUG_RETURN(0);
}


void ha_oqgraph::update_create_info(HA_CREATE_INFO *create_info)
{
  table->file->info(HA_STATUS_AUTO);
}

// --------------------
// Handler description.
// --------------------


static const char oqgraph_description[]=
  "Open Query Graph Computation Engine "
  "(http://openquery.com/graph)";

struct st_mysql_storage_engine oqgraph_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

extern "C" const char* const oqgraph_boost_version;

static const char *oqgraph_status_verbose_debug =
#ifdef VERBOSE_DEBUG
  "Verbose Debug is enabled. Performance may be adversely impacted.";
#else
  "Verbose Debug is not enabled.";
#endif

static const char *oqgraph_status_latch_compat_mode =
#ifdef RETAIN_INT_LATCH_COMPATIBILITY
  "Legacy tables with integer latches are supported.";
#else
  "Legacy tables with integer latches are not supported.";
#endif

static struct st_mysql_show_var oqgraph_status[]=
{
  { "OQGraph_Boost_Version", (char*) &oqgraph_boost_version, SHOW_CHAR_PTR },
  /* We thought about reporting the Judy version, but there seems to be no way to get that from code in the first place. */
  { "OQGraph_Verbose_Debug", (char*) &oqgraph_status_verbose_debug, SHOW_CHAR_PTR },
  { "OQGraph_Compat_mode",   (char*) &oqgraph_status_latch_compat_mode, SHOW_CHAR_PTR },
  { 0, 0, SHOW_UNDEF }
};

#ifdef RETAIN_INT_LATCH_COMPATIBILITY
static MYSQL_SYSVAR_BOOL( allow_create_integer_latch, g_allow_create_integer_latch, PLUGIN_VAR_RQCMDARG,
                        "Allow creation of integer latches so the upgrade logic can be tested. Not for normal use.",
                        NULL, NULL, FALSE);
#endif

static struct st_mysql_sys_var* oqgraph_sysvars[]= {
#ifdef RETAIN_INT_LATCH_COMPATIBILITY
  MYSQL_SYSVAR(allow_create_integer_latch),
#endif
  0
};

maria_declare_plugin(oqgraph)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &oqgraph_storage_engine,
  "OQGRAPH",
  "Arjen Lentz & Antony T Curtis, Open Query, and Andrew McDonnell",
  oqgraph_description,
  PLUGIN_LICENSE_GPL,
  oqgraph_init,                  /* Plugin Init                  */
  oqgraph_fini,                  /* Plugin Deinit                */
  0x0300,                        /* Version: 3s.0                */
  oqgraph_status,                /* status variables             */
  oqgraph_sysvars,               /* system variables             */
  "3.0",
  MariaDB_PLUGIN_MATURITY_GAMMA
}
maria_declare_plugin_end;
