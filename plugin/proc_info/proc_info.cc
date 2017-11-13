#include <my_global.h>
#include <table.h>
#include <sql_show.h>
#include <sql_class.h>
#include <mysql/plugin.h>

/* We'll use this to read directories */
#include <dirent.h>


static ST_FIELD_INFO meminfo_fields[]=
{
  { "NAME", 100, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE },
  { "VALUE", 21, MYSQL_TYPE_LONG, 0, MY_I_S_UNSIGNED, 0, SKIP_OPEN_TABLE },
  { 0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0 }
};

static ST_FIELD_INFO fdinfo_fields[]=
{
	/* This can be number if we want to */
  { "NAME", 100, MYSQL_TYPE_STRING, 0, 0, 0, SKIP_OPEN_TABLE },
  { "VALUE", FN_REFLEN, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, 0, SKIP_OPEN_TABLE },
  { 0, 0, MYSQL_TYPE_STRING, 0, 0, 0, 0 }
};



static int meminfo_fill(MYSQL_THD thd, TABLE_LIST *tables, COND *cond)
{
  TABLE *table= tables->table;
  char name[1024];
  unsigned long value;
  FILE *fp;
  int res;


  /* Here we open /proc/meminfo to get info */
  if (!(fp= fopen("/proc/meminfo", "r")))
    return 1;

  /* Read it in a loop, storing name and value */
  while ((res= fscanf(fp, "%[^:]: %lu kB\n", name, &value)) != EOF)
  {
    if (res != 2)
      continue;
    table->field[0]->store(name, strlen(name), system_charset_info);
    table->field[1]->store(value);
    /* table->record is filled by field[x]->store() */
    if (schema_table_store_record(thd, table))
    {
      fclose(fp);
      return 1;
    }
  }
  fclose(fp);

  return 0;
}


static int meminfo_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *) p;

  schema->fields_info= meminfo_fields;
  schema->fill_table= meminfo_fill;

  return 0;
}


static struct st_mysql_information_schema meminfo_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

static struct st_mysql_information_schema fdinfo_plugin=
{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };

static int fdinfo_fill(MYSQL_THD thd, TABLE_LIST *tables, COND *cond)
{
	int fd;

	DIR *dir; /* from dirent.h */
	struct dirent dirbuf, *result;
	TABLE *table= tables->table;

	char final_result[FN_REFLEN + 1];
	int error= 0;

	if ((fd= open("/proc/self/fd", O_DIRECTORY)) < 0)
		return 1;

	if ((dir= fdopendir(fd)) == NULL)
	{
		close(fd);
		return 1;
	}

	while (readdir_r(dir, &dirbuf, &result) == 0 && result)
	{
		
		if (result->d_name[0] == '.' /* for marko we'll do it later TODO */)
			continue;
		/* ASK  QUESTIONS IF THIS IS NOT CLEAR!! */
		ssize_t len;
		len= readlinkat(fd, result->d_name, final_result, sizeof(final_result)); 

		table->field[0]->store(result->d_name, strlen(result->d_name), system_charset_info);
		if (len < 0)
		{
			table->field[1]->set_null();
		}
		else
		{
			table->field[1]->store(final_result, len, system_charset_info);
			table->field[1]->set_notnull(); /* Sets not-null bit */
		}
    if (schema_table_store_record(thd, table))
    {
			error= 1;
			break; /* Error, Crash and burn?! */
			/* clang-format  We should use this! */
    }
	}

	close(fd);
	closedir(dir);

	return error;
}


static int fdinfo_init(void *p)
{
  ST_SCHEMA_TABLE *schema= (ST_SCHEMA_TABLE *) p;

  schema->fields_info= fdinfo_fields;
  schema->fill_table= fdinfo_fill;

  return 0;
}




maria_declare_plugin(proc_info)
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN, /* type                */
  &meminfo_plugin,                 /* information schema  */
  "PROC_MEMINFO",                  /* name                */
  ""      ,                        /* author              */
  "Useful information from /proc", /* description         */
  PLUGIN_LICENSE_GPL,              /* license             */
  meminfo_init,                    /* init callback       */
  0,                               /* deinit callback     */
  0x0100,                          /* version as hex      */
  NULL,                            /* status variables    */
  NULL,                            /* system variables    */
  "1.0",                           /* version as a string */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},

{
  MYSQL_INFORMATION_SCHEMA_PLUGIN, /* type                */
  &fdinfo_plugin,                  /* information schema  */
  "PROC_FDINFO",                   /* name                */
  ""      ,                        /* author              */
  "Useful information from /proc", /* description         */
  PLUGIN_LICENSE_GPL,              /* license             */
  fdinfo_init,                     /* init callback       */
  0,                               /* deinit callback     */
  0x0100,                          /* version as hex      */
  NULL,                            /* status variables    */
  NULL,                            /* system variables    */
  "1.0",                           /* version as a string */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
