#include "table.h"
#include "sql_lex.h"
#include "protocol.h"
#include "sql_plugin.h"
#include "sql_insert.h"
#include "create_options.h"

#define NO_YACC_SYMBOLS
#include "lex_analyzer.h"

#define MYSQL_SERVER 1
#define MYSQL_OPEN_FORCE_SHARED_HIGH_PRIO_MDL 0x0400

typedef enum
{
  WITHOUT_DB_NAME,
  WITH_DB_NAME
} enum_with_db_name;

#undef WITH_PARTITION_STORAGE_ENGINE

/* Match the values of enum ha_choice */
static const LEX_CSTRING ha_choice_values[]=
        {
                { STRING_WITH_LEN("") },
                { STRING_WITH_LEN("0") },
                { STRING_WITH_LEN("1") }
        };

class Show_create_table
{
protected:
  LEX_CSTRING db;
  ulong option_bits;
  ulong sql_mode;
  int get_quote_char_for_identifier(const char *name, size_t length);
  bool append_identifier(String *packet, const char *name, size_t length);
  bool append_identifier(String *packet, const LEX_CSTRING *name)
  { return append_identifier(packet, name->str, name->length); }

  bool append_at_host(String *buffer, const LEX_CSTRING *host);
  bool append_definer(String *buffer, const LEX_CSTRING *definer_user,
                      const LEX_CSTRING *definer_host);
  void append_create_options(String *packet,
                             engine_option_value *opt,
                             bool check_options,
                             ha_create_table_option *rules);
  void add_table_options(TABLE *table,
                         Table_specification_st *create_info_arg,
                         bool schema_table, bool sequence,
                         String *packet);
  void append_period(THD *thd, String *packet, const LEX_CSTRING &start,
                     const LEX_CSTRING &end, const LEX_CSTRING &period,
                     bool ident);

public:
  Show_create_table(LEX_CSTRING db, ulong options, ulong mode)
  : db(db), option_bits(options), sql_mode(mode) 
  {}
  int do_show(TABLE_LIST *table_list,
              const char *force_db, const char *force_name,
              String *packet, Table_specification_st *create_info_arg,
              enum_with_db_name with_db_name);
};

/*
  Go through all character combinations and ensure that sql_lex.cc can
  parse it as an identifier.

  SYNOPSIS
  require_quotes()
  name			attribute name
  name_length		length of name

  RETURN
    #	Pointer to conflicting character
    0	No conflicting character
*/

static const char *require_quotes(const char *name, uint name_length)
{
  bool pure_digit= TRUE;
  const char *end= name + name_length;

  for (; name < end ; name++)
  {
    uchar chr= (uchar) *name;
    int length= system_charset_info->charlen(name, end);
    if (length == 1 && !system_charset_info->ident_map[chr])
      return name;
    if (length == 1 && (chr < '0' || chr > '9'))
      pure_digit= FALSE;
  }
  if (pure_digit)
    return name;
  return 0;
}

bool is_keyword(const char *name, uint len)
{
  DBUG_ASSERT(len != 0);
  return get_hash_symbol(name,len,0)!=0;
}

int Show_create_table::get_quote_char_for_identifier(const char *name,
                                                     size_t length)
{
  if (length &&
      !is_keyword(name,(uint)length) &&
      !require_quotes(name, (uint)length) &&
      !(option_bits & OPTION_QUOTE_SHOW_CREATE))
    return EOF;
  if (sql_mode & MODE_ANSI_QUOTES)
    return '"';
  return '`';
}
bool Show_create_table::append_identifier(String *packet, const char *name,
                                          size_t length)
{
  const char *name_end;
  char quote_char;
  int q= get_quote_char_for_identifier(name, length);

  if (q == EOF)
    return packet->append(name, length, packet->charset());

  /*
    The identifier must be quoted as it includes a quote character or
    it's a keyword
  */

  /*
    Special code for swe7. It encodes the letter "E WITH ACUTE" on
    the position 0x60, where backtick normally resides.
    In swe7 we cannot append 0x60 using system_charset_info,
    because it cannot be converted to swe7 and will be replaced to
    question mark '?'. Use &my_charset_bin to avoid this.
    It will prevent conversion and will append the backtick as is.
  */
  CHARSET_INFO *quote_charset=
      q == 0x60 && (packet->charset()->state & MY_CS_NONASCII) &&
              packet->charset()->mbmaxlen == 1
          ? &my_charset_bin  //// ?????????
          : system_charset_info;

  (void) packet->reserve(length * 2 + 2);
  quote_char= (char) q;
  if (packet->append(&quote_char, 1, quote_charset))
    return true;

  for (name_end= name + length; name < name_end;)
  {
    uchar chr= (uchar) *name;
    int char_length= system_charset_info->charlen(name, name_end);
    /*
      charlen can return 0 and negative numbers on a wrong multibyte
      sequence. It is possible when upgrading from 4.0,
      and identifier contains some accented characters.
      The manual says it does not work. So we'll just
      change char_length to 1 not to hang in the endless loop.
    */
    if (char_length <= 0)
      char_length= 1;
    if (char_length == 1 && chr == (uchar) quote_char &&
        packet->append(&quote_char, 1, quote_charset))
      return true;
    if (packet->append(name, char_length, system_charset_info))
      return true;
    name+= char_length;
  }
  return packet->append(&quote_char, 1, quote_charset);
}

bool Show_create_table::append_at_host(String *buffer, const LEX_CSTRING *host)
{
  if (!host->str || !host->str[0])
    return false;
  return buffer->append('@') || append_identifier(buffer, host);
}

bool Show_create_table::append_definer(String *buffer, const LEX_CSTRING *definer_user,
                    const LEX_CSTRING *definer_host)
{
  return buffer->append(STRING_WITH_LEN("DEFINER=")) ||
         append_identifier(buffer, definer_user) ||
         append_at_host(buffer, definer_host) || buffer->append(' ');
}

/**
  Appends list of options to string

  @param thd             thread handler
  @param packet          string to append
  @param opt             list of options
  @param check_options   print all used options
  @param rules           list of known options
*/

void Show_create_table::append_create_options(String *packet,
				  engine_option_value *opt,
				  bool check_options,
				  ha_create_table_option *rules)
{
	bool in_comment= false;
	for(; opt; opt= opt->next)
	{
		if (check_options)
		{
			if (is_engine_option_known(opt, rules))
			{
				if (in_comment)
					packet->append(STRING_WITH_LEN(" */"));
				in_comment= false;
			}
			else
			{
				if (!in_comment)
					packet->append(STRING_WITH_LEN(" /*"));
				in_comment= true;
			}
		}

		DBUG_ASSERT(opt->value.str);
		packet->append(' ');
		append_identifier(packet, &opt->name);
		packet->append('=');
		if (opt->quoted_value)
			append_unescaped(packet, opt->value.str, opt->value.length);
		else
			packet->append(&opt->value);
	}
	if (in_comment)
		packet->append(STRING_WITH_LEN(" */"));
}

/**
   Add table options to end of CREATE statement

   @param schema_table  1 if schema table
   @param sequence      1 if sequence. If sequence, we flush out options
                          not relevant for sequences.
*/

void Show_create_table::add_table_options(TABLE *table,
                                          Table_specification_st *create_info_arg,
                                          bool schema_table, bool sequence,
                                          String *packet)
{
	TABLE_SHARE *share= table->s;
	handlerton *hton;
	HA_CREATE_INFO create_info;
	bool check_options= (!(sql_mode & MODE_IGNORE_BAD_TABLE_OPTIONS) &&
			     (!create_info_arg ||
			      create_info_arg->used_fields &
			      HA_CREATE_PRINT_ALL_OPTIONS));

#ifdef WITH_PARTITION_STORAGE_ENGINE
	if (table->part_info)
    hton= table->part_info->default_engine_type;
  else
#endif
	hton= table->file->ht;

	bzero((char*) &create_info, sizeof(create_info));
	/* Allow update_create_info to update row type, page checksums and options */
	create_info.row_type= share->row_type;
	create_info.page_checksum= share->page_checksum;
	create_info.options= share->db_create_options;
	table->file->update_create_info(&create_info);

	/*
	  IF   check_create_info
	  THEN add ENGINE only if it was used when creating the table
	*/
	if (!create_info_arg ||
	    (create_info_arg->used_fields & HA_CREATE_USED_ENGINE))
	{
		LEX_CSTRING *engine_name= table->file->engine_name();

		if (sql_mode & (MODE_MYSQL323 | MODE_MYSQL40))
			packet->append(STRING_WITH_LEN(" TYPE="));
		else
			packet->append(STRING_WITH_LEN(" ENGINE="));

		packet->append(engine_name->str, engine_name->length);
	}

	if (sequence)
		goto end_options;

	/*
	  Add AUTO_INCREMENT=... if there is an AUTO_INCREMENT column,
	  and NEXT_ID > 1 (the default).  We must not print the clause
	  for engines that do not support this as it would break the
	  import of dumps, but as of this writing, the test for whether
	  AUTO_INCREMENT columns are allowed and wether AUTO_INCREMENT=...
	  is supported is identical, !(file->table_flags() & HA_NO_AUTO_INCREMENT))
	  Because of that, we do not explicitly test for the feature,
	  but may extrapolate its existence from that of an AUTO_INCREMENT column.
	*/

	if (create_info.auto_increment_value > 1)
	{
		packet->append(STRING_WITH_LEN(" AUTO_INCREMENT="));
		packet->append_ulonglong(create_info.auto_increment_value);
	}

	if (share->table_charset && !(sql_mode & (MODE_MYSQL323 | MODE_MYSQL40)) &&
	    share->table_type != TABLE_TYPE_SEQUENCE)
	{
		/*
		  IF   check_create_info
		  THEN add DEFAULT CHARSET only if it was used when creating the table
		*/
		if (!create_info_arg ||
		    (create_info_arg->used_fields & HA_CREATE_USED_DEFAULT_CHARSET))
		{
			packet->append(STRING_WITH_LEN(" DEFAULT CHARSET="));
			packet->append(share->table_charset->cs_name);
			if (!(share->table_charset->state & MY_CS_PRIMARY))
			{
				packet->append(STRING_WITH_LEN(" COLLATE="));
				packet->append(table->s->table_charset->coll_name);
			}
		}
	}

	if (share->min_rows)
	{
		packet->append(STRING_WITH_LEN(" MIN_ROWS="));
		packet->append_ulonglong(share->min_rows);
	}

	if (share->max_rows && !schema_table && !sequence)
	{
		packet->append(STRING_WITH_LEN(" MAX_ROWS="));
		packet->append_ulonglong(share->max_rows);
	}

	if (share->avg_row_length)
	{
		packet->append(STRING_WITH_LEN(" AVG_ROW_LENGTH="));
		packet->append_ulonglong(share->avg_row_length);
	}

	if (create_info.options & HA_OPTION_PACK_KEYS)
		packet->append(STRING_WITH_LEN(" PACK_KEYS=1"));
	if (create_info.options & HA_OPTION_NO_PACK_KEYS)
		packet->append(STRING_WITH_LEN(" PACK_KEYS=0"));
	if (share->db_create_options & HA_OPTION_STATS_PERSISTENT)
		packet->append(STRING_WITH_LEN(" STATS_PERSISTENT=1"));
	if (share->db_create_options & HA_OPTION_NO_STATS_PERSISTENT)
		packet->append(STRING_WITH_LEN(" STATS_PERSISTENT=0"));
	if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_ON)
		packet->append(STRING_WITH_LEN(" STATS_AUTO_RECALC=1"));
	else if (share->stats_auto_recalc == HA_STATS_AUTO_RECALC_OFF)
		packet->append(STRING_WITH_LEN(" STATS_AUTO_RECALC=0"));
	if (share->stats_sample_pages != 0)
	{
		packet->append(STRING_WITH_LEN(" STATS_SAMPLE_PAGES="));
		packet->append_ulonglong(share->stats_sample_pages);
	}

	/* We use CHECKSUM, instead of TABLE_CHECKSUM, for backward compatibility */
	if (create_info.options & HA_OPTION_CHECKSUM)
		packet->append(STRING_WITH_LEN(" CHECKSUM=1"));
	if (create_info.page_checksum != HA_CHOICE_UNDEF)
	{
		packet->append(STRING_WITH_LEN(" PAGE_CHECKSUM="));
		packet->append(ha_choice_values[create_info.page_checksum]);
	}
	if (create_info.options & HA_OPTION_DELAY_KEY_WRITE)
		packet->append(STRING_WITH_LEN(" DELAY_KEY_WRITE=1"));
	if (create_info.row_type != ROW_TYPE_DEFAULT)
	{
		packet->append(STRING_WITH_LEN(" ROW_FORMAT="));
		packet->append(&ha_row_type[(uint) create_info.row_type]);
	}
	if (share->transactional != HA_CHOICE_UNDEF)
	{
		packet->append(STRING_WITH_LEN(" TRANSACTIONAL="));
		packet->append(ha_choice_values[(uint) share->transactional]);
	}
	if (share->table_type == TABLE_TYPE_SEQUENCE)
		packet->append(STRING_WITH_LEN(" SEQUENCE=1"));
	if (table->s->key_block_size)
	{
		packet->append(STRING_WITH_LEN(" KEY_BLOCK_SIZE="));
		packet->append_ulonglong(table->s->key_block_size);
	}
	table->file->append_create_info(packet);

	end_options:
	if (share->comment.length)
	{
		packet->append(STRING_WITH_LEN(" COMMENT="));
		append_unescaped(packet, share->comment.str, share->comment.length);
	}
	if (share->connect_string.length)
	{
		packet->append(STRING_WITH_LEN(" CONNECTION="));
		append_unescaped(packet, share->connect_string.str, share->connect_string.length);
	}
	append_create_options(packet, share->option_list, check_options,
			      hton->table_options);
	append_directory(packet, &DATA_clex_str,  create_info.data_file_name);
	append_directory(packet, &INDEX_clex_str, create_info.index_file_name);
}

// TODO: return OOM result
void Show_create_table::append_period(THD *thd, String *packet,
                                      const LEX_CSTRING &start,
                                      const LEX_CSTRING &end,
                                      const LEX_CSTRING &period,
                                      bool ident)
{
	packet->append(STRING_WITH_LEN(",\n  PERIOD FOR "));
	if (ident)
		append_identifier(packet, period.str, period.length);
	else
		packet->append(period);
	packet->append(STRING_WITH_LEN(" ("));
	append_identifier(packet, start.str, start.length);
	packet->append(STRING_WITH_LEN(", "));
	append_identifier(packet, end.str, end.length);
	packet->append(STRING_WITH_LEN(")"));
}


/*
  Build a CREATE TABLE statement for a table.

  SYNOPSIS
    show_create_table()
    thd               The thread
    table_list        A list containing one table to write statement
                      for.
    force_db          If not NULL, database name to use in the CREATE
                      TABLE statement.
    force_name        If not NULL, table name to use in the CREATE TABLE
                      statement. if NULL, the name from table_list will be
                      used.
    packet            Pointer to a string where statement will be
                      written.
    create_info_arg   Pointer to create information that can be used
                      to tailor the format of the statement.  Can be
                      NULL, in which case only SQL_MODE is considered
                      when building the statement.
    with_db_name     Add database name to table name

  NOTE
    Currently always return 0, but might return error code in the
    future.

  RETURN
    0       OK
 */

int Show_create_table::do_show(TABLE_LIST *table_list,
                               const char *force_db, const char *force_name,
                               String *packet,
                               Table_specification_st *create_info_arg,
                               enum_with_db_name with_db_name)
{
	List<Item> field_list;
	char tmp[MAX_FIELD_WIDTH], *for_str, def_value_buf[MAX_FIELD_WIDTH];
	LEX_CSTRING alias;
	String type;
	String def_value;
	Field **ptr,*field;
	uint primary_key;
	KEY *key_info;
	TABLE *table= table_list->table;
	TABLE_SHARE *share= table->s;
	TABLE_SHARE::period_info_t &period= share->period;
	bool explicit_fields= false;
	bool foreign_db_mode=  sql_mode & (MODE_POSTGRESQL | MODE_ORACLE |
					   MODE_MSSQL | MODE_DB2 |
					   MODE_MAXDB | MODE_ANSI);
	bool limited_mysql_mode= sql_mode & (MODE_NO_FIELD_OPTIONS | MODE_MYSQL323 |
					     MODE_MYSQL40);
	bool show_table_options= !(sql_mode & MODE_NO_TABLE_OPTIONS) &&
				 !foreign_db_mode;
	bool check_options= !(sql_mode & MODE_IGNORE_BAD_TABLE_OPTIONS) &&
			    !create_info_arg;
	handlerton *hton;
	int error= 0;
	DBUG_ENTER("show_create_table");
	DBUG_PRINT("enter",("table: %s", table->s->table_name.str));

#ifdef WITH_PARTITION_STORAGE_ENGINE
	if (table->part_info)
		hton= table->part_info->default_engine_type;
	else
#endif
		hton= table->file->ht;

	restore_record(table, s->default_values); // Get empty record

	packet->append(STRING_WITH_LEN("CREATE "));
	if (create_info_arg &&
	    ((create_info_arg->or_replace() &&
	      !create_info_arg->or_replace_slave_generated()) ||
	     create_info_arg->table_was_deleted))
		packet->append(STRING_WITH_LEN("OR REPLACE "));
	if (share->tmp_table)
		packet->append(STRING_WITH_LEN("TEMPORARY "));
	packet->append(STRING_WITH_LEN("TABLE "));
	if (create_info_arg && create_info_arg->if_not_exists())
		packet->append(STRING_WITH_LEN("IF NOT EXISTS "));

	if (force_name)
	{
		if (force_db)
		{
			append_identifier(packet, force_db, strlen(force_db));
			packet->append(STRING_WITH_LEN("."));
		}
		append_identifier(packet, force_name, strlen(force_name));
	}
	else
	{
		if (table_list->schema_table)
		{
			alias.str= table_list->schema_table->table_name;
			alias.length= strlen(alias.str);
		}
		else
		{
			if (lower_case_table_names == 2)
			{
				alias.str= table->alias.c_ptr();
				alias.length= table->alias.length();
			}
			else
				alias= share->table_name;
		}

		/*
		  Print the database before the table name if told to do that. The
		  database name is only printed in the event that it is different
		  from the current database.  The main reason for doing this is to
		  avoid having to update gazillions of tests and result files, but
		  it also saves a few bytes of the binary log.
		 */
		if (with_db_name == WITH_DB_NAME)
		{
			const LEX_CSTRING *const schema_db=
				table_list->schema_table ? &INFORMATION_SCHEMA_NAME : &table->s->db;
			if (!db.str || cmp(schema_db, &db))
			{
				append_identifier(packet, schema_db);
				packet->append(STRING_WITH_LEN("."));
			}
		}

		append_identifier(packet, &alias);
	}

	packet->append(STRING_WITH_LEN(" (\n"));
	/*
	  We need this to get default values from the table
	  We have to restore the read_set if we are called from insert in case
	  of row based replication.
	*/
	MY_BITMAP *old_map= tmp_use_all_columns(table, &table->read_set);

	bool not_the_first_field= false;
	for (ptr=table->field ; (field= *ptr); ptr++)
	{

		uint flags = field->flags;

		if (field->invisible > INVISIBLE_USER)
			continue;
		if (not_the_first_field)
			packet->append(STRING_WITH_LEN(",\n"));

		not_the_first_field= true;
		packet->append(STRING_WITH_LEN("  "));
		append_identifier(packet, &field->field_name);
		packet->append(' ');

		const Type_handler *th= field->type_handler();
		const Schema *implied_schema= Schema::find_implied(sql_mode);
		if (th != implied_schema->map_data_type(th))
		{
			packet->append(th->schema()->name(), system_charset_info);
			packet->append(STRING_WITH_LEN("."), system_charset_info);
		}
		type.set(tmp, sizeof(tmp), system_charset_info);
		field->sql_type(type);
		packet->append(type.ptr(), type.length(), system_charset_info);

		if (field->has_charset() && !(sql_mode & (MODE_MYSQL323 | MODE_MYSQL40)))
		{
			if (field->charset() != share->table_charset)
			{
				packet->append(STRING_WITH_LEN(" CHARACTER SET "));
				packet->append(field->charset()->cs_name);
			}
			/*
			  For string types dump collation name only if
			  collation is not primary for the given charset
		  
			  For generated fields don't print the COLLATE clause if
			  the collation matches the expression's collation.
			*/
			if (!(field->charset()->state & MY_CS_PRIMARY) &&
			    (!field->vcol_info ||
			     field->charset() != field->vcol_info->expr->collation.collation))
			{
				packet->append(STRING_WITH_LEN(" COLLATE "));
				packet->append(field->charset()->coll_name);
			}
		}

		if (field->vcol_info)
		{
			StringBuffer<MAX_FIELD_WIDTH> str(&my_charset_utf8mb4_general_ci);
			field->vcol_info->print(&str);
			packet->append(STRING_WITH_LEN(" GENERATED ALWAYS AS ("));
			packet->append(str);
			packet->append(STRING_WITH_LEN(")"));
			if (field->vcol_info->stored_in_db)
				packet->append(STRING_WITH_LEN(" STORED"));
			else
				packet->append(STRING_WITH_LEN(" VIRTUAL"));
			if (field->invisible == INVISIBLE_USER)
			{
				packet->append(STRING_WITH_LEN(" INVISIBLE"));
			}
		}
		else
		{
			if (field->flags & VERS_ROW_START)
			{
				packet->append(STRING_WITH_LEN(" GENERATED ALWAYS AS ROW START"));
			}
			else if (field->flags & VERS_ROW_END)
			{
				packet->append(STRING_WITH_LEN(" GENERATED ALWAYS AS ROW END"));
			}
			else if (flags & NOT_NULL_FLAG)
				packet->append(STRING_WITH_LEN(" NOT NULL"));
			else if (field->type() == MYSQL_TYPE_TIMESTAMP)
			{
				/*
				  TIMESTAMP field require explicit NULL flag, because unlike
				  all other fields they are treated as NOT NULL by default.
				*/
				packet->append(STRING_WITH_LEN(" NULL"));
			}

			if (field->invisible == INVISIBLE_USER)
			{
				packet->append(STRING_WITH_LEN(" INVISIBLE"));
			}
			def_value.set(def_value_buf, sizeof(def_value_buf), system_charset_info);
			if (get_field_default_value(field, &def_value, 1))
			{
				packet->append(STRING_WITH_LEN(" DEFAULT "));
				packet->append(def_value.ptr(), def_value.length(), system_charset_info);
			}

			if (field->vers_update_unversioned())
			{
				packet->append(STRING_WITH_LEN(" WITHOUT SYSTEM VERSIONING"));
			}

			if (!limited_mysql_mode &&
			    print_on_update_clause(field, &def_value, false))
			{
				packet->append(STRING_WITH_LEN(" "));
				packet->append(def_value);
			}

			if (field->unireg_check == Field::NEXT_NUMBER &&
			    !(sql_mode & MODE_NO_FIELD_OPTIONS))
				packet->append(STRING_WITH_LEN(" AUTO_INCREMENT"));
		}

		if (field->comment.length)
		{
			packet->append(STRING_WITH_LEN(" COMMENT "));
			append_unescaped(packet, field->comment.str, field->comment.length);
		}

		append_create_options(thd, packet, field->option_list, check_options,
				      hton->field_options);

		if (field->check_constraint)
		{
			StringBuffer<MAX_FIELD_WIDTH> str(&my_charset_utf8mb4_general_ci);
			field->check_constraint->print(&str);
			packet->append(STRING_WITH_LEN(" CHECK ("));
			packet->append(str);
			packet->append(STRING_WITH_LEN(")"));
		}

	}

	if (period.name)
	{
		append_period(thd, packet,
			      period.start_field(share)->field_name,
			      period.end_field(share)->field_name,
			      period.name, true);
	}

	key_info= table->s->key_info;
	primary_key= share->primary_key;

	for (uint i=0 ; i < share->keys ; i++,key_info++)
	{
		if (key_info->flags & HA_INVISIBLE_KEY)
			continue;
		KEY_PART_INFO *key_part= key_info->key_part;
		bool found_primary=0;
		packet->append(STRING_WITH_LEN(",\n  "));

		if (i == primary_key && !strcmp(key_info->name.str, primary_key_name.str))
		{
			found_primary=1;
			/*
			  No space at end, because a space will be added after where the
			  identifier would go, but that is not added for primary key.
			*/
			packet->append(STRING_WITH_LEN("PRIMARY KEY"));
		}
		else if (key_info->flags & HA_NOSAME)
			packet->append(STRING_WITH_LEN("UNIQUE KEY "));
		else if (key_info->flags & HA_FULLTEXT)
			packet->append(STRING_WITH_LEN("FULLTEXT KEY "));
		else if (key_info->flags & HA_SPATIAL)
			packet->append(STRING_WITH_LEN("SPATIAL KEY "));
		else
			packet->append(STRING_WITH_LEN("KEY "));

		if (!found_primary)
			append_identifier(thd, packet, &key_info->name);

		packet->append(STRING_WITH_LEN(" ("));

		uint key_parts= key_info->user_defined_key_parts;
		if (key_info->without_overlaps)
			key_parts-= 2;

		for (uint j=0 ; j < key_parts ; j++,key_part++)
		{
			Field *field= key_part->field;
			if (field->invisible > INVISIBLE_USER)
				continue;

			if (j)
				packet->append(',');

			if (key_part->field)
				append_identifier(thd, packet, &key_part->field->field_name);
			if (key_part->field &&
			    (key_part->length !=
			     table->field[key_part->fieldnr-1]->key_length() &&
			     !(key_info->flags & (HA_FULLTEXT | HA_SPATIAL))))
			{
				packet->append_parenthesized((long) key_part->length /
							     key_part->field->charset()->mbmaxlen);
			}
			if (table->file->index_flags(i, j, 0) & HA_READ_ORDER &&
			    key_part->key_part_flag & HA_REVERSE_SORT) /* same in SHOW KEYS */
				packet->append(STRING_WITH_LEN(" DESC"));
		}

		if (key_info->without_overlaps)
		{
			packet->append(',');
			append_identifier(thd, packet, &share->period.name);
			packet->append(STRING_WITH_LEN(" WITHOUT OVERLAPS"));
		}

		packet->append(')');
		store_key_options(thd, packet, table, &table->key_info[i]);
		if (key_info->parser)
		{
			LEX_CSTRING *parser_name= plugin_name(key_info->parser);
			packet->append(STRING_WITH_LEN(" /*!50100 WITH PARSER "));
			append_identifier(thd, packet, parser_name);
			packet->append(STRING_WITH_LEN(" */ "));
		}
		append_create_options(thd, packet, key_info->option_list, check_options,
				      hton->index_options);
	}

	if (table->versioned())
	{
		const Field *fs = table->vers_start_field();
		const Field *fe = table->vers_end_field();
		DBUG_ASSERT(fs);
		DBUG_ASSERT(fe);
		explicit_fields= fs->invisible < INVISIBLE_SYSTEM;
		DBUG_ASSERT(!explicit_fields || fe->invisible < INVISIBLE_SYSTEM);
		if (explicit_fields)
		{
			append_period(thd, packet, fs->field_name, fe->field_name,
				      table->s->vers.name, false);
		}
		else
		{
			DBUG_ASSERT(fs->invisible == INVISIBLE_SYSTEM);
			DBUG_ASSERT(fe->invisible == INVISIBLE_SYSTEM);
		}
	}

	/*
	  Get possible foreign key definitions stored in InnoDB and append them
	  to the CREATE TABLE statement
	*/

	if ((for_str= table->file->get_foreign_key_create_info()))
	{
		packet->append(for_str, strlen(for_str));
		table->file->free_foreign_key_create_info(for_str);
	}

	/* Add table level check constraints */
	if (share->table_check_constraints)
	{
		StringBuffer<MAX_FIELD_WIDTH> str(&my_charset_utf8mb4_general_ci);
		for (uint i= share->field_check_constraints;
		     i < share->table_check_constraints ; i++)
		{
			Virtual_column_info *check= table->check_constraints[i];
			// period constraint is implicit
			if (share->period.constr_name.streq(check->name))
				continue;

			str.set_buffer_if_not_allocated(&my_charset_utf8mb4_general_ci);
			str.length(0);                            // Print appends to str
			check->print(&str);

			packet->append(STRING_WITH_LEN(",\n  "));
			if (check->name.str)
			{
				packet->append(STRING_WITH_LEN("CONSTRAINT "));
				append_identifier(thd, packet, &check->name);
			}
			packet->append(STRING_WITH_LEN(" CHECK ("));
			packet->append(str);
			packet->append(STRING_WITH_LEN(")"));
		}
	}

	packet->append(STRING_WITH_LEN("\n)"));
	if (show_table_options)
		add_table_options(thd, table, create_info_arg,
				  table_list->schema_table != 0, 0, packet);

	if (table->versioned())
		packet->append(STRING_WITH_LEN(" WITH SYSTEM VERSIONING"));

#ifdef WITH_PARTITION_STORAGE_ENGINE
	{
		if (table->part_info &&
		    !((table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION) &&
		      table->part_info->is_auto_partitioned))
		{
			/*
			  Partition syntax for CREATE TABLE is at the end of the syntax.
			*/
			uint part_syntax_len;
			char *part_syntax;
			if ((part_syntax= generate_partition_syntax(thd, table->part_info,
								    &part_syntax_len,
								    show_table_options,
								    NULL, NULL)))
			{
				packet->append('\n');
				if (packet->append(part_syntax, part_syntax_len))
					error= 1;
			}
		}
	}
#endif
	tmp_restore_column_map(&table->read_set, old_map);
	DBUG_RETURN(error);
}

int show_create_table(TABLE_LIST *table_list, String *packet,
                      Table_specification_st *create_info_arg,
                      enum_with_db_name with_db_name, LEX_CSTRING db,
                      ulong option_bits, ulong sql_mode)
{
  Show_create_table show_create(db, option_bits, sql_mode);
  return show_create.do_show(table_list, NULL, NULL, packet,
                             create_info_arg, with_db_name);
}
