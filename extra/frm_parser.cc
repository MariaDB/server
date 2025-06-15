/* Copyright (c) 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA */

/**
  @file extra/frm_parser.cc
  FRM file parser utility - extracts table structure from .frm files
*/

#include "mariadb.h"

#include "mysqld.h"
#include "sql_class.h"
#include "table.h"
#include "sql_table.h"
#include "sql_parse.h"
#include <my_dir.h>
#include "field.h"


extern "C" {
  // Stub implementations for plugin system
  struct st_maria_plugin *mysql_mandatory_plugins[] = { NULL };
  struct st_maria_plugin *mysql_optional_plugins[] = { NULL };
}


/**
  Initialize THD for FRM parsing
*/
static THD* init_thd()
{
  printf("DEBUG: Entering init_thd\n");
  fflush(stdout);


  THD *thd= new THD(0);
  printf("DEBUG: THD created\n");
  fflush(stdout);
  
  if (!thd)
    return NULL;
    
  thd->thread_stack= (char*) &thd;
  printf("DEBUG: thread_stack set\n");
  fflush(stdout);
  
  thd->store_globals();
  printf("DEBUG: store_globals called\n");
  fflush(stdout);
  
  // Initialize basic THD settings needed for FRM parsing
  thd->variables.character_set_client= &my_charset_utf8mb4_general_ci;
  thd->variables.collation_connection= &my_charset_utf8mb4_general_ci;
  thd->variables.collation_database= &my_charset_utf8mb4_general_ci;
  thd->variables.character_set_results= &my_charset_utf8mb4_general_ci;
  printf("DEBUG: Character sets initialized\n");
  fflush(stdout);
  
  thd->security_ctx->master_access= ALL_KNOWN_ACL;
  printf("DEBUG: Security context set\n");
  fflush(stdout);
  
  printf("DEBUG: init_thd completed successfully\n");
  fflush(stdout);
  return thd;
}

/**
  Cleanup THD
*/
static void cleanup_thd(THD *thd)
{
  if (thd)
  {
    delete thd;
  }
}

/**
  Read FRM file into memory
*/
static uchar* read_frm_file(const char *filename, size_t *length)
{
  File file;
  MY_STAT stat_info;
  uchar *buffer= NULL;
  
  if (my_stat(filename, &stat_info, MYF(0)) == NULL)
  {
    fprintf(stderr, "Error: Cannot stat file '%s': %s\n", 
            filename, strerror(errno));
    return NULL;
  }
  
  *length= stat_info.st_size;
  
  if (!(buffer= (uchar*)my_malloc(PSI_NOT_INSTRUMENTED, *length, MYF(MY_WME))))
  {
    fprintf(stderr, "Error: Cannot allocate memory for FRM file\n");
    return NULL;
  }
  
  if ((file= mysql_file_open(key_file_frm, filename, O_RDONLY | O_SHARE, MYF(0))) < 0)
  {
    fprintf(stderr, "Error: Cannot open file '%s': %s\n", 
            filename, strerror(errno));
    my_free(buffer);
    return NULL;
  }
  
  if (mysql_file_read(file, buffer, *length, MYF(MY_NABP)))
  {
    fprintf(stderr, "Error: Cannot read file '%s': %s\n", 
            filename, strerror(errno));
    my_free(buffer);
    mysql_file_close(file, MYF(0));
    return NULL;
  }
  
  mysql_file_close(file, MYF(0));
  return buffer;
}

/**
  Extract database and table name from FRM file path
*/
static bool extract_db_table_names(const char *frm_path, 
                                   LEX_CSTRING *db_name, 
                                   LEX_CSTRING *table_name)
{
  char *path_copy, *db_start, *table_start, *frm_ext;
  
  if (!((path_copy= my_strdup(PSI_NOT_INSTRUMENTED, frm_path, MYF(MY_WME)))))
    return true;
  
  if (!((frm_ext= strstr(path_copy, ".frm"))))
  {
    my_free(path_copy);
    return true;
  }
  *frm_ext= '\0';
  
  table_start= strrchr(path_copy, '/');
  if (!table_start)
    table_start= strrchr(path_copy, '\\');
  
  if (!table_start)
  {
    // No path separator found, assume it's just a filename
    table_name->str= my_strdup(PSI_NOT_INSTRUMENTED, path_copy, MYF(MY_WME));
    table_name->length= strlen(table_name->str);
    db_name->str= my_strdup(PSI_NOT_INSTRUMENTED, "test", MYF(MY_WME));
    db_name->length= 4;
    my_free(path_copy);
    return false;
  }
  
  *table_start= '\0';
  table_start++;
  
  // Find the database name (second to last component)
  db_start= strrchr(path_copy, '/');
  if (!db_start)
    db_start= strrchr(path_copy, '\\');
  
  if (!db_start)
    db_start= path_copy;
  else
    db_start++;
  
  table_name->str= my_strdup(PSI_NOT_INSTRUMENTED, table_start, MYF(MY_WME));
  table_name->length= strlen(table_name->str);
  db_name->str= my_strdup(PSI_NOT_INSTRUMENTED, db_start, MYF(MY_WME));
  db_name->length= strlen(db_name->str);
  
  my_free(path_copy);
  return false;
}

/**
  Parse FRM file and create TABLE_SHARE and TABLE structures
*/
static bool parse_frm_file(THD *thd, const char *frm_path)
{
  printf("DEBUG: Entering parse_frm_file\n");
  fflush(stdout);
  
  TABLE_LIST table_list;
  uchar *frm_data= NULL;
  size_t frm_length= 0;
  TABLE_SHARE *share= NULL;
  TABLE *table= NULL;
  LEX_CSTRING db_name, table_name;
  bool error= true;
  
  memset(&table_list, 0, sizeof(table_list));
  printf("DEBUG: table_list initialized\n");
  fflush(stdout);

  // Read FRM file into memory
  printf("DEBUG: About to read FRM file: %s\n", frm_path);
  fflush(stdout);
  
  frm_data= read_frm_file(frm_path, &frm_length);
  if (!frm_data)
    goto cleanup;
  
  printf("DEBUG: FRM file read successfully, size: %zu bytes\n", frm_length);
  fflush(stdout);

  printf("DEBUG: Extracting database and table names\n");
  fflush(stdout);
  
  if (extract_db_table_names(frm_path, &db_name, &table_name))
  {
    fprintf(stderr, "Error: Cannot extract database and table names from path\n");
    goto cleanup;
  }
  
  printf("DEBUG: Names extracted - db: %.*s, table: %.*s\n", 
         (int)db_name.length, db_name.str, (int)table_name.length, table_name.str);
  fflush(stdout);

  // Create TABLE_SHARE and initialize it
  printf("DEBUG: Allocating TABLE_SHARE\n");
  fflush(stdout);
  
  share= alloc_table_share(db_name.str, table_name.str, "", 0);
  if (!share)
  {
    fprintf(stderr, "Error: Cannot allocate TABLE_SHARE\n");
    goto cleanup;
  }
  
  printf("DEBUG: TABLE_SHARE allocated successfully\n");
  fflush(stdout);
  
  // Initialize TABLE_SHARE from binary FRM image
  printf("DEBUG: About to call init_from_binary_frm_image\n");
  fflush(stdout);
  
  if (share->init_from_binary_frm_image(thd, false, frm_data, frm_length))
  {
    fprintf(stderr, "Error: Cannot parse FRM file - init_from_binary_frm_image failed\n");
    goto cleanup;
  }
  
  printf("DEBUG: init_from_binary_frm_image completed successfully\n");
  fflush(stdout);
  

  // Create TABLE structure
  table= static_cast<TABLE *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(TABLE), MYF(MY_ZEROFILL)));
  if (!table)
  {
    fprintf(stderr, "Error: Cannot allocate TABLE structure\n");
    goto cleanup;
  }
  
  // Initialize TABLE structure
  table->s= share;
  table->in_use= thd;
  
  // Call TABLE::init to complete initialization
  // Note: We use a dummy TABLE_LIST for this
  table_list.table_name= Lex_ident_table(table_name);
  table_list.db= Lex_ident_db(db_name);
  table_list.alias= Lex_ident_table(table_name);
  
  table->init(thd, &table_list);
  
  // Generate and output CREATE TABLE statement
  printf("Table: %.*s.%.*s\n", (int)db_name.length, db_name.str, (int)table_name.length, table_name.str);
  printf("Fields: %d\n", share->fields);
  printf("Keys: %d\n", share->keys);
  
  error= false;
  
cleanup:
  if (table)
  {
    my_free(table);
  }
  if (share)
  {
    free_table_share(share);
  }
  if (frm_data)
  {
    my_free(frm_data);
  }
  if (db_name.str)
  {
    my_free((void*)db_name.str);
  }
  if (table_name.str)
  {
    my_free((void*)table_name.str);
  }
  
  return error;
}

/**
  Main function
*/
int main(int argc, char **argv)
{
  printf("DEBUG: Starting frm_parser...\n");
  fflush(stdout);
  
  MY_INIT(argv[0]);
  printf("DEBUG: MY_INIT completed\n");
  fflush(stdout);
  
  THD *thd= NULL;
  int exit_code= 0;

  // Check if FRM file was specified as argument
  if (argc < 2)
  {
    fprintf(stderr, "Usage: %s <frm_file>\n", argv[0]);
    return 1;
  }

  printf("DEBUG: Arguments validated, FRM file: %s\n", argv[1]);
  fflush(stdout);

  printf("DEBUG: About to initialize THD...\n");
  fflush(stdout);

  // Initialize THD (Thread Descriptor)
  my_thread_init();
  thd= init_thd();
  if (!thd)
  {
    fprintf(stderr, "Error: Cannot initialize THD\n");
    exit_code= 1;
    goto exit;
  }
  
  printf("DEBUG: THD initialized successfully, about to parse FRM file...\n");
  fflush(stdout);

  // Parse the FRM file
  if (parse_frm_file(thd, argv[1]))
  {
    exit_code= 1;
  }
  
  printf("DEBUG: FRM parsing completed\n");
  fflush(stdout);

  exit:
    // Cleanup
    cleanup_thd(thd);
  my_thread_end();
  my_end(0);
  return exit_code;
}
