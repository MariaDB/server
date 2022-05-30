/* Copyright (c) 2002, 2012, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_help.h"
#include "sql_table.h"                          // primary_key_name
#include "sql_base.h"               // REPORT_ALL_ERRORS, setup_tables
#include "opt_range.h"              // SQL_SELECT
#include "records.h"          // init_read_record, end_read_record

struct st_find_field
{
  const char *table_name, *field_name;
  Field *field;
};

/* Used fields */

static struct st_find_field init_used_fields[]=
{
  { "help_topic",    "help_topic_id",      0},
  { "help_topic",    "name",               0},
  { "help_topic",    "help_category_id",   0},
  { "help_topic",    "description",        0},
  { "help_topic",    "example",            0},

  { "help_category", "help_category_id",   0},
  { "help_category", "parent_category_id", 0},
  { "help_category", "name",               0},

  { "help_keyword",  "help_keyword_id",    0},
  { "help_keyword",  "name",               0},

  { "help_relation", "help_topic_id",      0},
  { "help_relation", "help_keyword_id",    0}
};

enum enum_used_fields
{
  help_topic_help_topic_id= 0,
  help_topic_name,
  help_topic_help_category_id,
  help_topic_description,
  help_topic_example,

  help_category_help_category_id,
  help_category_parent_category_id,
  help_category_name,

  help_keyword_help_keyword_id,
  help_keyword_name,

  help_relation_help_topic_id,
  help_relation_help_keyword_id
};


/*
  Fill st_find_field structure with pointers to fields

  SYNOPSIS
    init_fields()
    thd          Thread handler
    tables       list of all tables for fields
    find_fields  array of structures
    count        size of previous array

  RETURN VALUES
    0           all ok
    1           one of the fileds was not found
*/

static bool init_fields(THD *thd, TABLE_LIST *tables,
			struct st_find_field *find_fields, uint count)
{
  Name_resolution_context *context= &thd->lex->first_select_lex()->context;
  DBUG_ENTER("init_fields");
  context->resolve_in_table_list_only(tables);
  for (; count-- ; find_fields++)
  {
    /* We have to use 'new' here as field will be re_linked on free */
    Item_field *field= (new (thd->mem_root)
                        Item_field(thd, context,
                                   {STRING_WITH_LEN("mysql")},
                                   Lex_cstring_strlen(find_fields->table_name),
                                   Lex_cstring_strlen(find_fields->field_name)));
    if (!(find_fields->field= find_field_in_tables(thd, field, tables, NULL,
                                                   ignored_tables_list_t(NULL),
						   0, REPORT_ALL_ERRORS, 1,
                                                   TRUE)))
      DBUG_RETURN(1);
    bitmap_set_bit(find_fields->field->table->read_set,
                   find_fields->field->field_index);
    /* To make life easier when setting values in keys */
    bitmap_set_bit(find_fields->field->table->write_set,
                   find_fields->field->field_index);
  }
  DBUG_RETURN(0);
}


/*
  Returns variants of found topic for help (if it is just single topic,
    returns description and example, or else returns only names..)

  SYNOPSIS
    memorize_variant_topic()

    thd           Thread handler
    topics        Table of topics
    count         number of alredy found topics
    find_fields   Filled array of information for work with fields

  RETURN VALUES
    names         array of names of found topics (out)

    name          name of found topic (out)
    description   description of found topic (out)
    example       example for found topic (out)

  NOTE
    Field 'names' is set only if more than one topic is found.
    Fields 'name', 'description', 'example' are set only if
    found exactly one topic.
*/

void memorize_variant_topic(THD *thd, TABLE *topics, int count,
			    struct st_find_field *find_fields,
			    List<String> *names,
			    String *name, String *description, String *example)
{
  DBUG_ENTER("memorize_variant_topic");
  MEM_ROOT *mem_root= thd->mem_root;
  if (count==0)
  {
    get_field(mem_root,find_fields[help_topic_name].field,        name);
    get_field(mem_root,find_fields[help_topic_description].field, description);
    get_field(mem_root,find_fields[help_topic_example].field,     example);
  }
  else
  {
    if (count == 1)
      names->push_back(name, thd->mem_root);
    String *new_name= new (thd->mem_root) String;
    get_field(mem_root,find_fields[help_topic_name].field,new_name);
    names->push_back(new_name, thd->mem_root);
  }
  DBUG_VOID_RETURN;
}

/*
  Look for topics by mask

  SYNOPSIS
    search_topics()
    thd 	 Thread handler
    topics	 Table of topics
    find_fields  Filled array of info for fields
    select	 Function to test for matching help topic.
		 Normally 'help_topic.name like 'bit%'

  RETURN VALUES
    #   number of topics found

    names        array of names of found topics (out)
    name         name of found topic (out)
    description  description of found topic (out)
    example      example for found topic (out)

  NOTE
    Field 'names' is set only if more than one topic was found.
    Fields 'name', 'description', 'example' are set only if
    exactly one topic was found.

*/

int search_topics(THD *thd, TABLE *topics, struct st_find_field *find_fields,
		  SQL_SELECT *select, List<String> *names,
		  String *name, String *description, String *example)
{
  int count= 0;
  READ_RECORD read_record_info;
  DBUG_ENTER("search_topics");

  /* Should never happen. As this is part of help, we can ignore this */
  if (init_read_record(&read_record_info, thd, topics, select, NULL, 1, 0,
                       FALSE))
    DBUG_RETURN(0);

  while (!read_record_info.read_record())
  {
    if (!select->cond->val_int())		// Doesn't match like
      continue;
    memorize_variant_topic(thd,topics,count,find_fields,
			   names,name,description,example);
    count++;
  }
  end_read_record(&read_record_info);

  DBUG_RETURN(count);
}

/*
  Look for keyword by mask

  SYNOPSIS
    search_keyword()
    thd          Thread handler
    keywords     Table of keywords
    find_fields  Filled array of info for fields
    select       Function to test for matching keyword.
	         Normally 'help_keyword.name like 'bit%'

    key_id       help_keyword_if of found topics (out)

  RETURN VALUES
    0   didn't find any topics matching the mask
    1   found exactly one topic matching the mask
    2   found more then one topic matching the mask
*/

int search_keyword(THD *thd, TABLE *keywords,
                   struct st_find_field *find_fields,
                   SQL_SELECT *select, int *key_id)
{
  int count= 0;
  READ_RECORD read_record_info;
  DBUG_ENTER("search_keyword");
  /* Should never happen. As this is part of help, we can ignore this */
  if (init_read_record(&read_record_info, thd, keywords, select, NULL, 1, 0,
                       FALSE))
    DBUG_RETURN(0);

  while (!read_record_info.read_record() && count<2)
  {
    if (!select->cond->val_int())		// Dosn't match like
      continue;

    *key_id= (int)find_fields[help_keyword_help_keyword_id].field->val_int();

    count++;
  }
  end_read_record(&read_record_info);

  DBUG_RETURN(count);
}

/*
  Look for all topics with keyword

  SYNOPSIS
    get_topics_for_keyword()
    thd		 Thread handler
    topics	 Table of topics
    relations	 Table of m:m relation "topic/keyword"
    find_fields  Filled array of info for fields
    key_id	 Primary index to use to find for keyword

  RETURN VALUES
    #   number of topics found

    names        array of name of found topics (out)

    name         name of found topic (out)
    description  description of found topic (out)
    example      example for found topic (out)

  NOTE
    Field 'names' is set only if more than one topic was found.
    Fields 'name', 'description', 'example' are set only if
    exactly one topic was found.
*/

int get_topics_for_keyword(THD *thd, TABLE *topics, TABLE *relations,
			   struct st_find_field *find_fields, int16 key_id,
			   List<String> *names,
			   String *name, String *description, String *example)
{
  uchar buff[8];	// Max int length
  int count= 0;
  int iindex_topic, iindex_relations;
  Field *rtopic_id, *rkey_id;
  DBUG_ENTER("get_topics_for_keyword");

  if ((iindex_topic=
       find_type(primary_key_name.str, &topics->s->keynames,
                 FIND_TYPE_NO_PREFIX) - 1) < 0 ||
      (iindex_relations=
       find_type(primary_key_name.str, &relations->s->keynames,
                 FIND_TYPE_NO_PREFIX) - 1) < 0)
  {
    my_message(ER_CORRUPT_HELP_DB, ER_THD(thd, ER_CORRUPT_HELP_DB), MYF(0));
    DBUG_RETURN(-1);
  }
  rtopic_id= find_fields[help_relation_help_topic_id].field;
  rkey_id=   find_fields[help_relation_help_keyword_id].field;

  if (topics->file->ha_index_init(iindex_topic,1) ||
      relations->file->ha_index_init(iindex_relations,1))
  {
    if (topics->file->inited)
      topics->file->ha_index_end();
    my_message(ER_CORRUPT_HELP_DB, ER_THD(thd, ER_CORRUPT_HELP_DB), MYF(0));
    DBUG_RETURN(-1);
  }

  rkey_id->store((longlong) key_id, TRUE);
  rkey_id->get_key_image(buff, rkey_id->pack_length(), Field::itRAW);
  int key_res= relations->file->ha_index_read_map(relations->record[0],
                                                  buff, (key_part_map) 1,
                                                  HA_READ_KEY_EXACT);

  for ( ;
        !key_res && key_id == (int16) rkey_id->val_int() ;
	key_res= relations->file->ha_index_next(relations->record[0]))
  {
    uchar topic_id_buff[8];
    longlong topic_id= rtopic_id->val_int();
    Field *field= find_fields[help_topic_help_topic_id].field;
    field->store((longlong) topic_id, TRUE);
    field->get_key_image(topic_id_buff, field->pack_length(), Field::itRAW);

    if (!topics->file->ha_index_read_map(topics->record[0], topic_id_buff,
                                         (key_part_map)1, HA_READ_KEY_EXACT))
    {
      memorize_variant_topic(thd,topics,count,find_fields,
			     names,name,description,example);
      count++;
    }
  }
  topics->file->ha_index_end();
  relations->file->ha_index_end();
  DBUG_RETURN(count);
}

/*
  Look for categories by mask

  SYNOPSIS
    search_categories()
    thd			THD for init_read_record
    categories		Table of categories
    find_fields         Filled array of info for fields
    select		Function to test for if matching help topic.
			Normally 'help_vategory.name like 'bit%'
    names		List of found categories names (out)
    res_id		Primary index of found category (only if
			found exactly one category)

  RETURN VALUES
    #			Number of categories found
*/

int search_categories(THD *thd, TABLE *categories,
		      struct st_find_field *find_fields,
		      SQL_SELECT *select, List<String> *names, int16 *res_id)
{
  Field *pfname= find_fields[help_category_name].field;
  Field *pcat_id= find_fields[help_category_help_category_id].field;
  int count= 0;
  READ_RECORD read_record_info;
  DBUG_ENTER("search_categories");

  /* Should never happen. As this is part of help, we can ignore this */
  if (init_read_record(&read_record_info, thd, categories, select, NULL,
                       1, 0, FALSE))
    DBUG_RETURN(0);
  while (!read_record_info.read_record())
  {
    if (select && !select->cond->val_int())
      continue;
    String *lname= new (thd->mem_root) String;
    get_field(thd->mem_root,pfname,lname);
    if (++count == 1 && res_id)
      *res_id= (int16) pcat_id->val_int();
    names->push_back(lname, thd->mem_root);
  }
  end_read_record(&read_record_info);

  DBUG_RETURN(count);
}

/*
  Look for all topics or subcategories of category

  SYNOPSIS
    get_all_items_for_category()
    thd	    Thread handler
    items   Table of items
    pfname  Field "name" in items
    select  "where" part of query..
    res     list of finded names
*/

void get_all_items_for_category(THD *thd, TABLE *items, Field *pfname,
				SQL_SELECT *select, List<String> *res)
{
  READ_RECORD read_record_info;
  DBUG_ENTER("get_all_items_for_category");

  /* Should never happen. As this is part of help, we can ignore this */
  if (init_read_record(&read_record_info, thd, items, select, NULL, 1, 0,
                       FALSE))
    DBUG_VOID_RETURN;

  while (!read_record_info.read_record())
  {
    if (!select->cond->val_int())
      continue;
    String *name= new (thd->mem_root) String();
    get_field(thd->mem_root,pfname,name);
    res->push_back(name, thd->mem_root);
  }
  end_read_record(&read_record_info);

  DBUG_VOID_RETURN;
}


/**
  Collect field names of HELP header that will be sent to a client

  @param      thd         Thread data object
  @param[out] field_list  List of fields whose metadata should be collected for
                          sending to client
*/

static void fill_answer_1_fields(THD *thd, List<Item> *field_list)
{
  MEM_ROOT *mem_root= thd->mem_root;

  field_list->push_back(new (mem_root) Item_empty_string(thd, "name", 64),
                        mem_root);
  field_list->push_back(new (mem_root) Item_empty_string(thd, "description",
                                                         1000),
                        mem_root);
  field_list->push_back(new (mem_root) Item_empty_string(thd, "example", 1000),
                        mem_root);
}


/**
  Send metadata of an answer on help request to a client

  @param protocol   protocol for sending
*/

static bool send_answer_1_metadata(Protocol *protocol)
{
  List<Item> field_list;

  fill_answer_1_fields(protocol->thd, &field_list);
  return protocol->send_result_set_metadata(&field_list,
                                            Protocol::SEND_NUM_ROWS |
                                            Protocol::SEND_EOF);
}


/*
  Send to client answer for help request

  SYNOPSIS
    send_answer_1()
    protocol - protocol for sending
    s1 - value of column "Name"
    s2 - value of column "Description"
    s3 - value of column "Example"

  IMPLEMENTATION
   Format used:
   +----------+------------+------------+
   |name      |description |example     |
   +----------+------------+------------+
   |String(64)|String(1000)|String(1000)|
   +----------+------------+------------+
   with exactly one row!

  RETURN VALUES
    1		Writing of head failed
    -1		Writing of row failed
    0		Successeful send
*/

static int send_answer_1(Protocol *protocol, String *s1, String *s2, String *s3)
{
  DBUG_ENTER("send_answer_1");

  if (send_answer_1_metadata(protocol))
    DBUG_RETURN(1);

  protocol->prepare_for_resend();
  protocol->store(s1);
  protocol->store(s2);
  protocol->store(s3);
  if (protocol->write())
    DBUG_RETURN(-1);
  DBUG_RETURN(0);
}


/**
  Collect field names of HELP header that will be sent to a client

  @param      thd          Thread data object
  @param[out] field_list   List of fields whose metadata should be collected for
                           sending to client
  @param      for_category need column 'source_category_name'
*/

static void fill_header_2_fields(THD *thd, List<Item> *field_list,
                                 bool for_category)
{
  MEM_ROOT *mem_root= thd->mem_root;
  if (for_category)
    field_list->push_back(new (mem_root)
                          Item_empty_string(thd, "source_category_name", 64),
                          mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "name", 64),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "is_it_category", 1),
                        mem_root);
}


/*
  Send to client help header

  SYNOPSIS
   send_header_2()
    protocol       - protocol for sending
    for_category   - need column 'source_category_name'

  IMPLEMENTATION
   +-                    -+
   |+-------------------- | +----------+--------------+
   ||source_category_name | |name      |is_it_category|
   |+-------------------- | +----------+--------------+
   ||String(64)           | |String(64)|String(1)     |
   |+-------------------- | +----------+--------------+
   +-                    -+

  RETURN VALUES
    result of protocol->send_result_set_metadata
*/

static int send_header_2(Protocol *protocol, bool for_category)
{
  DBUG_ENTER("send_header_2");
  List<Item> field_list;

  fill_header_2_fields(protocol->thd, &field_list, for_category);
  DBUG_RETURN(protocol->send_result_set_metadata(&field_list,
                                                 Protocol::SEND_NUM_ROWS |
                                                 Protocol::SEND_EOF));
}

/*
  strcmp for using in qsort

  SYNOPSIS
    strptrcmp()
    ptr1   (const void*)&str1
    ptr2   (const void*)&str2

  RETURN VALUES
    same as strcmp
*/

extern "C" int string_ptr_cmp(const void* ptr1, const void* ptr2)
{
  String *str1= *(String**)ptr1;
  String *str2= *(String**)ptr2;
  uint length1= str1->length();
  uint length2= str2->length();
  int tmp= memcmp(str1->ptr(),str2->ptr(), MY_MIN(length1, length2));
  if (tmp)
    return tmp;
  return (int) length2 - (int) length1;
}

/*
  Send to client rows in format:
   column1 : <name>
   column2 : <is_it_category>

  SYNOPSIS
    send_variant_2_list()
    protocol     Protocol for sending
    names        List of names
    cat	         Value of the column <is_it_category>
    source_name  name of category for all items..

  RETURN VALUES
    -1 	Writing fail
    0	Data was successefully send
*/

int send_variant_2_list(MEM_ROOT *mem_root, Protocol *protocol,
			List<String> *names,
			const char *cat, String *source_name)
{
  DBUG_ENTER("send_variant_2_list");

  String **pointers= (String**)alloc_root(mem_root,
					  sizeof(String*)*names->elements);
  String **pos;
  String **end= pointers + names->elements;

  List_iterator<String> it(*names);
  for (pos= pointers; pos!=end; (*pos++= it++))
    ;

  my_qsort(pointers,names->elements,sizeof(String*),string_ptr_cmp);

  for (pos= pointers; pos!=end; pos++)
  {
    protocol->prepare_for_resend();
    if (source_name)
      protocol->store(source_name);
    protocol->store(*pos);
    protocol->store(cat,1,&my_charset_latin1);
    if (protocol->write())
      DBUG_RETURN(-1);
  }

  DBUG_RETURN(0);
}

/*
  Prepare simple SQL_SELECT table.* WHERE <Item>

  SYNOPSIS
    prepare_simple_select()
    thd      Thread handler
    cond     WHERE part of select
    table    goal table

    error    code of error (out)

  RETURN VALUES
    #  created SQL_SELECT
*/

SQL_SELECT *prepare_simple_select(THD *thd, Item *cond,
				  TABLE *table, int *error)
{
  cond->fix_fields_if_needed(thd, &cond);  // can never fail

  /* Assume that no indexes cover all required fields */
  table->covering_keys.clear_all();

  SQL_SELECT *res= make_select(table, 0, 0, cond, 0, 0, error);
  if (unlikely(*error) ||
      (likely(res) && unlikely(res->check_quick(thd, 0, HA_POS_ERROR))) ||
      (likely(res) && res->quick && unlikely(res->quick->reset())))
  {
    delete res;
    res=0;
  }
  return res;
}

/*
  Prepare simple SQL_SELECT table.* WHERE table.name LIKE mask

  SYNOPSIS
    prepare_select_for_name()
    thd      Thread handler
    mask     mask for compare with name
    mlen     length of mask
    table    goal table
    pfname   field "name" in table

    error    code of error (out)

  RETURN VALUES
    #  created SQL_SELECT
*/

SQL_SELECT *prepare_select_for_name(THD *thd, const char *mask, size_t mlen,
				    TABLE *table, Field *pfname, int *error)
{
  MEM_ROOT *mem_root= thd->mem_root;
  Item *cond= new (mem_root)
    Item_func_like(thd,
                   new (mem_root)
                   Item_field(thd, pfname),
                   new (mem_root) Item_string(thd, mask, (uint)mlen,
                                              pfname->charset()),
                   new (mem_root) Item_string_ascii(thd, "\\"),
                   FALSE);
  if (unlikely(thd->is_fatal_error))
    return 0;					// OOM
  return prepare_simple_select(thd, cond, table, error);
}


/**
  Initialize the TABLE_LIST with tables used in HELP statement handling.

  @param thd      Thread handler
  @param tables   Array of four TABLE_LIST objects to initialize with data
                  about the tables help_topic, help_category, help_relation,
                  help_keyword
*/

static void initialize_tables_for_help_command(THD *thd, TABLE_LIST *tables)
{
  LEX_CSTRING MYSQL_HELP_TOPIC_NAME=    {STRING_WITH_LEN("help_topic") };
  LEX_CSTRING MYSQL_HELP_CATEGORY_NAME= {STRING_WITH_LEN("help_category") };
  LEX_CSTRING MYSQL_HELP_RELATION_NAME= {STRING_WITH_LEN("help_relation") };
  LEX_CSTRING MYSQL_HELP_KEYWORD_NAME=  {STRING_WITH_LEN("help_keyword") };

  tables[0].init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_HELP_TOPIC_NAME, 0,
                           TL_READ);
  tables[1].init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_HELP_CATEGORY_NAME, 0,
                           TL_READ);
  tables[2].init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_HELP_RELATION_NAME, 0,
                           TL_READ);
  tables[3].init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_HELP_KEYWORD_NAME, 0,
                           TL_READ);
  tables[0].next_global= tables[0].next_local=
    tables[0].next_name_resolution_table= &tables[1];
  tables[1].next_global= tables[1].next_local=
    tables[1].next_name_resolution_table= &tables[2];
  tables[2].next_global= tables[2].next_local=
    tables[2].next_name_resolution_table= &tables[3];
}


/**
  Setup tables and fields for query.

  @param thd               Thread handler
  @param first_select_lex  SELECT_LEX of the parsed statement
  @param tables            Array of tables used in handling of the HELP
                           statement
  @param used_fields       Array of fields used in handling of the HELP
                           statement

  @return false on success, else true.
*/

template <size_t M, size_t N>
static bool init_items_for_help_command(THD *thd,
                                        SELECT_LEX *first_select_lex,
                                        TABLE_LIST (&tables)[M],
                                        st_find_field (& used_fields)[N])
{
  List<TABLE_LIST> leaves;

  /*
    Initialize tables and fields to be usable from items.
    tables do not contain VIEWs => we can pass 0 as conds
  */
  first_select_lex->context.table_list=
    first_select_lex->context.first_name_resolution_table=
      &tables[0];

  if (setup_tables(thd, &first_select_lex->context,
                   &first_select_lex->top_join_list,
                   &tables[0], leaves, false, false))
    return true;

  memcpy((char*) used_fields, (char*) init_used_fields,
         sizeof(used_fields[0]) * N);
  if (init_fields(thd, &tables[0], used_fields, N))
    return true;

  for (size_t i= 0; i < M; i++)
    tables[i].table->file->init_table_handle_for_HANDLER();

  return false;
}


/**
  Prepare (in the sense of prepared statement) the HELP statement.

  @param thd          Thread handler
  @param mask         string value passed to the HELP statement
  @oaram[out] fields  fields for result set metadata

  @return false on success, else true.
*/

bool mysqld_help_prepare(THD *thd, const char *mask, List<Item> *fields)
{
  TABLE_LIST tables[4];
  st_find_field used_fields[array_elements(init_used_fields)];
  SQL_SELECT *select;

  List<String> topics_list;

  Sql_mode_instant_remove sms(thd, MODE_PAD_CHAR_TO_FULL_LENGTH);
  initialize_tables_for_help_command(thd, tables);

  /*
    HELP must be available under LOCK TABLES.
    Reset and backup the current open tables state to
    make it possible.
  */
  start_new_trans new_trans(thd);

  if (open_system_tables_for_read(thd, tables))
    return true;

  auto cleanup_and_return= [&](bool ret)
  {
    thd->commit_whole_transaction_and_close_tables();
    new_trans.restore_old_transaction();
    return ret;
  };

  if (init_items_for_help_command(thd, thd->lex->first_select_lex(),
                                  tables, used_fields))
    return cleanup_and_return(false);

  size_t mlen= strlen(mask);
  int error;

  /*
    Prepare the query 'SELECT * FROM help_topic WHERE name LIKE mask'
    for execution
  */
  if (!(select=
        prepare_select_for_name(thd,mask, mlen, tables[0].table,
                                used_fields[help_topic_name].field, &error)))
    return cleanup_and_return(true);

  String name, description, example;
  /*
    Run the query 'SELECT * FROM help_topic WHERE name LIKE mask'
  */
  int count_topics= search_topics(thd, tables[0].table, used_fields,
                                  select, &topics_list,
                                  &name, &description, &example);
  delete select;

  if (thd->is_error())
    return cleanup_and_return(true);

  if (count_topics == 0)
  {
    int UNINIT_VAR(key_id);
    /*
      Prepare the query 'SELECT * FROM help_keyword WHERE name LIKE mask'
      for execution
    */
    if (!(select=
        prepare_select_for_name(thd, mask, mlen, tables[3].table,
                                used_fields[help_keyword_name].field,
                                &error)))
      return cleanup_and_return(true);

    /*
      Run the query 'SELECT * FROM help_keyword WHERE name LIKE mask'
    */
    count_topics= search_keyword(thd,tables[3].table, used_fields, select,
                                 &key_id);
    delete select;
    count_topics= (count_topics != 1) ? 0 :
        get_topics_for_keyword(thd, tables[0].table, tables[2].table,
                               used_fields, key_id, &topics_list, &name,
                               &description, &example);

  }

  if (count_topics == 0)
  {
    if (!(select=
        prepare_select_for_name(thd, mask, mlen, tables[1].table,
                                used_fields[help_category_name].field,
                                &error)))
      return cleanup_and_return(true);

    List<String> categories_list;
    int16 category_id;
    int count_categories= search_categories(thd, tables[1].table, used_fields,
                                            select,
                                            &categories_list,&category_id);
    delete select;
    if (count_categories == 1)
      fill_header_2_fields(thd, fields, true);
    else
      fill_header_2_fields(thd, fields, false);
  }
  else if (count_topics == 1)
    fill_answer_1_fields(thd, fields);
  else
    fill_header_2_fields(thd, fields, false);

  return cleanup_and_return(false);
}


/*
  Server-side function 'help'

  SYNOPSIS
    mysqld_help()
    thd			Thread handler

  RETURN VALUES
    FALSE Success
    TRUE  Error and send_error already committed
*/

static bool mysqld_help_internal(THD *thd, const char *mask)
{
  Protocol *protocol= thd->protocol;
  SQL_SELECT *select;
  st_find_field used_fields[array_elements(init_used_fields)];
  TABLE_LIST tables[4];
  List<String> topics_list, categories_list, subcategories_list;
  String name, description, example;
  int count_topics, count_categories, error;
  size_t mlen= strlen(mask);
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("mysqld_help");

  initialize_tables_for_help_command(thd, tables);

  /*
    HELP must be available under LOCK TABLES. 
    Reset and backup the current open tables state to
    make it possible.
  */
  start_new_trans new_trans(thd);

  if (open_system_tables_for_read(thd, tables))
    goto error2;

  if (init_items_for_help_command(thd, thd->lex->first_select_lex(),
                                  tables, used_fields))
    goto error;

  if (!(select=
	prepare_select_for_name(thd,mask,mlen,tables[0].table,
				used_fields[help_topic_name].field,&error)))
    goto error;

  count_topics= search_topics(thd,tables[0].table,used_fields,
			      select,&topics_list,
			      &name, &description, &example);
  delete select;

  if (thd->is_error())
    goto error;

  if (count_topics == 0)
  {
    int UNINIT_VAR(key_id);
    if (!(select=
          prepare_select_for_name(thd,mask,mlen,tables[3].table,
                                  used_fields[help_keyword_name].field,
                                  &error)))
      goto error;

    count_topics= search_keyword(thd,tables[3].table, used_fields, select,
                                 &key_id);
    delete select;
    count_topics= (count_topics != 1) ? 0 :
                  get_topics_for_keyword(thd,tables[0].table,tables[2].table,
                                         used_fields,key_id,&topics_list,&name,
                                         &description,&example);
  }

  if (count_topics == 0)
  {
    int16 category_id;
    Field *cat_cat_id= used_fields[help_category_parent_category_id].field;
    if (!(select=
          prepare_select_for_name(thd,mask,mlen,tables[1].table,
                                  used_fields[help_category_name].field,
                                  &error)))
      goto error;

    count_categories= search_categories(thd, tables[1].table, used_fields,
					select,
					&categories_list,&category_id);
    delete select;
    if (!count_categories)
    {
      if (send_header_2(protocol,FALSE))
	goto error;
    }
    else if (count_categories > 1)
    {
      if (send_header_2(protocol,FALSE) ||
	  send_variant_2_list(mem_root,protocol,&categories_list,"Y",0))
	goto error;
    }
    else
    {
      Field *topic_cat_id= used_fields[help_topic_help_category_id].field;
      Item *cond_topic_by_cat=
        new (mem_root)
        Item_func_equal(thd,
                        new (mem_root)
                        Item_field(thd, topic_cat_id),
                        new (mem_root)
                        Item_int(thd, (int32) category_id));
      Item *cond_cat_by_cat=
        new (mem_root)
        Item_func_equal(thd,
                        new (mem_root) Item_field(thd, cat_cat_id),
                        new (mem_root) Item_int(thd, (int32) category_id));
      if (!(select= prepare_simple_select(thd, cond_topic_by_cat,
                                          tables[0].table, &error)))
        goto error;
      get_all_items_for_category(thd,tables[0].table,
				 used_fields[help_topic_name].field,
				 select,&topics_list);
      delete select;
      if (!(select= prepare_simple_select(thd, cond_cat_by_cat,
                                          tables[1].table, &error)))
        goto error;
      get_all_items_for_category(thd,tables[1].table,
				 used_fields[help_category_name].field,
				 select,&subcategories_list);
      delete select;
      String *cat= categories_list.head();
      if (send_header_2(protocol, TRUE) ||
	  send_variant_2_list(mem_root,protocol,&topics_list,       "N",cat) ||
	  send_variant_2_list(mem_root,protocol,&subcategories_list,"Y",cat))
	goto error;
    }
  }
  else if (count_topics == 1)
  {
    if (send_answer_1(protocol,&name,&description,&example))
      goto error;
  }
  else
  {
    /* First send header and functions */
    if (send_header_2(protocol, FALSE) ||
	send_variant_2_list(mem_root,protocol, &topics_list, "N", 0))
      goto error;
    if (!(select=
          prepare_select_for_name(thd,mask,mlen,tables[1].table,
                                  used_fields[help_category_name].field,&error)))
      goto error;
    search_categories(thd, tables[1].table, used_fields,
		      select,&categories_list, 0);
    delete select;
    /* Then send categories */
    if (send_variant_2_list(mem_root,protocol, &categories_list, "Y", 0))
      goto error;
  }
  my_eof(thd);

  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();
  DBUG_RETURN(FALSE);

error:
  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

error2:
  DBUG_RETURN(TRUE);
}


bool mysqld_help(THD *thd, const char *mask)
{
  Sql_mode_instant_remove sms(thd, MODE_PAD_CHAR_TO_FULL_LENGTH);
  bool rc= mysqld_help_internal(thd, mask);
  return rc;
}
