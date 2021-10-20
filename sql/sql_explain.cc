/*
   Copyright (c) 2013 Monty Program Ab

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include <my_global.h>
#include "sql_priv.h"
#include "sql_select.h"
#include "my_json_writer.h"
#include "opt_range.h"
#include "sql_expression_cache.h"

const char * STR_DELETING_ALL_ROWS= "Deleting all rows";
const char * STR_IMPOSSIBLE_WHERE= "Impossible WHERE";
const char * STR_NO_ROWS_AFTER_PRUNING= "No matching rows after partition pruning";

static void write_item(Json_writer *writer, Item *item);
static void append_item_to_str(String *out, Item *item);

Explain_query::Explain_query(THD *thd_arg, MEM_ROOT *root) : 
  mem_root(root), upd_del_plan(NULL),  insert_plan(NULL),
  unions(root), selects(root),  thd(thd_arg), apc_enabled(false),
  operations(0)
{
}

static void print_json_array(Json_writer *writer,
                             const char *title, String_list &list)
{
  List_iterator_fast<char> it(list);
  const char *name;
  writer->add_member(title).start_array();
  while ((name= it++))
    writer->add_str(name);
  writer->end_array();
}



Explain_query::~Explain_query()
{
  if (apc_enabled)
    thd->apc_target.disable();

  delete upd_del_plan;
  delete insert_plan;
  uint i;
  for (i= 0 ; i < unions.elements(); i++)
    delete unions.at(i);
  for (i= 0 ; i < selects.elements(); i++)
    delete selects.at(i);
}


Explain_node *Explain_query::get_node(uint select_id)
{
  Explain_union *u;
  if ((u= get_union(select_id)))
    return u;
  else
    return get_select(select_id);
}

Explain_union *Explain_query::get_union(uint select_id)
{
  return (unions.elements() > select_id) ? unions.at(select_id) : NULL;
}

Explain_select *Explain_query::get_select(uint select_id)
{
  return (selects.elements() > select_id) ? selects.at(select_id) : NULL;
}


void Explain_query::add_node(Explain_node *node)
{
  uint select_id;
  operations++;
  if (node->get_type() == Explain_node::EXPLAIN_UNION)
  {
    Explain_union *u= (Explain_union*)node;
    select_id= u->get_select_id();
    if (unions.elements() <= select_id)
      unions.resize(MY_MAX(select_id+1, unions.elements()*2), NULL);

    Explain_union *old_node;
    if ((old_node= get_union(select_id)))
      delete old_node;

    unions.at(select_id)= u;
  }
  else
  {
    Explain_select *sel= (Explain_select*)node;
    if (sel->select_id == FAKE_SELECT_LEX_ID)
    {
      DBUG_ASSERT(0); // this is a "fake select" from a UNION.
    }
    else
    {
      select_id= sel->select_id;
      Explain_select *old_node;

      if (selects.elements() <= select_id)
        selects.resize(MY_MAX(select_id+1, selects.elements()*2), NULL);

      if ((old_node= get_select(select_id)))
        delete old_node;

      selects.at(select_id)= sel;
    }
  }
}


void Explain_query::add_insert_plan(Explain_insert *insert_plan_arg)
{
  insert_plan= insert_plan_arg;
  query_plan_ready();
}


void Explain_query::add_upd_del_plan(Explain_update *upd_del_plan_arg)
{
  upd_del_plan= upd_del_plan_arg;
  query_plan_ready();
}


void Explain_query::query_plan_ready()
{
  if (!apc_enabled)
    thd->apc_target.enable();
  apc_enabled= true;
}

/*
  Send EXPLAIN output to the client.
*/

int Explain_query::send_explain(THD *thd)
{
  select_result *result;
  LEX *lex= thd->lex;
 
  if (!(result= new (thd->mem_root) select_send(thd)) || 
      thd->send_explain_fields(result, lex->describe, lex->analyze_stmt))
    return 1;

  int res= 0;
  if (thd->lex->explain_json)
    print_explain_json(result, thd->lex->analyze_stmt);
  else
    res= print_explain(result, lex->describe, thd->lex->analyze_stmt);

  if (res)
    result->abort_result_set();
  else
    result->send_eof();

  return res;
}


/*
  The main entry point to print EXPLAIN of the entire query
*/

int Explain_query::print_explain(select_result_sink *output, 
                                 uint8 explain_flags, bool is_analyze)
{
  if (upd_del_plan)
  {
    upd_del_plan->print_explain(this, output, explain_flags, is_analyze);
    return 0;
  }
  else if (insert_plan)
  {
    insert_plan->print_explain(this, output, explain_flags, is_analyze);
    return 0;
  }
  else
  {
    /* Start printing from node with id=1 */
    Explain_node *node= get_node(1);
    if (!node)
      return 1; /* No query plan */
    return node->print_explain(this, output, explain_flags, is_analyze);
  }
}


void Explain_query::print_explain_json(select_result_sink *output,
                                       bool is_analyze)
{
  Json_writer writer;
  writer.start_object();

  if (upd_del_plan)
    upd_del_plan->print_explain_json(this, &writer, is_analyze);
  else if (insert_plan)
    insert_plan->print_explain_json(this, &writer, is_analyze);
  else
  {
    /* Start printing from node with id=1 */
    Explain_node *node= get_node(1);
    if (!node)
      return; /* No query plan */
    node->print_explain_json(this, &writer, is_analyze);
  }

  writer.end_object();

  CHARSET_INFO *cs= system_charset_info;
  List<Item> item_list;
  String *buf= &writer.output;
  item_list.push_back(new (thd->mem_root)
                      Item_string(thd, buf->ptr(), buf->length(), cs),
                      thd->mem_root);
  output->send_data(item_list);
}


bool print_explain_for_slow_log(LEX *lex, THD *thd, String *str)
{
  return lex->explain->print_explain_str(thd, str, /*is_analyze*/ true);
}


/* 
  Return tabular EXPLAIN output as a text string
*/

bool Explain_query::print_explain_str(THD *thd, String *out_str,
                                      bool is_analyze)
{
  List<Item> fields;
  thd->make_explain_field_list(fields, thd->lex->describe, is_analyze);

  select_result_text_buffer output_buf(thd);
  output_buf.send_result_set_metadata(fields, thd->lex->describe);
  if (print_explain(&output_buf, thd->lex->describe, is_analyze))
    return true;
  output_buf.save_to(out_str);
  return false;
}


static void push_str(THD *thd, List<Item> *item_list, const char *str)
{
  item_list->push_back(new (thd->mem_root) Item_string_sys(thd, str),
                       thd->mem_root);
}


static void push_string(THD *thd, List<Item> *item_list, String *str)
{
  item_list->push_back(new (thd->mem_root)
                       Item_string_sys(thd, str->ptr(), str->length()),
                       thd->mem_root);
}

static void push_string_list(THD *thd, List<Item> *item_list,
                             String_list &lines, String *buf)
{
  List_iterator_fast<char> it(lines);
  char *line;
  bool first= true;
  while ((line= it++))
  {
    if (first)
      first= false;
    else
      buf->append(',');

    buf->append(line);
  }
  push_string(thd, item_list, buf);
}


/*
  Print an EXPLAIN output row, based on information provided in the parameters

  @note
    Parameters that may have NULL value in EXPLAIN output, should be passed
    (char*)NULL.

  @return 
    0  - OK
    1  - OOM Error
*/

static
int print_explain_row(select_result_sink *result,
                      uint8 options, bool is_analyze,
                      uint select_number,
                      const char *select_type,
                      const char *table_name,
                      const char *partitions,
                      enum join_type jtype,
                      String_list *possible_keys,
                      const char *index,
                      const char *key_len,
                      const char *ref,
                      ha_rows *rows,
                      double *r_rows,
                      double r_filtered,
                      const char *extra)
{
  THD *thd= result->thd;
  MEM_ROOT *mem_root= thd->mem_root;
  Item *item_null= new (mem_root) Item_null(thd);
  List<Item> item_list;
  Item *item;

  item_list.push_back(new (mem_root) Item_int(thd, (int32) select_number),
                      mem_root);
  item_list.push_back(new (mem_root) Item_string_sys(thd, select_type),
                      mem_root);
  item_list.push_back(new (mem_root) Item_string_sys(thd, table_name),
                      mem_root);
  if (options & DESCRIBE_PARTITIONS)
  {
    if (partitions)
    {
      item_list.push_back(new (mem_root) Item_string_sys(thd, partitions),
                          mem_root);
    }
    else
      item_list.push_back(item_null, mem_root);
  }
  
  const char *jtype_str= join_type_str[jtype];
  item_list.push_back(new (mem_root) Item_string_sys(thd, jtype_str),
                      mem_root);
  
  /* 'possible_keys'
     The buffer must not be deallocated before we call send_data, otherwise
     we may end up reading freed memory.
  */
  StringBuffer<64> possible_keys_buf;
  if (possible_keys && !possible_keys->is_empty())
  {
    push_string_list(thd, &item_list, *possible_keys, &possible_keys_buf);
  }
  else
    item_list.push_back(item_null, mem_root);
  
  /* 'index */
  item= index ? new (mem_root) Item_string_sys(thd, index) : item_null;
  item_list.push_back(item, mem_root);
  
  /* 'key_len */
  item= key_len ? new (mem_root) Item_string_sys(thd, key_len) : item_null;
  item_list.push_back(item, mem_root);
  
  /* 'ref' */
  item= ref ? new (mem_root) Item_string_sys(thd, ref) : item_null;
  item_list.push_back(item, mem_root);

  /* 'rows' */
  if (rows)
  {
    item_list.push_back(new (mem_root)
                        Item_int(thd, *rows, MY_INT64_NUM_DECIMAL_DIGITS),
                        mem_root);
  }
  else
    item_list.push_back(item_null, mem_root);
  
  /* 'r_rows' */
  if (is_analyze)
  {
    if (r_rows)
      item_list.push_back(new (mem_root) Item_float(thd, *r_rows, 2),
                          mem_root);
    else
      item_list.push_back(item_null, mem_root);
  }

  /* 'filtered' */
  const double filtered=100.0;
  if (options & DESCRIBE_EXTENDED || is_analyze)
    item_list.push_back(new (mem_root) Item_float(thd, filtered, 2), mem_root);
  
  /* 'r_filtered' */
  if (is_analyze)
    item_list.push_back(new (mem_root) Item_float(thd, r_filtered, 2),
                        mem_root);
  
  /* 'Extra' */
  if (extra)
    item_list.push_back(new (mem_root) Item_string_sys(thd, extra), mem_root);
  else
    item_list.push_back(item_null, mem_root);

  if (result->send_data(item_list))
    return 1;
  return 0;
}




uint Explain_union::make_union_table_name(char *buf)
{
  uint childno= 0;
  uint len= 6, lastop= 0;
  memcpy(buf, STRING_WITH_LEN("<union"));

  for (; childno < union_members.elements() && len + lastop + 5 < NAME_LEN;
       childno++)
  {
    len+= lastop;
    lastop= my_snprintf(buf + len, NAME_LEN - len,
                        "%u,", union_members.at(childno));
  }

  if (childno < union_members.elements() || len + lastop >= NAME_LEN)
  {
    memcpy(buf + len, STRING_WITH_LEN("...>") + 1);
    len+= 4;
  }
  else
  {
    len+= lastop;
    buf[len - 1]= '>';  // change ',' to '>'
  }
  return len;
}


int Explain_union::print_explain(Explain_query *query, 
                                 select_result_sink *output,
                                 uint8 explain_flags, 
                                 bool is_analyze)
{
  THD *thd= output->thd;
  MEM_ROOT *mem_root= thd->mem_root;
  char table_name_buffer[SAFE_NAME_LEN];

  /* print all UNION children, in order */
  for (int i= 0; i < (int) union_members.elements(); i++)
  {
    Explain_select *sel= query->get_select(union_members.at(i));
    sel->print_explain(query, output, explain_flags, is_analyze);
  }

  if (!using_tmp)
    return 0;

  /* Print a line with "UNION RESULT" */
  List<Item> item_list;
  Item *item_null= new (mem_root) Item_null(thd);

  /* `id` column */
  item_list.push_back(item_null, mem_root);

  /* `select_type` column */
  push_str(thd, &item_list, fake_select_type);

  /* `table` column: something like "<union1,2>" */
  uint len= make_union_table_name(table_name_buffer);
  item_list.push_back(new (mem_root)
                      Item_string_sys(thd, table_name_buffer, len),
                      mem_root);
  
  /* `partitions` column */
  if (explain_flags & DESCRIBE_PARTITIONS)
    item_list.push_back(item_null, mem_root);

  /* `type` column */
  push_str(thd, &item_list, join_type_str[JT_ALL]);

  /* `possible_keys` column */
  item_list.push_back(item_null, mem_root);

  /* `key` */
  item_list.push_back(item_null, mem_root);

  /* `key_len` */
  item_list.push_back(item_null, mem_root);

  /* `ref` */
  item_list.push_back(item_null, mem_root);
 
  /* `rows` */
  item_list.push_back(item_null, mem_root);
  
  /* `r_rows` */
  if (is_analyze)
  {
    double avg_rows= fake_select_lex_tracker.get_avg_rows();
    item_list.push_back(new (mem_root) Item_float(thd, avg_rows, 2), mem_root);
  }

  /* `filtered` */
  if (explain_flags & DESCRIBE_EXTENDED || is_analyze)
    item_list.push_back(item_null, mem_root);

  /* `r_filtered` */
  if (is_analyze)
    item_list.push_back(item_null, mem_root);

  /* `Extra` */
  StringBuffer<256> extra_buf;
  if (using_filesort)
  {
    extra_buf.append(STRING_WITH_LEN("Using filesort"));
  }
  item_list.push_back(new (mem_root)
                      Item_string_sys(thd, extra_buf.ptr(),
                                      extra_buf.length()),
                      mem_root);

  //output->unit.offset_limit_cnt= 0; 
  if (output->send_data(item_list))
    return 1;
  
  /*
    Print all subquery children (UNION children have already been printed at
    the start of this function)
  */
  return print_explain_for_children(query, output, explain_flags, is_analyze);
}


void Explain_union::print_explain_json(Explain_query *query, 
                                       Json_writer *writer, bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);
  char table_name_buffer[SAFE_NAME_LEN];
  
  bool started_object= print_explain_json_cache(writer, is_analyze);

  writer->add_member("query_block").start_object();
  
  if (is_recursive_cte)
    writer->add_member("recursive_union").start_object();
  else
    writer->add_member("union_result").start_object();

  // using_temporary_table
  make_union_table_name(table_name_buffer);
  writer->add_member("table_name").add_str(table_name_buffer);
  writer->add_member("access_type").add_str("ALL"); // not very useful

  /* r_loops (not present in tabular output) */
  if (is_analyze)
  {
    writer->add_member("r_loops").add_ll(fake_select_lex_tracker.get_loops());
  }

  /* `r_rows` */
  if (is_analyze)
  {
    writer->add_member("r_rows");
    if (fake_select_lex_tracker.has_scans())
      writer->add_double(fake_select_lex_tracker.get_avg_rows());
    else
      writer->add_null();
  }

  writer->add_member("query_specifications").start_array();

  for (int i= 0; i < (int) union_members.elements(); i++)
  {
    writer->start_object();
    //writer->add_member("dependent").add_str("TODO");
    //writer->add_member("cacheable").add_str("TODO");
    Explain_select *sel= query->get_select(union_members.at(i));
    sel->print_explain_json(query, writer, is_analyze);
    writer->end_object();
  }
  writer->end_array();

  print_explain_json_for_children(query, writer, is_analyze);

  writer->end_object(); // union_result
  writer->end_object(); // query_block

  if (started_object)
    writer->end_object();
}


/*
  Print EXPLAINs for all children nodes (i.e. for subqueries)
*/

int Explain_node::print_explain_for_children(Explain_query *query, 
                                             select_result_sink *output,
                                             uint8 explain_flags, 
                                             bool is_analyze)
{
  for (int i= 0; i < (int) children.elements(); i++)
  {
    Explain_node *node= query->get_node(children.at(i));
    if (node->print_explain(query, output, explain_flags, is_analyze))
      return 1;
  }
  return 0;
}

bool Explain_basic_join::add_table(Explain_table_access *tab, Explain_query *query)
{
  if (!join_tabs)
  {
    n_join_tabs= 0;
    if (!(join_tabs= ((Explain_table_access**)
                      alloc_root(query->mem_root,
                                 sizeof(Explain_table_access*) *
                                 MAX_TABLES))))
      return true;
  }
  join_tabs[n_join_tabs++]= tab;
  return false;
}

/*
  This tells whether a child subquery should be printed in JSON output.

  Derived tables and Non-merged semi-joins should not be printed, because they
  are printed inline in Explain_table_access.
*/
bool is_connection_printable_in_json(enum Explain_node::explain_connection_type type)
{
  return (type != Explain_node::EXPLAIN_NODE_DERIVED && 
          type != Explain_node::EXPLAIN_NODE_NON_MERGED_SJ);
}


void Explain_node::print_explain_json_for_children(Explain_query *query, 
                                                  Json_writer *writer,
                                                  bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);
  
  bool started= false;
  for (int i= 0; i < (int) children.elements(); i++)
  {
    Explain_node *node= query->get_node(children.at(i));
    /* Derived tables are printed inside Explain_table_access objects */
    
    if (!is_connection_printable_in_json(node->connection_type))
      continue;

    if (!started)
    {
      writer->add_member("subqueries").start_array();
      started= true;
    }

    writer->start_object();
    node->print_explain_json(query, writer, is_analyze);
    writer->end_object();
  }

  if (started)
    writer->end_array();
}


bool Explain_node::print_explain_json_cache(Json_writer *writer,
                                            bool is_analyze)
{
  if (cache_tracker)
  {
    cache_tracker->fetch_current_stats();
    writer->add_member("expression_cache").start_object();
    if (cache_tracker->state != Expression_cache_tracker::OK)
    {
      writer->add_member("state").
        add_str(Expression_cache_tracker::state_str[cache_tracker->state]);
    }

    if (is_analyze)
    {
      longlong cache_reads= cache_tracker->hit + cache_tracker->miss;
      writer->add_member("r_loops").add_ll(cache_reads);
      if (cache_reads != 0) 
      {
        double hit_ratio= double(cache_tracker->hit) / cache_reads * 100.0;
        writer->add_member("r_hit_ratio").add_double(hit_ratio);
      }
    }
    return true;
  }
  return false;
}


Explain_basic_join::~Explain_basic_join()
{
  if (join_tabs)
  {
    for (uint i= 0; i< n_join_tabs; i++)
      delete join_tabs[i];
  }
} 


int Explain_select::print_explain(Explain_query *query, 
                                  select_result_sink *output,
                                  uint8 explain_flags, bool is_analyze)
{
  THD *thd= output->thd;
  MEM_ROOT *mem_root= thd->mem_root;

  if (message)
  {
    List<Item> item_list;
    Item *item_null= new (mem_root) Item_null(thd);

    item_list.push_back(new (mem_root) Item_int(thd, (int32) select_id),
                        mem_root);
    item_list.push_back(new (mem_root) Item_string_sys(thd, select_type),
                        mem_root);
    for (uint i=0 ; i < 7; i++)
      item_list.push_back(item_null, mem_root);
    if (explain_flags & DESCRIBE_PARTITIONS)
      item_list.push_back(item_null, mem_root);

    /* filtered */
    if (is_analyze || explain_flags & DESCRIBE_EXTENDED)
      item_list.push_back(item_null, mem_root);
    
    if (is_analyze)
    {
      /* r_rows, r_filtered */
      item_list.push_back(item_null, mem_root);
      item_list.push_back(item_null, mem_root);
    }

    item_list.push_back(new (mem_root) Item_string_sys(thd, message),
                        mem_root);

    if (output->send_data(item_list))
      return 1;
  }
  else
  {
    bool using_tmp= false;
    bool using_fs= false;

    for (Explain_aggr_node *node= aggr_tree; node; node= node->child)
    {
      switch (node->get_type())
      {
        case AGGR_OP_TEMP_TABLE:
          using_tmp= true;
          break;
        case AGGR_OP_FILESORT:
          using_fs= true;
          break;
        default:
          break;
      }
    }

    for (uint i=0; i< n_join_tabs; i++)
    {
      join_tabs[i]->print_explain(output, explain_flags, is_analyze, select_id,
                                  select_type, using_tmp, using_fs);
      if (i == 0)
      {
        /* 
          "Using temporary; Using filesort" should only be shown near the 1st
          table
        */
        using_tmp= false;
        using_fs= false;
      }
    }
    for (uint i=0; i< n_join_tabs; i++)
    {
      Explain_basic_join* nest;
      if ((nest= join_tabs[i]->sjm_nest))
        nest->print_explain(query, output, explain_flags, is_analyze);
    }
  }

  return print_explain_for_children(query, output, explain_flags, is_analyze);
}


int Explain_basic_join::print_explain(Explain_query *query, 
                                      select_result_sink *output,
                                      uint8 explain_flags, bool is_analyze)
{
  for (uint i=0; i< n_join_tabs; i++)
  {
    if (join_tabs[i]->print_explain(output, explain_flags, is_analyze, 
                                    select_id,
                                    "MATERIALIZED" /*select_type*/, 
                                    FALSE /*using temporary*/, 
                                    FALSE /*using filesort*/))
      return 1;
  }
  return 0;
}


void Explain_select::print_explain_json(Explain_query *query, 
                                        Json_writer *writer, bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);
  
  bool started_cache= print_explain_json_cache(writer, is_analyze);

  if (message)
  {
    writer->add_member("query_block").start_object();
    writer->add_member("select_id").add_ll(select_id);

    writer->add_member("table").start_object();
    writer->add_member("message").add_str(message);
    writer->end_object();

    print_explain_json_for_children(query, writer, is_analyze);
    writer->end_object();
  }
  else
  {
    writer->add_member("query_block").start_object();
    writer->add_member("select_id").add_ll(select_id);

    if (is_analyze && time_tracker.get_loops())
    {
      writer->add_member("r_loops").add_ll(time_tracker.get_loops());
      writer->add_member("r_total_time_ms").add_double(time_tracker.get_time_ms());
    }

    if (exec_const_cond)
    {
      writer->add_member("const_condition");
      write_item(writer, exec_const_cond);
    }
    if (outer_ref_cond)
    {
      writer->add_member("outer_ref_condition");
      write_item(writer, outer_ref_cond);
    }
    if (pseudo_bits_cond)
    {
      writer->add_member("pseudo_bits_condition");
      write_item(writer, pseudo_bits_cond);
    }

    /* we do not print HAVING which always evaluates to TRUE */
    if (having || (having_value == Item::COND_FALSE))
    {
      writer->add_member("having_condition");
      if (likely(having))
        write_item(writer, having);
      else
      {
        /* Normally we should not go this branch, left just for safety */
        DBUG_ASSERT(having_value == Item::COND_FALSE);
        writer->add_str("0");
      }
    }

    int started_objects= 0;
    
    Explain_aggr_node *node= aggr_tree;

    for (; node; node= node->child)
    {
      switch (node->get_type())
      {
        case AGGR_OP_TEMP_TABLE:
          writer->add_member("temporary_table").start_object();
          break;
        case AGGR_OP_FILESORT:
        {
          writer->add_member("filesort").start_object();
          ((Explain_aggr_filesort*)node)->print_json_members(writer, is_analyze);
          break;
        }
        case AGGR_OP_REMOVE_DUPLICATES:
          writer->add_member("duplicate_removal").start_object();
          break;
        case AGGR_OP_WINDOW_FUNCS:
        {
          //TODO: make print_json_members virtual?
          writer->add_member("window_functions_computation").start_object();
          ((Explain_aggr_window_funcs*)node)->print_json_members(writer, is_analyze);
          break;
        }
        default:
          DBUG_ASSERT(0);
      }
      started_objects++;
    }
    
    Explain_basic_join::print_explain_json_interns(query, writer, is_analyze);

    for (;started_objects; started_objects--)
      writer->end_object();

    writer->end_object();
  }

  if (started_cache)
    writer->end_object();
}


Explain_aggr_filesort::Explain_aggr_filesort(MEM_ROOT *mem_root, 
                                             bool is_analyze,
                                             Filesort *filesort)
 : tracker(is_analyze)
{
  child= NULL;
  for (ORDER *ord= filesort->order; ord; ord= ord->next)
  {
    sort_items.push_back(ord->item[0], mem_root);
    sort_directions.push_back(&ord->direction, mem_root);
  }
  filesort->tracker= &tracker;
}


void Explain_aggr_filesort::print_json_members(Json_writer *writer, 
                                               bool is_analyze)
{
  char item_buf[256];
  String str(item_buf, sizeof(item_buf), &my_charset_bin);
  str.length(0);
  
  List_iterator_fast<Item> it(sort_items);
  List_iterator_fast<ORDER::enum_order> it_dir(sort_directions);
  Item* item;
  ORDER::enum_order *direction;
  bool first= true;
  while ((item= it++))
  {
    direction= it_dir++;
    if (first)
      first= false;
    else
    {
      str.append(", ");
    }
    append_item_to_str(&str, item);
    if (*direction == ORDER::ORDER_DESC)
      str.append(" desc");
  }

  writer->add_member("sort_key").add_str(str.c_ptr_safe());

  if (is_analyze)
    tracker.print_json_members(writer);
}


void Explain_aggr_window_funcs::print_json_members(Json_writer *writer, 
                                                   bool is_analyze)
{
  Explain_aggr_filesort *srt;
  List_iterator<Explain_aggr_filesort> it(sorts);
  writer->add_member("sorts").start_object();
  while ((srt= it++))
  {
    writer->add_member("filesort").start_object();
    srt->print_json_members(writer, is_analyze);
    writer->end_object(); // filesort
  }
  writer->end_object(); // sorts
}


void Explain_basic_join::print_explain_json(Explain_query *query, 
                                            Json_writer *writer, 
                                            bool is_analyze)
{
  writer->add_member("query_block").start_object();
  writer->add_member("select_id").add_ll(select_id);
  
  print_explain_json_interns(query, writer, is_analyze);

  writer->end_object();
}


void Explain_basic_join::
print_explain_json_interns(Explain_query *query, 
                           Json_writer *writer, 
                           bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);
  for (uint i=0; i< n_join_tabs; i++)
  {
    if (join_tabs[i]->start_dups_weedout)
      writer->add_member("duplicates_removal").start_object();

    join_tabs[i]->print_explain_json(query, writer, is_analyze);

    if (join_tabs[i]->end_dups_weedout)
      writer->end_object();
  }
  print_explain_json_for_children(query, writer, is_analyze);
}


void Explain_table_access::push_extra(enum explain_extra_tag extra_tag)
{
  extra_tags.append(extra_tag);
}


/*
  Put the contents of 'key' field of EXPLAIN otuput into key_str.

  It is surprisingly complex:
  - hash join shows #hash#used_key
  - quick selects that use single index will print index name
*/

void Explain_table_access::fill_key_str(String *key_str, bool is_json) const
{
  CHARSET_INFO *cs= system_charset_info;
  bool is_hj= (type == JT_HASH || type == JT_HASH_NEXT || 
               type == JT_HASH_RANGE || type == JT_HASH_INDEX_MERGE);
  const char *hash_key_prefix= "#hash#";

  if (key.get_key_name())
  {
    if (is_hj)
      key_str->append(hash_key_prefix, strlen(hash_key_prefix), cs);

    key_str->append(key.get_key_name());

    if (is_hj && type != JT_HASH)
      key_str->append(':');
  }
  
  if (quick_info)
  {
    StringBuffer<64> buf2;
    if (is_json)
      quick_info->print_extra_recursive(&buf2);
    else
      quick_info->print_key(&buf2);
    key_str->append(buf2);
  }
  if (type == JT_HASH_NEXT)
    key_str->append(hash_next_key.get_key_name());
}


/*
  Fill "key_length".
   - this is just used key length for ref/range 
   - for index_merge, it is a comma-separated list of lengths.
   - for hash join, it is key_len:pseudo_key_len

  The column looks identical in tabular and json forms. In JSON, we consider
  the column legacy, it is superceded by used_key_parts.
*/

void Explain_table_access::fill_key_len_str(String *key_len_str) const
{
  bool is_hj= (type == JT_HASH || type == JT_HASH_NEXT || 
               type == JT_HASH_RANGE || type == JT_HASH_INDEX_MERGE);
  if (key.get_key_len() != (uint)-1)
  {
    char buf[64];
    size_t length;
    length= longlong10_to_str(key.get_key_len(), buf, 10) - buf;
    key_len_str->append(buf, length);
    if (is_hj && type != JT_HASH)
      key_len_str->append(':');
  }

  if (quick_info)
  {
    StringBuffer<64> buf2;
    quick_info->print_key_len(&buf2);
    key_len_str->append(buf2);
  } 

  if (type == JT_HASH_NEXT)
  {
    char buf[64];
    size_t length;
    length= longlong10_to_str(hash_next_key.get_key_len(), buf, 10) - buf;
    key_len_str->append(buf, length);
  }
}


void Explain_index_use::set(MEM_ROOT *mem_root, KEY *key, uint key_len_arg)
{
  set_pseudo_key(mem_root, key->name);
  key_len= key_len_arg;
  uint len= 0;
  for (uint i= 0; i < key->usable_key_parts; i++)
  {
    key_parts_list.append_str(mem_root, key->key_part[i].field->field_name);
    len += key->key_part[i].store_length;
    if (len >= key_len_arg)
      break;
  }
}


void Explain_index_use::set_pseudo_key(MEM_ROOT *root, const char* key_name_arg)
{
  if (key_name_arg)
  {
    size_t name_len= strlen(key_name_arg);
    if ((key_name= (char*)alloc_root(root, name_len+1)))
      memcpy(key_name, key_name_arg, name_len+1);
  }
  else
    key_name= NULL;
  key_len= ~(uint) 0;
}


/*
  Given r_filtered% from join buffer condition and join condition, produce a
  combined r_filtered% number. This is needed for tabular EXPLAIN output which
  has only one cell for r_filtered value.
*/

double Explain_table_access::get_r_filtered()
{
  double r_filtered= tracker.get_filtered_after_where();
  if (bka_type.is_using_jbuf())
    r_filtered *= jbuf_tracker.get_filtered_after_where();
  return r_filtered;
}


int Explain_table_access::print_explain(select_result_sink *output, uint8 explain_flags, 
                                        bool is_analyze,
                                        uint select_id, const char *select_type,
                                        bool using_temporary, bool using_filesort)
{
  THD *thd= output->thd;
  MEM_ROOT *mem_root= thd->mem_root;

  List<Item> item_list;
  Item *item_null= new (mem_root) Item_null(thd);
  
  /* `id` column */
  item_list.push_back(new (mem_root) Item_int(thd, (int32) select_id),
                      mem_root);

  /* `select_type` column */
  push_str(thd, &item_list, select_type);

  /* `table` column */
  push_string(thd, &item_list, &table_name);
  
  /* `partitions` column */
  if (explain_flags & DESCRIBE_PARTITIONS)
  {
    if (used_partitions_set)
    {
      push_string(thd, &item_list, &used_partitions);
    }
    else
      item_list.push_back(item_null, mem_root);
  }

  /* `type` column */
  push_str(thd, &item_list, join_type_str[type]);

  /* `possible_keys` column */
  StringBuffer<64> possible_keys_buf;
  if (possible_keys.is_empty())
    item_list.push_back(item_null, mem_root);
  else
    push_string_list(thd, &item_list, possible_keys, &possible_keys_buf);

  /* `key` */
  StringBuffer<64> key_str;
  fill_key_str(&key_str, false);
  
  if (key_str.length() > 0)
    push_string(thd, &item_list, &key_str);
  else
    item_list.push_back(item_null, mem_root);

  /* `key_len` */
  StringBuffer<64> key_len_str;
  fill_key_len_str(&key_len_str);

  if (key_len_str.length() > 0)
    push_string(thd, &item_list, &key_len_str);
  else
    item_list.push_back(item_null, mem_root);

  /* `ref` */
  StringBuffer<64> ref_list_buf;
  if (ref_list.is_empty())
  {
    if (type == JT_FT)
    {
      /* Traditionally, EXPLAIN lines with type=fulltext have ref='' */
      push_str(thd, &item_list, "");
    }
    else
      item_list.push_back(item_null, mem_root);
  }
  else
    push_string_list(thd, &item_list, ref_list, &ref_list_buf);
 
  /* `rows` */
  if (rows_set)
  {
    item_list.push_back(new (mem_root)
                        Item_int(thd, (ulonglong) rows,
                                 MY_INT64_NUM_DECIMAL_DIGITS),
                        mem_root);
  }
  else
    item_list.push_back(item_null, mem_root);

  /* `r_rows` */
  if (is_analyze)
  {
    if (!tracker.has_scans())
    {
      item_list.push_back(item_null, mem_root);
    }
    else
    {
      double avg_rows= tracker.get_avg_rows();
      item_list.push_back(new (mem_root) Item_float(thd, avg_rows, 2),
                          mem_root);
    }
  }

  /* `filtered` */
  if (explain_flags & DESCRIBE_EXTENDED || is_analyze)
  {
    if (filtered_set)
    {
      item_list.push_back(new (mem_root) Item_float(thd, filtered, 2),
                          mem_root);
    }
    else
      item_list.push_back(item_null, mem_root);
  }

  /* `r_filtered` */
  if (is_analyze)
  {
    if (!tracker.has_scans())
    {
      item_list.push_back(item_null, mem_root);
    }
    else
    {
      double r_filtered= tracker.get_filtered_after_where();
      if (bka_type.is_using_jbuf())
        r_filtered *= jbuf_tracker.get_filtered_after_where();
      item_list.push_back(new (mem_root)
                          Item_float(thd, r_filtered * 100.0, 2),
                          mem_root);
    }
  }

  /* `Extra` */
  StringBuffer<256> extra_buf;
  bool first= true;
  for (int i=0; i < (int)extra_tags.elements(); i++)
  {
    if (first)
      first= false;
    else
      extra_buf.append(STRING_WITH_LEN("; "));
    append_tag_name(&extra_buf, extra_tags.at(i));
  }

  if (using_temporary)
  {
    if (first)
      first= false;
    else
      extra_buf.append(STRING_WITH_LEN("; "));
    extra_buf.append(STRING_WITH_LEN("Using temporary"));
  }

  if (using_filesort || this->pre_join_sort)
  {
    if (first)
      first= false;
    else
      extra_buf.append(STRING_WITH_LEN("; "));
    extra_buf.append(STRING_WITH_LEN("Using filesort"));
  }

  item_list.push_back(new (mem_root)
                      Item_string_sys(thd, extra_buf.ptr(),
                                      extra_buf.length()),
                      mem_root);

  if (output->send_data(item_list))
    return 1;

  return 0;
}


/**
  Adds copy of the string to the list

  @param mem_root        where to allocate string
  @param str             string to copy and add

  @return
    NULL - out of memory error
    poiner on allocated copy of the string
*/

const char *String_list::append_str(MEM_ROOT *mem_root, const char *str)
{
  size_t len= strlen(str);
  char *cp;
  if (!(cp = (char*)alloc_root(mem_root, len+1)))
    return NULL;
  memcpy(cp, str, len+1);
  push_back(cp, mem_root);
  return cp;
}


static void write_item(Json_writer *writer, Item *item)
{
  THD *thd= current_thd;
  char item_buf[256];
  String str(item_buf, sizeof(item_buf), &my_charset_bin);
  str.length(0);

  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

  item->print(&str, QT_EXPLAIN);

  thd->variables.option_bits= save_option_bits;
  writer->add_str(str.c_ptr_safe());
}

static void append_item_to_str(String *out, Item *item)
{
  THD *thd= current_thd;
  ulonglong save_option_bits= thd->variables.option_bits;
  thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

  item->print(out, QT_EXPLAIN);
  thd->variables.option_bits= save_option_bits;
}

void Explain_table_access::tag_to_json(Json_writer *writer, enum explain_extra_tag tag)
{
  switch (tag)
  {
    case ET_OPEN_FULL_TABLE:
      writer->add_member("open_full_table").add_bool(true);
      break;
    case ET_SCANNED_0_DATABASES:
      writer->add_member("scanned_databases").add_ll(0);
      break;
    case ET_SCANNED_1_DATABASE:
      writer->add_member("scanned_databases").add_ll(1);
      break;
    case ET_SCANNED_ALL_DATABASES:
      writer->add_member("scanned_databases").add_str("all");
      break;
    case ET_SKIP_OPEN_TABLE:
      writer->add_member("skip_open_table").add_bool(true);
      break;
    case ET_OPEN_FRM_ONLY:
      writer->add_member("open_frm_only").add_bool(true);
      break;
    case ET_USING_INDEX_CONDITION:
      writer->add_member("index_condition");
      write_item(writer, pushed_index_cond);
      break;
    case ET_USING_INDEX_CONDITION_BKA:
      writer->add_member("index_condition_bka");
      write_item(writer, pushed_index_cond);
      break;
    case ET_USING_WHERE:
      {
        /*
          We are printing the condition that is checked when scanning this
          table.
          - when join buffer is used, it is cache_cond. 
          - in other cases, it is where_cond.
        */
        Item *item= bka_type.is_using_jbuf()? cache_cond: where_cond;
        if (item)
        {
          writer->add_member("attached_condition");
          write_item(writer, item);
        }
      }
      break;
    case ET_USING_INDEX:
      writer->add_member("using_index").add_bool(true);
      break;
    case ET_USING:
      // index merge: case ET_USING 
      break;
    case ET_RANGE_CHECKED_FOR_EACH_RECORD:
      /* Handled as range_checked_fer */
    case ET_USING_JOIN_BUFFER:
      /* Do nothing. Join buffer is handled differently */
    case ET_START_TEMPORARY:
    case ET_END_TEMPORARY:
      /* Handled as "duplicates_removal: { ... } */
    case ET_FULL_SCAN_ON_NULL_KEY:
      /* Handled in full_scan_on_null_key */
      break;
    case ET_FIRST_MATCH:
      writer->add_member("first_match").add_str(firstmatch_table_name.c_ptr());
      break;
    case ET_LOOSESCAN:
      writer->add_member("loose_scan").add_bool(true);
      break;
    case ET_USING_MRR:
      writer->add_member("mrr_type").add_str(mrr_type.c_ptr());
      break;
    case ET_USING_INDEX_FOR_GROUP_BY:
      writer->add_member("using_index_for_group_by");
      if (loose_scan_is_scanning)
        writer->add_str("scanning");
      else
        writer->add_bool(true);
      break;

    /*new:*/
    case ET_CONST_ROW_NOT_FOUND:
      writer->add_member("const_row_not_found").add_bool(true);
      break;
    case ET_UNIQUE_ROW_NOT_FOUND:
      /* 
        Currently, we never get here.  All SELECTs that have 
        ET_UNIQUE_ROW_NOT_FOUND for a table are converted into degenerate
        SELECTs with message="Impossible WHERE ...". 
        MySQL 5.6 has the same property.
        I'm leaving the handling in just for the sake of covering all enum
        members and safety.
      */
      writer->add_member("unique_row_not_found").add_bool(true);
      break;
    case ET_IMPOSSIBLE_ON_CONDITION:
      writer->add_member("impossible_on_condition").add_bool(true);
      break;
    case ET_USING_WHERE_WITH_PUSHED_CONDITION:
      /*
        It would be nice to print the pushed condition, but current Storage
        Engine API doesn't provide any way to do that
      */
      writer->add_member("pushed_condition").add_bool(true);
      break;

    case ET_NOT_EXISTS:
      writer->add_member("not_exists").add_bool(true);
      break;
    case ET_DISTINCT:
      writer->add_member("distinct").add_bool(true);
      break;

    default:
      DBUG_ASSERT(0);
  }
}


static
void add_json_keyset(Json_writer *writer, const char *elem_name,
                     String_list *keyset)
{
  if (!keyset->is_empty())
    print_json_array(writer, elem_name, *keyset);
}


void Explain_table_access::print_explain_json(Explain_query *query,
                                              Json_writer *writer,
                                              bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);
  
  if (pre_join_sort)
  {
    /* filesort was invoked on this join tab before doing the join with the rest */
    writer->add_member("read_sorted_file").start_object();
    if (is_analyze)
    {
      writer->add_member("r_rows");
      /*
        r_rows when reading filesort result. This can be less than the number
        of rows produced by filesort due to NL-join having LIMIT.
      */
      if (tracker.has_scans())
        writer->add_double(tracker.get_avg_rows());
      else
        writer->add_null();

      /* 
        r_filtered when reading filesort result. We should have checked the
        WHERE while doing filesort but lets check just in case.
      */
      if (tracker.has_scans() && tracker.get_filtered_after_where() < 1.0)
      {
        writer->add_member("r_filtered");
        writer->add_double(tracker.get_filtered_after_where()*100.0);
      }
    }
    writer->add_member("filesort").start_object();
    pre_join_sort->print_json_members(writer, is_analyze);
  }

  if (bka_type.is_using_jbuf())
  {
    writer->add_member("block-nl-join").start_object();
  }

  if (range_checked_fer)
  {
    range_checked_fer->print_json(writer, is_analyze);
  }

  if (full_scan_on_null_key)
    writer->add_member("full-scan-on-null_key").start_object();

  writer->add_member("table").start_object();

  writer->add_member("table_name").add_str(table_name);

  if (used_partitions_set)
    print_json_array(writer, "partitions", used_partitions_list);

  writer->add_member("access_type").add_str(join_type_str[type]);

  add_json_keyset(writer, "possible_keys", &possible_keys);

  /* `key` */
  /* For non-basic quick select, 'key' will not be present */
  if (!quick_info || quick_info->is_basic())
  {
    StringBuffer<64> key_str;
    fill_key_str(&key_str, true);
    if (key_str.length())
      writer->add_member("key").add_str(key_str);
  }

  /* `key_length` */
  StringBuffer<64> key_len_str;
  fill_key_len_str(&key_len_str);
  if (key_len_str.length())
    writer->add_member("key_length").add_str(key_len_str);

  /* `used_key_parts` */
  String_list *parts_list= NULL;
  if (quick_info && quick_info->is_basic())
    parts_list= &quick_info->range.key_parts_list;
  else
    parts_list= &key.key_parts_list;

  if (parts_list && !parts_list->is_empty())
    print_json_array(writer, "used_key_parts", *parts_list);

  if (quick_info && !quick_info->is_basic())
  {
    writer->add_member("index_merge").start_object();
    quick_info->print_json(writer);
    writer->end_object();
  }
  
  /* `ref` */
  if (!ref_list.is_empty())
    print_json_array(writer, "ref", ref_list);

  /* r_loops (not present in tabular output) */
  if (is_analyze)
  {
    writer->add_member("r_loops").add_ll(tracker.get_loops());
  }
  
  /* `rows` */
  if (rows_set)
    writer->add_member("rows").add_ull(rows);

  /* `r_rows` */
  if (is_analyze)
  {
    writer->add_member("r_rows");
    if (pre_join_sort)
    {
      /* Get r_rows value from filesort */
      if (pre_join_sort->tracker.get_r_loops())
        writer->add_double(pre_join_sort->tracker.get_avg_examined_rows());
      else
        writer->add_null();
    }
    else
    {
      if (tracker.has_scans())
        writer->add_double(tracker.get_avg_rows());
      else
        writer->add_null();
    }

    if (op_tracker.get_loops())
    {
      writer->add_member("r_total_time_ms").
              add_double(op_tracker.get_time_ms());
    }
  }
  
  /* `filtered` */
  if (filtered_set)
    writer->add_member("filtered").add_double(filtered);

  /* `r_filtered` */
  if (is_analyze)
  {
    writer->add_member("r_filtered");
    if (pre_join_sort)
    {
      /* Get r_filtered value from filesort */
      if (pre_join_sort->tracker.get_r_loops())
        writer->add_double(pre_join_sort->tracker.get_r_filtered()*100);
      else
        writer->add_null();
    }
    else
    {
      /* Get r_filtered from the NL-join runtime */
      if (tracker.has_scans())
        writer->add_double(tracker.get_filtered_after_where()*100.0);
      else
        writer->add_null();
    }
  }

  for (int i=0; i < (int)extra_tags.elements(); i++)
  {
    tag_to_json(writer, extra_tags.at(i));
  }
  
  if (full_scan_on_null_key)
    writer->end_object(); //"full-scan-on-null_key"

  if (range_checked_fer)
    writer->end_object(); // "range-checked-for-each-record"

  if (bka_type.is_using_jbuf())
  {
    writer->end_object(); // "block-nl-join"
    writer->add_member("buffer_type").add_str(bka_type.incremental?
                                              "incremental":"flat");
    writer->add_member("buffer_size").add_size(bka_type.join_buffer_size);
    writer->add_member("join_type").add_str(bka_type.join_alg);
    if (bka_type.mrr_type.length())
      writer->add_member("mrr_type").add_str(bka_type.mrr_type);
    if (where_cond)
    {
      writer->add_member("attached_condition");
      write_item(writer, where_cond);
    }

    if (is_analyze)
    {
      //writer->add_member("r_loops").add_ll(jbuf_tracker.get_loops());
      writer->add_member("r_filtered");
      if (jbuf_tracker.has_scans())
        writer->add_double(jbuf_tracker.get_filtered_after_where()*100.0);
      else
        writer->add_null();
    }
  }

  if (derived_select_number)
  {
    /* This is a derived table. Print its contents here */
    writer->add_member("materialized").start_object();
    Explain_node *node= query->get_node(derived_select_number);
    node->print_explain_json(query, writer, is_analyze);
    writer->end_object();
  }
  if (non_merged_sjm_number)
  {
    /* This is a non-merged semi-join table. Print its contents here */
    writer->add_member("materialized").start_object();
    writer->add_member("unique").add_ll(1);
    Explain_node *node= query->get_node(non_merged_sjm_number);
    node->connection_type= Explain_node::EXPLAIN_NODE_NON_MERGED_SJ;
    node->print_explain_json(query, writer, is_analyze);
    writer->end_object();
  }
  if (sjm_nest)
  {
    /* This is a non-merged semi-join table. Print its contents here */
    writer->add_member("materialized").start_object();
    writer->add_member("unique").add_ll(1);
    sjm_nest->print_explain_json(query, writer, is_analyze);
    writer->end_object();
  }

  if (pre_join_sort)
  {
    writer->end_object(); // filesort
    writer->end_object(); // read_sorted_file
  }

  writer->end_object();
}


/*
  Elements in this array match members of enum explain_extra_tag, defined in
  sql_explain.h
*/

const char * extra_tag_text[]=
{
  "ET_none",
  "Using index condition",
  "Using index condition(BKA)",
  "Using ", // special handling
  "Range checked for each record (index map: 0x", // special handling
  "Using where with pushed condition",
  "Using where",
  "Not exists",
  
  "Using index",
  "Full scan on NULL key",
  "Skip_open_table",
  "Open_frm_only",
  "Open_full_table", 

  "Scanned 0 databases",
  "Scanned 1 database",
  "Scanned all databases",

  "Using index for group-by", // special handling

  "USING MRR: DONT PRINT ME", // special handling

  "Distinct",
  "LooseScan",
  "Start temporary",
  "End temporary",
  "FirstMatch", // special handling

  "Using join buffer", // special handling 

  "const row not found",
  "unique row not found",
  "Impossible ON condition"
};


void Explain_table_access::append_tag_name(String *str, enum explain_extra_tag tag)
{
  switch (tag) {
    case ET_USING:
    {
      // quick select
      str->append(STRING_WITH_LEN("Using "));
      quick_info->print_extra(str);
      break;
    }
    case ET_RANGE_CHECKED_FOR_EACH_RECORD:
    {
      /* 4 bits per 1 hex digit + terminating '\0' */
      char buf[MAX_KEY / 4 + 1];
      str->append(STRING_WITH_LEN("Range checked for each "
                                   "record (index map: 0x"));
      str->append(range_checked_fer->keys_map.print(buf));
      str->append(')');
      break;
    }
    case ET_USING_MRR:
    {
      str->append(mrr_type);
      break;
    }
    case ET_USING_JOIN_BUFFER:
    {
      str->append(extra_tag_text[tag]);

      str->append(STRING_WITH_LEN(" ("));
      const char *buffer_type= bka_type.incremental ? "incremental" : "flat";
      str->append(buffer_type);
      str->append(STRING_WITH_LEN(", "));
      str->append(bka_type.join_alg);
      str->append(STRING_WITH_LEN(" join"));
      str->append(STRING_WITH_LEN(")"));
      if (bka_type.mrr_type.length())
      {
        str->append(STRING_WITH_LEN("; "));
        str->append(bka_type.mrr_type);
      }

      break;
    }
    case ET_FIRST_MATCH:
    {
      if (firstmatch_table_name.length())
      {
        str->append("FirstMatch(");
        str->append(firstmatch_table_name);
        str->append(")");
      }
      else
        str->append(extra_tag_text[tag]);
      break;
    }
    case ET_USING_INDEX_FOR_GROUP_BY:
    {
      str->append(extra_tag_text[tag]);
      if (loose_scan_is_scanning)
        str->append(" (scanning)");
      break;
    }
    default:
     str->append(extra_tag_text[tag]);
  }
}


/* 
  This is called for top-level Explain_quick_select only. The point of this
  function is:
  - index_merge should print $index_merge_type (child, ...)
  - 'range'  should not print anything.
*/

void Explain_quick_select::print_extra(String *str)
{
  if (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE || 
      quick_type == QUICK_SELECT_I::QS_TYPE_RANGE_DESC ||
      quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX)
  {
    /* print nothing */
  }
  else
    print_extra_recursive(str);
}

void Explain_quick_select::print_json(Json_writer *writer)
{
  if (is_basic())
  {
    writer->add_member("range").start_object();

    writer->add_member("key").add_str(range.get_key_name());

    print_json_array(writer, "used_key_parts", range.key_parts_list);

    writer->end_object();
  }
  else
  {
    writer->add_member(get_name_by_type()).start_object();

    List_iterator_fast<Explain_quick_select> it (children);
    Explain_quick_select* child;
    while ((child = it++))
      child->print_json(writer);

    writer->end_object();
  }
}

void Explain_quick_select::print_extra_recursive(String *str)
{
  if (is_basic())
  {
    str->append(range.get_key_name());
  }
  else
  {
    str->append(get_name_by_type());
    str->append('(');
    List_iterator_fast<Explain_quick_select> it (children);
    Explain_quick_select* child;
    bool first= true;
    while ((child = it++))
    {
      if (first)
        first= false;
      else
        str->append(',');

      child->print_extra_recursive(str);
    }
    str->append(')');
  }
}


const char * Explain_quick_select::get_name_by_type()
{
  switch (quick_type) {
    case QUICK_SELECT_I::QS_TYPE_INDEX_MERGE:
      return "sort_union";
    case QUICK_SELECT_I::QS_TYPE_ROR_UNION:
      return "union";
    case QUICK_SELECT_I::QS_TYPE_ROR_INTERSECT:
      return "intersect";
    case QUICK_SELECT_I::QS_TYPE_INDEX_INTERSECT:
      return "sort_intersect";
    default:
      DBUG_ASSERT(0);
      return "unknown quick select type";
  }
}


/*
  This prints a comma-separated list of used indexes, ignoring nesting
*/

void Explain_quick_select::print_key(String *str)
{
  if (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE || 
      quick_type == QUICK_SELECT_I::QS_TYPE_RANGE_DESC || 
      quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX)
  {
    if (str->length() > 0)
      str->append(',');
    str->append(range.get_key_name());
  }
  else
  {
    List_iterator_fast<Explain_quick_select> it (children);
    Explain_quick_select* child;
    while ((child = it++))
    {
      child->print_key(str);
    }
  }
}


/*
  This prints a comma-separated list of used key_lengths, ignoring nesting
*/

void Explain_quick_select::print_key_len(String *str)
{
  if (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE || 
      quick_type == QUICK_SELECT_I::QS_TYPE_RANGE_DESC ||
      quick_type == QUICK_SELECT_I::QS_TYPE_GROUP_MIN_MAX)
  {
    char buf[64];
    size_t length;
    length= longlong10_to_str(range.get_key_len(), buf, 10) - buf;
    if (str->length() > 0)
      str->append(',');
    str->append(buf, length);
  }
  else
  {
    List_iterator_fast<Explain_quick_select> it (children);
    Explain_quick_select* child;
    while ((child = it++))
    {
      child->print_key_len(str);
    }
  }
}


int Explain_delete::print_explain(Explain_query *query, 
                                  select_result_sink *output,
                                  uint8 explain_flags,
                                  bool is_analyze)
{
  if (deleting_all_rows)
  {
    const char *msg= STR_DELETING_ALL_ROWS;
    int res= print_explain_message_line(output, explain_flags, is_analyze,
                                        1 /*select number*/,
                                        select_type, &rows, msg);
    return res;

  }
  else
  {
    return Explain_update::print_explain(query, output, explain_flags,
                                         is_analyze);
  }
}


void Explain_delete::print_explain_json(Explain_query *query, 
                                        Json_writer *writer,
                                        bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);

  if (deleting_all_rows)
  {
    writer->add_member("query_block").start_object();
    writer->add_member("select_id").add_ll(1);
    writer->add_member("table").start_object();
    // just like mysql-5.6, we don't print table name. Is this ok?
    writer->add_member("message").add_str(STR_DELETING_ALL_ROWS);
    writer->end_object(); // table
    writer->end_object(); // query_block
    return;
  }
  Explain_update::print_explain_json(query, writer, is_analyze);
}


int Explain_update::print_explain(Explain_query *query, 
                                  select_result_sink *output,
                                  uint8 explain_flags,
                                  bool is_analyze)
{
  StringBuffer<64> key_buf;
  StringBuffer<64> key_len_buf;
  StringBuffer<64> extra_str;
  if (impossible_where || no_partitions)
  {
    const char *msg= impossible_where ? 
                     STR_IMPOSSIBLE_WHERE : 
                     STR_NO_ROWS_AFTER_PRUNING;
    int res= print_explain_message_line(output, explain_flags, is_analyze,
                                        1 /*select number*/,
                                        select_type, 
                                        NULL, /* rows */
                                        msg);
    return res;
  }

  if (quick_info)
  {
    quick_info->print_key(&key_buf);
    quick_info->print_key_len(&key_len_buf);

    StringBuffer<64> quick_buf;
    quick_info->print_extra(&quick_buf);
    if (quick_buf.length())
    {
      extra_str.append(STRING_WITH_LEN("Using "));
      extra_str.append(quick_buf);
    }
  }
  else if (key.get_key_name())
  {
    const char *name= key.get_key_name();
    key_buf.set(name, strlen(name), &my_charset_bin);
    char buf[64];
    size_t length= longlong10_to_str(key.get_key_len(), buf, 10) - buf;
    key_len_buf.copy(buf, length, &my_charset_bin);
  }

  if (using_where)
  {
    if (extra_str.length() !=0)
      extra_str.append(STRING_WITH_LEN("; "));
    extra_str.append(STRING_WITH_LEN("Using where"));
  }

  if (mrr_type.length() != 0)
  {
    if (extra_str.length() !=0)
      extra_str.append(STRING_WITH_LEN("; "));
    extra_str.append(mrr_type);
  }
  
  if (is_using_filesort())
  {
    if (extra_str.length() !=0)
      extra_str.append(STRING_WITH_LEN("; "));
    extra_str.append(STRING_WITH_LEN("Using filesort"));
  }

  if (using_io_buffer)
  {
    if (extra_str.length() !=0)
      extra_str.append(STRING_WITH_LEN("; "));
    extra_str.append(STRING_WITH_LEN("Using buffer"));
  }

  /* 
    Single-table DELETE commands do not do "Using temporary".
    "Using index condition" is also not possible (which is an unjustified limitation)
  */
  double r_filtered= 100 * tracker.get_filtered_after_where();
  double r_rows= tracker.get_avg_rows();

  print_explain_row(output, explain_flags, is_analyze,
                    1, /* id */
                    select_type,
                    table_name.c_ptr(), 
                    used_partitions_set? used_partitions.c_ptr() : NULL,
                    jtype,
                    &possible_keys,
                    key_buf.length()? key_buf.c_ptr() : NULL,
                    key_len_buf.length() ? key_len_buf.c_ptr() : NULL,
                    NULL, /* 'ref' is always NULL in single-table EXPLAIN DELETE */
                    &rows,
                    tracker.has_scans()? &r_rows : NULL,
                    r_filtered,
                    extra_str.c_ptr_safe());

  return print_explain_for_children(query, output, explain_flags, is_analyze);
}


void Explain_update::print_explain_json(Explain_query *query,
                                        Json_writer *writer,
                                        bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);

  writer->add_member("query_block").start_object();
  writer->add_member("select_id").add_ll(1);
 
  /* This is the total time it took to do the UPDATE/DELETE */
  if (is_analyze && command_tracker.get_loops())
  {
    writer->add_member("r_total_time_ms").
            add_double(command_tracker.get_time_ms());
  }
  
  if (impossible_where || no_partitions)
  {
    const char *msg= impossible_where ?  STR_IMPOSSIBLE_WHERE : 
                                         STR_NO_ROWS_AFTER_PRUNING;
    writer->add_member("table").start_object();
    writer->add_member("message").add_str(msg);
    writer->end_object(); // table
    writer->end_object(); // query_block
    return;
  }

  DBUG_ASSERT(!(is_using_filesort() && using_io_buffer));
  
  bool doing_buffering= false;

  if (is_using_filesort())
  {
    writer->add_member("filesort").start_object();
    if (is_analyze)
      filesort_tracker->print_json_members(writer);
    doing_buffering= true;
  }

  if (using_io_buffer)
  {
    writer->add_member("buffer").start_object();
    doing_buffering= true;
  }

  /* Produce elements that are common for buffered and un-buffered cases */
  writer->add_member("table").start_object();

  if (get_type() == EXPLAIN_UPDATE)
    writer->add_member("update").add_ll(1);
  else
    writer->add_member("delete").add_ll(1);

  writer->add_member("table_name").add_str(table_name);

  if (used_partitions_set)
    print_json_array(writer, "partitions", used_partitions_list);

  writer->add_member("access_type").add_str(join_type_str[jtype]);

  if (!possible_keys.is_empty())
  {
    List_iterator_fast<char> it(possible_keys);
    const char *name;
    writer->add_member("possible_keys").start_array();
    while ((name= it++))
      writer->add_str(name);
    writer->end_array();
  }

  /* `key`, `key_length` */
  if (quick_info && quick_info->is_basic())
  {
    StringBuffer<64> key_buf;
    StringBuffer<64> key_len_buf;
    quick_info->print_extra_recursive(&key_buf);
    quick_info->print_key_len(&key_len_buf);
    
    writer->add_member("key").add_str(key_buf);
    writer->add_member("key_length").add_str(key_len_buf);
  }
  else if (key.get_key_name())
  {
    writer->add_member("key").add_str(key.get_key_name());
    writer->add_member("key_length").add_str(key.get_key_len());
  }

  /* `used_key_parts` */
  String_list *parts_list= NULL;
  if (quick_info && quick_info->is_basic())
    parts_list= &quick_info->range.key_parts_list;
  else
    parts_list= &key.key_parts_list;

  if (parts_list && !parts_list->is_empty())
  {
    List_iterator_fast<char> it(*parts_list);
    const char *name;
    writer->add_member("used_key_parts").start_array();
    while ((name= it++))
      writer->add_str(name);
    writer->end_array();
  }

  if (quick_info && !quick_info->is_basic())
  {
    writer->add_member("index_merge").start_object();
    quick_info->print_json(writer);
    writer->end_object();
  }
  
  /* `rows` */
  writer->add_member("rows").add_ull(rows);


  if (mrr_type.length() != 0)
    writer->add_member("mrr_type").add_str(mrr_type.ptr());

  if (is_analyze)
  {
    if (doing_buffering)
    {
      ha_rows r_rows;
      double r_filtered;

      if (is_using_filesort())
      {
        if (filesort_tracker->get_r_loops())
          r_rows= (ha_rows) filesort_tracker->get_avg_examined_rows();
        else
          r_rows= 0;
        r_filtered= filesort_tracker->get_r_filtered() * 100.0;
      }
      else
      {
        if (buf_tracker.has_scans())
          r_rows= (ha_rows) buf_tracker.get_avg_rows();
        else
          r_rows= 0;
        r_filtered= buf_tracker.get_filtered_after_where() * 100.0;
      }
      writer->add_member("r_rows").add_ull(r_rows);
      writer->add_member("r_filtered").add_double(r_filtered);
    }
    else /* Not doing buffering */
    {
      writer->add_member("r_rows");
      if (tracker.has_scans())
        writer->add_double(tracker.get_avg_rows());
      else
        writer->add_null();

      /* There is no 'filtered' estimate in UPDATE/DELETE atm */
      double r_filtered= tracker.get_filtered_after_where() * 100.0;
      writer->add_member("r_filtered").add_double(r_filtered);
    }

    if (table_tracker.get_loops())
    {
      writer->add_member("r_total_time_ms").
              add_double(table_tracker.get_time_ms());
    }
  }

  if (where_cond)
  {
    writer->add_member("attached_condition");
    write_item(writer, where_cond);
  }

  /*** The part of plan that is before the buffering/sorting ends here ***/
  if (is_using_filesort())
    writer->end_object();

  if (using_io_buffer)
    writer->end_object();

  writer->end_object(); // table

  print_explain_json_for_children(query, writer, is_analyze);
  writer->end_object(); // query_block
}


int Explain_insert::print_explain(Explain_query *query, 
                                  select_result_sink *output, 
                                  uint8 explain_flags,
                                  bool is_analyze)
{
  const char *select_type="INSERT";
  print_explain_row(output, explain_flags, is_analyze,
                    1, /* id */
                    select_type,
                    table_name.c_ptr(), 
                    NULL, // partitions
                    JT_ALL,
                    NULL, // possible_keys
                    NULL, // key
                    NULL, // key_len
                    NULL, // ref
                    NULL, // rows
                    NULL, // r_rows
                    100.0, // r_filtered
                    NULL);

  return print_explain_for_children(query, output, explain_flags, is_analyze);
}

void Explain_insert::print_explain_json(Explain_query *query, 
                                        Json_writer *writer, bool is_analyze)
{
  Json_writer_nesting_guard guard(writer);

  writer->add_member("query_block").start_object();
  writer->add_member("select_id").add_ll(1);
  writer->add_member("table").start_object();
  writer->add_member("table_name").add_str(table_name.c_ptr());
  writer->end_object(); // table
  print_explain_json_for_children(query, writer, is_analyze);
  writer->end_object(); // query_block
}


void delete_explain_query(LEX *lex)
{
  DBUG_ENTER("delete_explain_query");
  delete lex->explain;
  lex->explain= NULL;
  DBUG_VOID_RETURN;
}


void create_explain_query(LEX *lex, MEM_ROOT *mem_root)
{
  DBUG_ASSERT(!lex->explain);
  DBUG_ENTER("create_explain_query");

  lex->explain= new (mem_root) Explain_query(lex->thd, mem_root);
  DBUG_ASSERT(mem_root == current_thd->mem_root);

  DBUG_VOID_RETURN;
}

void create_explain_query_if_not_exists(LEX *lex, MEM_ROOT *mem_root)
{
  if (!lex->explain)
    create_explain_query(lex, mem_root);
}


/**
  Build arrays for collectiong keys statistics, sdd possible key names
  to the list and name array

  @param alloc           MEM_ROOT to put data in
  @param list            list of possible key names to fill
  @param table           table of the keys
  @patam possible_keys   possible keys map

  @retval 0 - OK
  @retval 1 - Error
*/

int Explain_range_checked_fer::append_possible_keys_stat(MEM_ROOT *alloc,
                                                         TABLE *table,
                                                         key_map possible_keys)
{
  uint j;
  multi_alloc_root(alloc, &keys_stat, sizeof(ha_rows) * table->s->keys,
                   &keys_stat_names, sizeof(char *) * table->s->keys, NULL);
  if ((!keys_stat) || (!keys_stat_names))
  {
    keys_stat= NULL;
    keys_stat_names= NULL;
    return 1;
  }
  keys_map= possible_keys;
  keys= table->s->keys;
  bzero(keys_stat, sizeof(ha_rows) * table->s->keys);
  for (j= 0; j < table->s->keys; j++)
  {
    if (possible_keys.is_set(j))
      keys_stat_names[j]= key_set.append_str(alloc, table->key_info[j].name);
    else
      keys_stat_names[j]= NULL;
  }
  return 0;
}

void Explain_range_checked_fer::collect_data(QUICK_SELECT_I *quick)
{
  if (quick)
  {
    if (quick->index == MAX_KEY)
      index_merge++;
    else
    {
      DBUG_ASSERT(quick->index < keys);
      DBUG_ASSERT(keys_stat);
      DBUG_ASSERT(keys_stat_names);
      DBUG_ASSERT(keys_stat_names[ quick->index]);
      keys_stat[quick->index]++;
    }
  }
  else
    full_scan++;
}


void Explain_range_checked_fer::print_json(Json_writer *writer,
                                           bool is_analyze)
{
  writer->add_member("range-checked-for-each-record").start_object();
  add_json_keyset(writer, "keys", &key_set);
  if (is_analyze)
  {
    writer->add_member("r_keys").start_object();
    writer->add_member("full_scan").add_ll(full_scan);
    writer->add_member("index_merge").add_ll(index_merge);
    if (keys_stat)
    {
      writer->add_member("range").start_object();
      for (uint i= 0; i < keys; i++)
      {
        if (keys_stat_names[i])
        {
          writer->add_member(keys_stat_names[i]).add_ll(keys_stat[i]);
        }
      }
      writer->end_object();
    }
    writer->end_object();
  }
}
