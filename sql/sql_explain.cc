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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include <my_global.h>
#include "sql_priv.h"
#include "sql_select.h"


Explain_query::Explain_query(THD *thd_arg) : 
  upd_del_plan(NULL), insert_plan(NULL), thd(thd_arg), apc_enabled(false)
{
  operations= 0;
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
 
  if (!(result= new select_send()) || 
      thd->send_explain_fields(result))
    return 1;

  int res;
  if ((res= print_explain(result, lex->describe)))
    result->abort_result_set();
  else
    result->send_eof();

  return res;
}


/*
  The main entry point to print EXPLAIN of the entire query
*/

int Explain_query::print_explain(select_result_sink *output, 
                                 uint8 explain_flags)
{
  if (upd_del_plan)
  {
    upd_del_plan->print_explain(this, output, explain_flags);
    return 0;
  }
  else if (insert_plan)
  {
    insert_plan->print_explain(this, output, explain_flags);
    return 0;
  }
  else
  {
    /* Start printing from node with id=1 */
    Explain_node *node= get_node(1);
    if (!node)
      return 1; /* No query plan */
    return node->print_explain(this, output, explain_flags);
  }
}


bool print_explain_query(LEX *lex, THD *thd, String *str)
{
  return lex->explain->print_explain_str(thd, str);
}


/* 
  Return tabular EXPLAIN output as a text string
*/

bool Explain_query::print_explain_str(THD *thd, String *out_str)
{
  List<Item> fields;
  thd->make_explain_field_list(fields);

  select_result_text_buffer output_buf(thd);
  output_buf.send_result_set_metadata(fields, thd->lex->describe);
  if (print_explain(&output_buf, thd->lex->describe))
    return true;
  output_buf.save_to(out_str);
  return false;
}


static void push_str(List<Item> *item_list, const char *str)
{
  item_list->push_back(new Item_string_sys(str));
}


static void push_string(List<Item> *item_list, String *str)
{
  item_list->push_back(new Item_string_sys(str->ptr(), str->length()));
}


int Explain_union::print_explain(Explain_query *query, 
                                 select_result_sink *output,
                                 uint8 explain_flags)
{
  char table_name_buffer[SAFE_NAME_LEN];

  /* print all UNION children, in order */
  for (int i= 0; i < (int) union_members.elements(); i++)
  {
    Explain_select *sel= query->get_select(union_members.at(i));
    sel->print_explain(query, output, explain_flags);
  }

  /* Print a line with "UNION RESULT" */
  List<Item> item_list;
  Item *item_null= new Item_null();

  /* `id` column */
  item_list.push_back(item_null);

  /* `select_type` column */
  push_str(&item_list, fake_select_type);

  /* `table` column: something like "<union1,2>" */
  {
    uint childno= 0;
    uint len= 6, lastop= 0;
    memcpy(table_name_buffer, STRING_WITH_LEN("<union"));

    for (; childno < union_members.elements() && len + lastop + 5 < NAME_LEN;
         childno++)
    {
      len+= lastop;
      lastop= my_snprintf(table_name_buffer + len, NAME_LEN - len,
                          "%u,", union_members.at(childno));
    }

    if (childno < union_members.elements() || len + lastop >= NAME_LEN)
    {
      memcpy(table_name_buffer + len, STRING_WITH_LEN("...>") + 1);
      len+= 4;
    }
    else
    {
      len+= lastop;
      table_name_buffer[len - 1]= '>';  // change ',' to '>'
    }
    item_list.push_back(new Item_string_sys(table_name_buffer, len));
  }
  
  /* `partitions` column */
  if (explain_flags & DESCRIBE_PARTITIONS)
    item_list.push_back(item_null);

  /* `type` column */
  push_str(&item_list, join_type_str[JT_ALL]);

  /* `possible_keys` column */
  item_list.push_back(item_null);

  /* `key` */
  item_list.push_back(item_null);

  /* `key_len` */
  item_list.push_back(item_null);

  /* `ref` */
  item_list.push_back(item_null);
 
  /* `rows` */
  item_list.push_back(item_null);

  /* `filtered` */
  if (explain_flags & DESCRIBE_EXTENDED)
    item_list.push_back(item_null);

  /* `Extra` */
  StringBuffer<256> extra_buf;
  if (using_filesort)
  {
    extra_buf.append(STRING_WITH_LEN("Using filesort"));
  }
  item_list.push_back(new Item_string_sys(extra_buf.ptr(), extra_buf.length()));

  //output->unit.offset_limit_cnt= 0; 
  if (output->send_data(item_list))
    return 1;
  
  /*
    Print all subquery children (UNION children have already been printed at
    the start of this function)
  */
  return print_explain_for_children(query, output, explain_flags);
}


/*
  Print EXPLAINs for all children nodes (i.e. for subqueries)
*/

int Explain_node::print_explain_for_children(Explain_query *query, 
                                         select_result_sink *output,
                                         uint8 explain_flags)
{
  for (int i= 0; i < (int) children.elements(); i++)
  {
    Explain_node *node= query->get_node(children.at(i));
    if (node->print_explain(query, output, explain_flags))
      return 1;
  }
  return 0;
}


Explain_select::~Explain_select()
{
  if (join_tabs)
  {
    for (uint i= 0; i< n_join_tabs; i++)
      delete join_tabs[i];
    my_free(join_tabs);
  }
} 


int Explain_select::print_explain(Explain_query *query, 
                                  select_result_sink *output,
                                  uint8 explain_flags)
{
  if (message)
  {
    List<Item> item_list;
    Item *item_null= new Item_null();

    item_list.push_back(new Item_int((int32) select_id));
    item_list.push_back(new Item_string_sys(select_type));
    for (uint i=0 ; i < 7; i++)
      item_list.push_back(item_null);
    if (explain_flags & DESCRIBE_PARTITIONS)
      item_list.push_back(item_null);
    if (explain_flags & DESCRIBE_EXTENDED)
      item_list.push_back(item_null);

    item_list.push_back(new Item_string_sys(message));

    if (output->send_data(item_list))
      return 1;
  }
  else
  {
    bool using_tmp= using_temporary;
    bool using_fs= using_filesort;
    for (uint i=0; i< n_join_tabs; i++)
    {
      join_tabs[i]->print_explain(output, explain_flags, select_id,
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
  }

  return print_explain_for_children(query, output, explain_flags);
}


void Explain_table_access::push_extra(enum explain_extra_tag extra_tag)
{
  extra_tags.append(extra_tag);
}


int Explain_table_access::print_explain(select_result_sink *output, uint8 explain_flags, 
                                    uint select_id, const char *select_type,
                                    bool using_temporary, bool using_filesort)
{
  const CHARSET_INFO *cs= system_charset_info;
  const char *hash_key_prefix= "#hash#";
  bool is_hj= (type == JT_HASH || type == JT_HASH_NEXT || 
               type == JT_HASH_RANGE || type == JT_HASH_INDEX_MERGE);

  List<Item> item_list;
  Item *item_null= new Item_null();
  
  if (sjm_nest_select_id)
    select_id= sjm_nest_select_id;

  /* `id` column */
  item_list.push_back(new Item_int((int32) select_id));

  /* `select_type` column */
  if (sjm_nest_select_id)
    push_str(&item_list, "MATERIALIZED");
  else
    push_str(&item_list, select_type);

  /* `table` column */
  push_string(&item_list, &table_name);
  
  /* `partitions` column */
  if (explain_flags & DESCRIBE_PARTITIONS)
  {
    if (used_partitions_set)
    {
      push_string(&item_list, &used_partitions);
    }
    else
      item_list.push_back(item_null); 
  }

  /* `type` column */
  push_str(&item_list, join_type_str[type]);

  /* `possible_keys` column */
  if (possible_keys_str.length() > 0)
    push_string(&item_list, &possible_keys_str);
  else
    item_list.push_back(item_null); 

  /* `key` */
  StringBuffer<64> key_str;
  if (key.get_key_name())
  {
    if (is_hj)
      key_str.append(hash_key_prefix, strlen(hash_key_prefix), cs);

    key_str.append(key.get_key_name());

    if (is_hj && type != JT_HASH)
      key_str.append(':');
  }
  
  if (quick_info)
  {
    StringBuffer<64> buf2;
    quick_info->print_key(&buf2);
    key_str.append(buf2);
  }
  if (type == JT_HASH_NEXT)
    key_str.append(hash_next_key.get_key_name());
  
  if (key_str.length() > 0)
    push_string(&item_list, &key_str);
  else
    item_list.push_back(item_null); 

  /* `key_len` */
  StringBuffer<64> key_len_str;

  if (key.get_key_len() != (uint)-1)
  {
    char buf[64];
    size_t length;
    length= longlong10_to_str(key.get_key_len(), buf, 10) - buf;
    key_len_str.append(buf, length);
    if (is_hj && type != JT_HASH)
      key_len_str.append(':');
  }

  if (quick_info)
  {
    StringBuffer<64> buf2;
    quick_info->print_key_len(&buf2);
    key_len_str.append(buf2);
  } 

  if (type == JT_HASH_NEXT)
  {
    char buf[64];
    size_t length;
    length= longlong10_to_str(hash_next_key.get_key_len(), buf, 10) - buf;
    key_len_str.append(buf, length);
  }

  if (key_len_str.length() > 0)
    push_string(&item_list, &key_len_str);
  else
    item_list.push_back(item_null);

  /* `ref` */
  if (ref_set)
    push_string(&item_list, &ref);
  else
    item_list.push_back(item_null);
 
  /* `rows` */
  if (rows_set)
  {
    item_list.push_back(new Item_int((longlong) (ulonglong) rows, 
                         MY_INT64_NUM_DECIMAL_DIGITS));
  }
  else
    item_list.push_back(item_null);

  /* `filtered` */
  if (explain_flags & DESCRIBE_EXTENDED)
  {
    if (filtered_set)
    {
      item_list.push_back(new Item_float(filtered, 2));
    }
    else
      item_list.push_back(item_null);
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

  if (using_filesort)
  {
    if (first)
      first= false;
    else
      extra_buf.append(STRING_WITH_LEN("; "));
    extra_buf.append(STRING_WITH_LEN("Using filesort"));
  }

  item_list.push_back(new Item_string_sys(extra_buf.ptr(), extra_buf.length()));

  if (output->send_data(item_list))
    return 1;

  return 0;
}


/*
  Elements in this array match members of enum Extra_tag, defined in
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
      str->append(range_checked_map.print(buf));
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
        str->append(bka_type.mrr_type);

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


void Explain_quick_select::print_extra_recursive(String *str)
{
  if (quick_type == QUICK_SELECT_I::QS_TYPE_RANGE || 
      quick_type == QUICK_SELECT_I::QS_TYPE_RANGE_DESC)
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
                                  uint8 explain_flags)
{
  if (deleting_all_rows)
  {
    const char *msg= "Deleting all rows";
    int res= print_explain_message_line(output, explain_flags,
                                        1 /*select number*/,
                                        select_type, &rows, msg);
    return res;

  }
  else
  {
    return Explain_update::print_explain(query, output, explain_flags);
  }
}


int Explain_update::print_explain(Explain_query *query, 
                                  select_result_sink *output,
                                  uint8 explain_flags)
{
  StringBuffer<64> key_buf;
  StringBuffer<64> key_len_buf;
  StringBuffer<64> extra_str;
  if (impossible_where || no_partitions)
  {
    const char *msg= impossible_where ? 
                     "Impossible WHERE" : 
                     "No matching rows after partition pruning";
    int res= print_explain_message_line(output, explain_flags,
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
  else
  {
    key_buf.copy(key_str);
    key_len_buf.copy(key_len_str);
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
  
  if (using_filesort)
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

  print_explain_row(output, explain_flags, 
                    1, /* id */
                    select_type,
                    table_name.c_ptr(), 
                    used_partitions_set? used_partitions.c_ptr() : NULL,
                    jtype,
                    possible_keys_line.length()? possible_keys_line.c_ptr(): NULL,
                    key_buf.length()? key_buf.c_ptr() : NULL,
                    key_len_buf.length() ? key_len_buf.c_ptr() : NULL,
                    NULL, /* 'ref' is always NULL in single-table EXPLAIN DELETE */
                    &rows,
                    extra_str.c_ptr_safe());

  return print_explain_for_children(query, output, explain_flags);
}


int Explain_insert::print_explain(Explain_query *query, 
                                  select_result_sink *output, 
                                  uint8 explain_flags)
{
  const char *select_type="INSERT";
  print_explain_row(output, explain_flags, 
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
                    NULL);

  return print_explain_for_children(query, output, explain_flags);
}


void delete_explain_query(LEX *lex)
{
  delete lex->explain;
  lex->explain= NULL;
}


void create_explain_query(LEX *lex, MEM_ROOT *mem_root)
{
  DBUG_ASSERT(!lex->explain);
  lex->explain= new Explain_query(lex->thd);
  DBUG_ASSERT(mem_root == current_thd->mem_root);
  lex->explain->mem_root= mem_root;
}

void create_explain_query_if_not_exists(LEX *lex, MEM_ROOT *mem_root)
{
  if (!lex->explain)
    create_explain_query(lex, mem_root);
}

