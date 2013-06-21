/*
   TODO MP AB copyright
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "sql_priv.h"
#include "sql_select.h"


QPF_query::QPF_query()
{
  upd_del_plan= NULL;
  //memset(&unions, 0, sizeof(unions));
  //memset(&selects, 0, sizeof(selects));
}


QPF_query::~QPF_query()
{
  delete upd_del_plan;
  uint i;
  for (i= 0 ; i < unions.elements(); i++)
    delete unions.at(i);
  for (i= 0 ; i < selects.elements(); i++)
    delete selects.at(i);
}


QPF_node *QPF_query::get_node(uint select_id)
{
  QPF_union *u;
  if ((u= get_union(select_id)))
    return u;
  else
    return get_select(select_id);
}

QPF_union *QPF_query::get_union(uint select_id)
{
  return (unions.elements() > select_id) ? unions.at(select_id) : NULL;
}

QPF_select *QPF_query::get_select(uint select_id)
{
  return (selects.elements() > select_id) ? selects.at(select_id) : NULL;
}


void QPF_query::add_node(QPF_node *node)
{
  if (node->get_type() == QPF_node::QPF_UNION)
  {
    QPF_union *u= (QPF_union*)node;
    uint select_id= u->get_select_id();
    DBUG_ASSERT(!get_union(select_id));

    if (unions.elements() <= select_id)
      unions.resize(max(select_id+1, unions.elements()*2), NULL);

    unions.at(select_id)= u;
  }
  else
  {
    QPF_select *sel= (QPF_select*)node;
    if (sel->select_id == (int)UINT_MAX)
    {
      //TODO this is a "fake select" from a UNION.
      DBUG_ASSERT(0);
    }
    else
    {
      uint select_id= sel->select_id;
      DBUG_ASSERT(!get_select(select_id));

      if (selects.elements() <= select_id)
        selects.resize(max(select_id+1, selects.elements()*2), NULL);
      selects.at(select_id)= sel;
    }
  }
}


/*
  The main entry point to print EXPLAIN of the entire query
*/

int QPF_query::print_explain(select_result_sink *output, 
                             uint8 explain_flags)
{
  if (upd_del_plan)
  {
    upd_del_plan->print_explain(this, output, explain_flags);
    return 0;
  }
  else
  {
    // Start with id=1
    QPF_node *node= get_node(1);
    return node->print_explain(this, output, explain_flags);
  }
}


void QPF_union::push_table_name(List<Item> *item_list)
{
}


static void push_str(List<Item> *item_list, const char *str)
{
  item_list->push_back(new Item_string(str,
                                      strlen(str), system_charset_info));
}


static void push_string(List<Item> *item_list, String *str)
{
  item_list->push_back(new Item_string(str->ptr(), str->length(),
                       system_charset_info));
}


int QPF_union::print_explain(QPF_query *query, select_result_sink *output, 
                             uint8 explain_flags)
{
  // print all children, in order
  for (int i= 0; i < (int) union_members.elements(); i++)
  {
    QPF_select *sel= query->get_select(union_members.at(i));
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
  //
  {
    char table_name_buffer[SAFE_NAME_LEN];
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
    const CHARSET_INFO *cs= system_charset_info;
    item_list.push_back(new Item_string(table_name_buffer, len, cs));
  }
  //
  push_table_name(&item_list);
  
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
  const CHARSET_INFO *cs= system_charset_info;
  item_list.push_back(new Item_string(extra_buf.ptr(), extra_buf.length(), cs));

  //output->unit.offset_limit_cnt= 0; 
  if (output->send_data(item_list))
    return 1;

  return print_explain_for_children(query, output, explain_flags);
}


int QPF_node::print_explain_for_children(QPF_query *query, 
                                         select_result_sink *output,
                                         uint8 explain_flags)
{
  for (int i= 0; i < (int) children.elements(); i++)
  {
    QPF_node *node= query->get_node(children.at(i));
    if (node->print_explain(query, output, explain_flags))
      return 1;
  }
  return 0;
}


QPF_select::~QPF_select()
{
  if (join_tabs)
  {
    for (uint i= 0; i< n_join_tabs; i++)
      delete join_tabs[i];
    my_free(join_tabs);
  }
} 


int QPF_select::print_explain(QPF_query *query, select_result_sink *output,
                              uint8 explain_flags)
{
  //output->unit.offset_limit_cnt= 0; 

  if (message)
  {
    List<Item> item_list;
    const CHARSET_INFO *cs= system_charset_info;
    Item *item_null= new Item_null();

    item_list.push_back(new Item_int((int32) select_id));
    item_list.push_back(new Item_string(select_type,
                                        strlen(select_type), cs));
    for (uint i=0 ; i < 7; i++)
      item_list.push_back(item_null);
    if (explain_flags & DESCRIBE_PARTITIONS)
      item_list.push_back(item_null);
    if (explain_flags & DESCRIBE_EXTENDED)
      item_list.push_back(item_null);

    item_list.push_back(new Item_string(message,strlen(message),cs));

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


int QPF_table_access::print_explain(select_result_sink *output, uint8 explain_flags, 
                                    uint select_id, const char *select_type,
                                    bool using_temporary, bool using_filesort)
{
  List<Item> item_list;
  Item *item_null= new Item_null();
  //const CHARSET_INFO *cs= system_charset_info;
  
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
  //push_str(item_list, "TODO");
  if (possible_keys_str.length() > 0)
    push_string(&item_list, &possible_keys_str);
  else
    item_list.push_back(item_null); 

  /* `key` */
  if (key_set)
    push_string(&item_list, &key);
  else
    item_list.push_back(item_null); 

  /* `key_len` */
  if (key_len_set)
    push_string(&item_list, &key_len);
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

  const CHARSET_INFO *cs= system_charset_info;
  item_list.push_back(new Item_string(extra_buf.ptr(), extra_buf.length(), cs));

  if (output->send_data(item_list))
    return 1;

  return 0;
}


const char * extra_tag_text[]=
{
  "ET_none",
  "Using index condition",
  "Using index condition(BKA)",
  "Using ", //special
  "Range checked for each record (index map: 0x", //special
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

  "Using index for group-by", // Special?

  "USING MRR: DONT PRINT ME", // Special!

  "Distinct",
  "LooseScan",
  "Start temporary",
  "End temporary",
  "FirstMatch", //TODO: also handle special variant!

  "Using join buffer", // Special!,

  "const row not found",
  "unique row not found",
  "Impossible ON condition"
};


void QPF_table_access::append_tag_name(String *str, enum Extra_tag tag)
{
  switch (tag) {
    case ET_USING:
    {
      // quick select
      str->append(STRING_WITH_LEN("Using "));
      str->append(quick_info);
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
      str->append(join_buffer_type);
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
      str->append(loose_scan_type);
      break;
    }
    default:
     str->append(extra_tag_text[tag]);
  }
}


int QPF_delete::print_explain(QPF_query *query, select_result_sink *output, 
                              uint8 explain_flags)
{
  if (deleting_all_rows)
  {
    const char *msg= "Deleting all rows";
    int res= print_explain_message_line(output, explain_flags,
                                        1 /*select number*/,
                                        select_type, msg);
    return res;

  }
  else
  {
    return QPF_update::print_explain(query, output, explain_flags);
  }
}


int QPF_update::print_explain(QPF_query *query, select_result_sink *output, 
                              uint8 explain_flags)
{
  StringBuffer<64> extra_str;
  if (impossible_where)
  {
    const char *msg= "Impossible where";
    int res= print_explain_message_line(output, explain_flags,
                                        1 /*select number*/,
                                        select_type, msg);
    return res;
  }

  if (using_where)
    extra_str.append(STRING_WITH_LEN("Using where"));

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

  /* 
    Single-table DELETE commands do not do "Using temporary".
    "Using index condition" is also not possible (which is an unjustified limitation)
  */

  print_explain_row(output, explain_flags, 
                    1, /* id */
                    select_type,
                    table_name.c_ptr(), 
                    // partitions,
                    jtype,
                    possible_keys_line.length()? possible_keys_line.c_ptr(): NULL,
                    key_str.length()? key_str.c_ptr() : NULL,
                    key_len_str.length() ? key_len_str.c_ptr() : NULL,
                    NULL, /* 'ref' is always NULL in single-table EXPLAIN DELETE */
                    rows,
                    extra_str.c_ptr());

  return print_explain_for_children(query, output, explain_flags);
}


void delete_qpf_query(LEX *lex)
{
  delete lex->query_plan_footprint;
  lex->query_plan_footprint= NULL;
}


void create_qpf_query(LEX *lex, MEM_ROOT *mem_root)
{
  DBUG_ASSERT(!lex->query_plan_footprint);
  lex->query_plan_footprint= new QPF_query;
  DBUG_ASSERT(mem_root == current_thd->mem_root);
  lex->query_plan_footprint->mem_root= mem_root;
}

