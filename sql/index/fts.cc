/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <my_global.h>
#include "hlindex.h"
#include "fts.h"
#include "create_options.h"
#include "ft_global.h"
#include "mysql/plugin_ftparser.h"

ha_create_table_option fts_index_options[]=
{
  HA_IOPTION_END
};

static struct st_mysql_storage_engine fts_daemon=
{ MYSQL_DAEMON_INTERFACE_VERSION };

st_plugin_int *fts_plugin;

#define fts_deinit 0
#define fts_sys_vars 0

class fts_index : public hlindex
{
public:
  fts_index(TABLE *t) : hlindex(t) {}
  int insert_row(TABLE *tbl, KEY *keyinfo) override;
  int read_init(TABLE *tbl, KEY *keyinfo, Item *dist, ulonglong limit) override { return HA_ERR_WRONG_COMMAND; }
  int read_next(TABLE *tbl) override { return HA_ERR_WRONG_COMMAND; }
  int read_end(TABLE *tbl) override { return 0; }
  int delete_row(TABLE *tbl, const uchar *rec, KEY *keyinfo) override { return HA_ERR_WRONG_COMMAND; }
  int delete_all(TABLE *tbl, KEY *keyinfo, bool truncate) override { return HA_ERR_WRONG_COMMAND; }
  bool reading() override { return false; }
};

struct fts_word_ctx
{
  TABLE *table;
  const uchar *tref;
  uint tref_len;
};

static int fts_add_word(MYSQL_FTPARSER_PARAM *param,
                        const char *word, int word_len,
                        MYSQL_FTPARSER_BOOLEAN_INFO *)
{
  auto *ctx= static_cast<fts_word_ctx *>(param->ftparser_state);
  TABLE *t= ctx->table;
  restore_record(t, s->default_values);
  t->field[0]->store(word, word_len, param->cs);
  t->field[1]->store_binary(ctx->tref, ctx->tref_len);
  return t->file->ha_write_row(t->record[0]);
}

#define fts_word_char(ctype, c) ((ctype) & (_MY_U | _MY_L | _MY_NMR) || (c) == '_')
#define fts_misc_word_char(c)   ((c) == '\'')

static int fts_mysql_parse(MYSQL_FTPARSER_PARAM *param,
                           const char *doc_arg, int doc_len)
{
  CHARSET_INFO *cs= param->cs;
  const uchar *doc= (const uchar *)doc_arg;
  const uchar *end= doc + doc_len;
  while (doc < end)
  {
    int ctype, mbl;
    for (; doc < end; doc+= (mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1)))
    {
      mbl= my_ci_ctype(cs, &ctype, doc, end);
      if (fts_word_char(ctype, *doc))
        break;
    }
    if (doc >= end)
      break;
    const uchar *wstart= doc;
    uint wchars= 0, mwc= 0;
    for (; doc < end; wchars++, doc+= (mbl > 0 ? mbl : (mbl < 0 ? -mbl : 1)))
    {
      mbl= my_ci_ctype(cs, &ctype, doc, end);
      if (fts_word_char(ctype, *doc))
        mwc= 0;
      else if (!fts_misc_word_char(*doc) || mwc)
        break;
      else
        mwc++;
    }
    uint wbytes= (uint)(doc - wstart) - mwc;
    if (wchars >= ft_min_word_len && wchars < ft_max_word_len)
      if (param->mysql_add_word(param, (char *)wstart, wbytes, nullptr))
        return 1;
  }
  return 0;
}

int fts_index::insert_row(TABLE *tbl, KEY *keyinfo)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(tbl, &tbl->read_set);
  fts_word_ctx ctx= { table, tbl->file->ref, tbl->file->ref_length };

  MYSQL_FTPARSER_PARAM param {};
  param.mysql_parse=    fts_mysql_parse;
  param.mysql_add_word= fts_add_word;
  param.ftparser_state= &ctx;
  param.mode=           MYSQL_FTPARSER_SIMPLE_MODE;

  struct st_mysql_ftparser *parser= keyinfo->parser
      ? plugin_data(keyinfo->parser, struct st_mysql_ftparser *)
      : &ft_default_parser; // XXX move ft_default_parser out of myisam

  tbl->file->position(tbl->record[0]);

  int err= 0;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> buf;
  for (uint i= 0; i < keyinfo->user_defined_key_parts && !err; i++)
  {
    Field *field= keyinfo->key_part[i].field;
    if (field->is_null())
      continue;
    String *val= field->val_str(&buf);
    param.cs=     field->charset();
    param.doc=    val->ptr();
    param.length= val->length();
    err= parser->parse(&param);
  }

  dbug_tmp_restore_column_map(&tbl->read_set, old_map);
  return err;
}

class fts_share : public hlindex_share
{
public:
  fts_share(TABLE_SHARE *s) : hlindex_share(s) {}
  hlindex *create(TABLE *tbl, MEM_ROOT *mem_root) override
  { return new (mem_root) fts_index(tbl); }
};

static const LEX_CSTRING fts_hlindex_table_def(THD *thd, uint ref_length)
{
  const char templ[]="CREATE TABLE i (              "
                     "  word varchar(84) NOT NULL,  "
                     "  tref varbinary(%u) NOT NULL,"
                     "  KEY (word))                 ";
  size_t len= sizeof(templ) + 32;
  char *s= thd->alloc(len);
  len= my_snprintf(s, len, templ, ref_length);
  return {s, len};
}

struct hlindexton fts_hton=
{
  {0, 0, 0,
  nullptr,                        /* close_connection */
  nullptr,                        /* savepoint_set */
  nullptr,
  nullptr,                        /*savepoint_rollback_can_release_mdl*/
  nullptr,                        /*savepoint_release*/
  nullptr, nullptr,
  nullptr,                        /* prepare */
  nullptr,                        /* recover */
  nullptr, nullptr,               /* commit/rollback_by_xid */
  nullptr, nullptr,               /* recover_rollback_by_xid/recovery_done */
  nullptr, nullptr, nullptr,      /* snapshot, commit/prepare_ordered */
  nullptr, nullptr},              /* checkpoint, versioned */
  fts_index_options,              /* options */
  fts_hlindex_table_def,          /* tabledef */
  [](TABLE_SHARE *s, MEM_ROOT *mem_root) -> hlindex_share* {
    return new (mem_root) fts_share(s);
  },
  nullptr                         /* uses_distance */
};

static int fts_init(void *p)
{
  fts_plugin= (st_plugin_int *)p;
  fts_plugin->data= &fts_hton;
  if (setup_transaction_participant(fts_plugin))
    return 1;
  return resolve_sysvar_table_options(fts_index_options); // XXX move it out
}

maria_declare_plugin(fts)
{
  MYSQL_DAEMON_PLUGIN,
  &fts_daemon, "fts", "MariaDB plc",
  "A plugin for fts index algorithm",
  PLUGIN_LICENSE_GPL, fts_init, fts_deinit, 0x0100, NULL,
  fts_sys_vars, "1.0", MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
