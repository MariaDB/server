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
#include "sql_tree.h"
#include "sql_hset.h"
#include "item_func.h"
#include "key.h"
#include <math.h>

extern struct st_mysql_ftparser ft_default_parser; // XXX

constexpr double pivot= 0.0115;
static inline double lws(double tf) { return log(tf) + 1; }

ha_create_table_option fts_index_options[]=
{
  HA_IOPTION_END
};

static struct st_mysql_storage_engine fts_daemon=
{ MYSQL_DAEMON_INTERFACE_VERSION };

st_plugin_int *fts_plugin;

#define fts_deinit 0
#define fts_sys_vars 0

struct fts_hit
{
  double score;
  uchar tref[1];
};

struct fts_search_ctx
{
  TABLE *htable;
  ha_rows N;
  Hash_set<fts_hit> *hash;
  MEM_ROOT *mem_root;
  DYNAMIC_ARRAY tmp;   /* to reuse, not allocate per-word */
  size_t elem_size;
  uint ref_len;
  int err;
};

struct fts_read_ctx : public Sql_alloc
{
  DYNAMIC_ARRAY results;  /* element_size = offsetof(fts_hit, tref) + ref_len */
  size_t count;
  size_t pos;
  hli_ft_handler *fth;
};

class fts_index : public hlindex
{
  fts_read_ctx *context= nullptr;
public:
  fts_index(TABLE *t) : hlindex(t) {}
  int insert_row(TABLE *tbl, KEY *keyinfo) override;
  int read_init(TABLE *tbl, KEY *keyinfo, Item *match, ulonglong limit) override;
  int read_next(TABLE *tbl) override;
  int read_end(TABLE *tbl) override;
  int delete_row(TABLE *tbl, const uchar *rec, KEY *keyinfo) override
    { return HA_ERR_WRONG_COMMAND; }
  int delete_all(TABLE *tbl, KEY *keyinfo, bool truncate) override
    { return HA_ERR_WRONG_COMMAND; }
  bool reading() override { return context; }
};

struct fts_word
{
  const uchar *ptr;
  uint len;
};

static int cmpw(void* cs, const void* a_, const void* b_)
{
  auto a= static_cast<const fts_word*>(a_), b= static_cast<const fts_word*>(b_);
  return ha_compare_word((CHARSET_INFO*)cs, a->ptr, a->len, b->ptr, b->len);
}

typedef Tree<fts_word, CHARSET_INFO> fts_word_tree;

struct fts_collect_ctx
{
  fts_word_tree *wtree;
  MEM_ROOT *mem_root;
};

static int fts_collect_word(MYSQL_FTPARSER_PARAM *param,
                            const char *word, int word_len,
                            MYSQL_FTPARSER_BOOLEAN_INFO *)
{
  auto *ctx= static_cast<fts_collect_ctx *>(param->ftparser_state);
  fts_word w;
  w.ptr= (uchar *)memdup_root(ctx->mem_root, word, word_len); // XXX why?
  if (!w.ptr)
    return 1;
  w.len= (uint)word_len;
  return ctx->wtree->insert(w);
}

struct fts_docstat
{
  uint uniq;
  double sum;
};

struct fts_write_ctx
{
  TABLE *table;
  const uchar *tref;
  uint tref_len;
  CHARSET_INFO *cs;
  double sum;
  uint uniq;
  int err;
};

static inline bool fts_word_char(int ctype, uchar c)
{ return (ctype & (_MY_U | _MY_L | _MY_NMR)) || (c) == '_'; }

static inline bool fts_misc_word_char(uchar c)
{ return c == '\''; }

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
    if (wchars >= ft_min_word_len && wchars <= ft_max_word_len)
      if (param->mysql_add_word(param, (char *)wstart, wbytes, nullptr))
        return 1;
  }
  return 0;
}

static int fts_sum_words(const fts_word &, element_count cnt, fts_docstat *stat)
{
  stat->sum+= lws(cnt);
  stat->uniq++; // XXX Isn't it tree->element_count ?
  return 0;
}

static int fts_write_word(const fts_word &word, element_count cnt,
                          fts_write_ctx *ctx)
{
  double weight= lws(cnt) / ctx->sum * ctx->uniq / (1 + pivot * ctx->uniq);
  TABLE *t= ctx->table;
  restore_record(t, s->default_values);
  t->field[0]->store((char *)word.ptr, word.len, ctx->cs);
  t->field[1]->store_binary(ctx->tref, ctx->tref_len);
  t->field[2]->store(weight);
  return ctx->err= t->file->ha_write_row(t->record[0]);
}

int fts_index::insert_row(TABLE *tbl, KEY *keyinfo)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(tbl, &tbl->read_set);
  CHARSET_INFO *cs= keyinfo->key_part[0].field->charset();
  MEM_ROOT mem_root;
  init_alloc_root(PSI_INSTRUMENT_MEM, &mem_root, 4096, 4096, MYF(0));
  fts_word_tree wtree(cmpw, cs);
  fts_collect_ctx cctx= { &wtree, &mem_root };
  MYSQL_FTPARSER_PARAM param {};
  param.mysql_parse=    fts_mysql_parse;
  param.mysql_add_word= fts_collect_word;
  param.ftparser_state= &cctx;
  param.mode=           MYSQL_FTPARSER_SIMPLE_MODE;
  param.cs=             cs;

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
    String *val=  field->val_str(&buf);
    param.doc=    val->ptr();
    param.length= val->length();
    err= parser->parse(&param);
  }

  if (!err)
  {
    fts_docstat stat= { 0, 0.0 };
    wtree.walk(fts_sum_words, &stat);

    fts_write_ctx wctx= { table, tbl->file->ref, tbl->file->ref_length,
                          cs, stat.sum, stat.uniq, 0 };
    wtree.walk(fts_write_word, &wctx);
    err= wctx.err;
  }

  free_root(&mem_root, MYF(0));
  dbug_tmp_restore_column_map(&tbl->read_set, old_map);
  return err;
}

static int cmp_hit_score_desc(const void *a_, const void *b_)
{
  double sa= ((fts_hit*)a_)->score, sb= ((fts_hit*)b_)->score;
  return sb < sa ? -1 : sb > sa;
}

static int fts_search_word(const fts_word &word, element_count query_cnt,
                           fts_search_ctx *sctx)
{
  StringBuffer<STRING_BUFFER_USUAL_SIZE> tbuf;
  TABLE *t= sctx->htable;
  restore_record(t, s->default_values); // XXX create key directly
  t->field[0]->store((char*)word.ptr, word.len, t->field[0]->charset());
  uint keylen= t->key_info[0].key_length;
  uchar *key= (uchar*)alloca(keylen);
  key_copy(key, t->record[0], &t->key_info[0], keylen);

  int err= t->file->ha_index_read_map(t->record[0], key,
                                      HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (err == HA_ERR_KEY_NOT_FOUND)
    return 0;

  DYNAMIC_ARRAY *tmp= &sctx->tmp;
  tmp->elements= 0;

  fts_hit *scratch= (fts_hit*)alloca(sctx->elem_size);
  while (!err)
  {
    String *tref_str= t->field[1]->val_str(&tbuf);
    scratch->score= t->field[2]->val_real();
    memcpy(scratch->tref, tref_str->ptr(), sctx->ref_len);
    if (insert_dynamic(tmp, scratch))
      return sctx->err= HA_ERR_OUT_OF_MEM;
    err= t->file->ha_index_next_same(t->record[0], key, keylen);
  }
  if (err != HA_ERR_END_OF_FILE)
    return sctx->err= err;

  ha_rows N= sctx->N, df= (ha_rows)tmp->elements;
  double gw= N > df ? query_cnt * log((double)(N - df) / df) : 0;

  if (gw > 0)
  {
    for (uint i= 0; i < tmp->elements; i++)
    {
      fts_hit *src= dynamic_element(tmp, i, fts_hit*);
      fts_hit *hit= sctx->hash->find(src->tref, sctx->ref_len);
      if (hit)
        hit->score += src->score * gw;
      else
      {
        if (!(hit= (fts_hit*)alloc_root(sctx->mem_root, sctx->elem_size)))
          return sctx->err= HA_ERR_OUT_OF_MEM;
        hit->score= src->score * gw;
        memcpy(hit->tref, src->tref, sctx->ref_len);
        if (sctx->hash->insert(hit))
          return sctx->err= HA_ERR_OUT_OF_MEM;
      }
    }
  }
  return 0;
}

int fts_index::read_init(TABLE *tbl, KEY *keyinfo, Item *match, ulonglong limit)
{
  auto *ifm= static_cast<Item_func_match *>(match->real_item());
  auto *fth= static_cast<hli_ft_handler *>(ifm->fth);
  CHARSET_INFO *cs= keyinfo->key_part[0].field->charset();
  THD *thd= tbl->in_use;

  StringBuffer<STRING_BUFFER_USUAL_SIZE> qbuf;
  String *query= ifm->key_item()->val_str(&qbuf);
  if (!query || !query->length())
    return tbl->file->ha_rnd_init(0);

  uint ref_len= tbl->file->ref_length;
  size_t elem_size= ALIGN_SIZE(offsetof(fts_hit, tref) + ref_len);

  MEM_ROOT qmem, hmem;
  init_alloc_root(PSI_INSTRUMENT_MEM, &qmem, 4096, 4096, MYF(0));
  init_alloc_root(PSI_INSTRUMENT_MEM, &hmem, 4096, 4096, MYF(0));
  SCOPE_EXIT([&qmem,&hmem](){
    free_root(&qmem, MYF(0));
    free_root(&hmem, MYF(0));
   });

  fts_word_tree wtree(cmpw, cs);
  fts_collect_ctx cctx= { &wtree, &qmem };
  MYSQL_FTPARSER_PARAM param;
  param.mysql_parse=    fts_mysql_parse;
  param.mysql_add_word= fts_collect_word;
  param.ftparser_state= &cctx;
  param.mode=           MYSQL_FTPARSER_SIMPLE_MODE;
  param.cs=             cs;
  param.doc=            query->ptr();
  param.length=         query->length();
  struct st_mysql_ftparser *parser= keyinfo->parser
      ? plugin_data(keyinfo->parser, struct st_mysql_ftparser *)
      : &ft_default_parser;

  if (int err= parser->parse(&param))
    return err;

  tbl->file->info(HA_STATUS_VARIABLE);
  ha_rows N= tbl->file->stats.records;

  Hash_set<fts_hit> hits(PSI_INSTRUMENT_MEM, &my_charset_bin, 64,
                         offsetof(fts_hit, tref), ref_len,
                         nullptr, nullptr, HASH_UNIQUE);
  fts_search_ctx sctx= { table, N, &hits, &hmem, {}, elem_size, ref_len, 0 };

  if (int err= table->file->ha_index_init(0, 0))
    return err;

  if (my_init_dynamic_array(PSI_INSTRUMENT_MEM, &sctx.tmp, elem_size, 8, 8, MYF(0)))
    sctx.err= HA_ERR_OUT_OF_MEM;
  else
    wtree.walk(fts_search_word, &sctx);
  table->file->ha_index_end();
  delete_dynamic(&sctx.tmp);
  free_root(&qmem, MYF(0));
  if (sctx.err)
    return sctx.err;

  size_t ndoc= hits.size();
  if (!ndoc)
    return tbl->file->ha_rnd_init(0);

  fts_read_ctx *ctx= new (thd->mem_root) fts_read_ctx();
  ctx->fth= fth;
  if (my_init_dynamic_array(PSI_INSTRUMENT_MEM, &ctx->results, elem_size,
                            ndoc, 0, MYF(0)))
    return HA_ERR_OUT_OF_MEM;

  for (size_t i= 0; i < ndoc; i++)
    insert_dynamic(&ctx->results, hits.at(i));
  free_root(&hmem, MYF(0));

  sort_dynamic(&ctx->results, cmp_hit_score_desc); // XXX if needed
  ctx->count= ndoc;

  if (limit && static_cast<ulonglong>(ctx->count) > limit)
    ctx->count= static_cast<size_t>(limit);

  context= ctx;
  return tbl->file->ha_rnd_init(0);
}

int fts_index::read_next(TABLE *tbl)
{
  if (!context || context->pos >= context->count)
    return HA_ERR_END_OF_FILE;
  fts_hit *r= dynamic_element(&context->results, context->pos++, fts_hit*);
  context->fth->relevance= r->score;
  return tbl->file->ha_rnd_pos(tbl->record[0], r->tref);
}

int fts_index::read_end(TABLE *tbl)
{
  if (context)
  {
    delete_dynamic(&context->results);
    context= nullptr;
  }
  return tbl->file->ha_rnd_end();
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
  const char templ[]="CREATE TABLE i (                "
                     "  word varchar(84) NOT NULL,    "
                     "  tref varbinary(%u) NOT NULL,  "
                     "  weight float NOT NULL,        "
                     "  KEY (word))                   ";
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
