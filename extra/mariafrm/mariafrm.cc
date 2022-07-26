
#define MARIAFRM_VERSION "1.0"

#include <my_global.h>
#include <my_sys.h>
#include <my_base.h>
#include <m_ctype.h>
#include <m_string.h>
#include <unireg.h>
#include <handler.h>
#include <mysql_com.h>
#include <strfunc.h>

#include <sql_type.h>
#include <field.h>
#include <table.h>

#include "mariafrm.h"

int read_file(const char *path,const uchar **frm, size_t *len)
{
  File file;
  uchar *read_data;
  size_t read_len= 0;
  *frm= NULL;
  *len= 0;
  int error= 1;
  MY_STAT stat;
  if (!my_stat(path, &stat, MYF(MY_WME)))
    goto err_end;
  read_len= stat.st_size;
  error= 2;
  if (!MY_S_ISREG(stat.st_mode))
    goto err_end;
  error= 3;
  if ((file= my_open(path, O_RDONLY, MYF(MY_WME))) < 0)
    goto err_end;
  error= 4;
  if (!(read_data=
            (uchar *) my_malloc(PSI_NOT_INSTRUMENTED, read_len, MYF(MY_FAE))))
    goto err;
  error= 5;
  if (my_read(file, read_data, read_len, MYF(MY_NABP)))
  {
    my_free(read_data);
    goto err;
  }
  error= 0;
  *frm= read_data;
  *len= read_len;
 err:
  my_close(file, MYF(MY_WME));
 err_end:
  return error;
}

int get_tablename(const char* filename, char** tablename, size_t *tablename_len)
{
  const char *basename= my_basename(filename);
  int i= 0;
  while (basename[i] != '\0' && basename[i] != '.')
    i++;
  char *ts= (char *) my_safe_alloca(i);
  memcpy(ts, basename, i);
  char *name_buff;
  CHARSET_INFO *system_charset_info= &my_charset_utf8mb3_general_ci;
  uint errors;
  size_t res;
  int error= 1;
  if (!(name_buff=
            (char *) my_malloc(PSI_NOT_INSTRUMENTED, FN_LEN, MYF(MY_FAE))))
    goto err_end;
  error= 2;
  res= strconvert(&my_charset_filename, ts, i,
                  system_charset_info, name_buff, FN_LEN, &errors);
  if (unlikely(errors))
    goto err_end;
  error= 0;
  *tablename= name_buff;
  *tablename_len= res;
err_end:
  my_safe_afree(ts,i);
  return error;
}

int get_charset(frm_file_data *ffd, uint cs_number)
{
  cs_number= 5;
  CHARSET_INFO *c= get_charset(cs_number, MYF(0));
  ffd->table_cs_name= c->cs_name;
  ffd->table_coll_name= c->coll_name;
  ffd->charset_primary_number= c->primary_number;
  return 0;
}

int parse(frm_file_data *ffd, const uchar *frm, size_t len)
{
  size_t current_pos, end;
  size_t t, comment_pos; //, extra_info_pos;
  ffd->connect_string= {NULL, 0};
  ffd->engine_name= {NULL, 0};
  ffd->magic_number= uint2korr(frm);

  ffd->mysql_version= uint4korr(frm + 51);
  ffd->keyinfo_offset= uint2korr(frm + 6);
  ffd->keyinfo_length= uint2korr(frm + 14);
  if (ffd->keyinfo_length == 65535)
    ffd->keyinfo_length= uint4korr(frm + 47);
  ffd->defaults_offset= ffd->keyinfo_offset + ffd->keyinfo_length;
  ffd->defaults_length= uint2korr(frm + 16);

  ffd->extrainfo_offset= ffd->defaults_offset + ffd->defaults_length;
  ffd->extrainfo_length= uint2korr(frm + 55);

  ffd->names_length= uint2korr(frm + 4);
  ffd->forminfo_offset= uint4korr(frm + FRM_HEADER_SIZE + ffd->names_length);

  ffd->screens_length= uint2korr(frm + ffd->forminfo_offset + 260);

  ffd->null_fields= uint2korr(frm + ffd->forminfo_offset + 282);
  ffd->column_count= uint2korr(frm + ffd->forminfo_offset + 258);
  ffd->names_length= uint2korr(frm + ffd->forminfo_offset + 268);
  ffd->labels_length= uint2korr(frm + ffd->forminfo_offset + 274);
  ffd->comments_length= uint2korr(frm + ffd->forminfo_offset + 284);
  ffd->metadata_offset=
      ffd->forminfo_offset + FRM_FORMINFO_SIZE + ffd->screens_length;
  ffd->metadata_length=
      17 * ffd->column_count; // 17 bytes of metadata per column

  ffd->table_charset= frm[38];
  if (get_charset(ffd, ffd->table_charset))
    goto err;
  ffd->min_rows= uint4korr(frm + 22);
  ffd->max_rows= uint4korr(frm + 18);
  ffd->avg_row_length= uint4korr(frm + 34);
  ffd->row_format= frm[40];
  ffd->rtype= (enum row_type)(uint) ffd->row_format;
  ffd->key_block_size= uint2korr(frm + 62);
  ffd->handler_option= uint2korr(frm + 30);

  if (ffd->extrainfo_length)
  {
    current_pos= ffd->extrainfo_offset;
    end= current_pos + ffd->extrainfo_length;
    ffd->connect_string.length= uint2korr(frm + current_pos);
    current_pos+= 2;
    ffd->connection= c_malloc(ffd->connect_string.length);
    memcpy(ffd->connection, frm + current_pos, ffd->connect_string.length);
    ffd->connect_string.str= ffd->connection;
    current_pos+= ffd->connect_string.length;
    if (current_pos + 2 < end)
    {
      ffd->engine_name.length= uint2korr(frm + current_pos);
      current_pos+= 2;
      ffd->engine_name.str= (char *) (frm + current_pos);
      current_pos+= ffd->engine_name.length;
    }
    if (current_pos + 5 < end)
    {
      ffd->partition_info_str_len= uint4korr(frm + current_pos);
      current_pos+= 4;
      ffd->partition_info_str= c_malloc(ffd->partition_info_str_len + 1);
      memcpy(ffd->partition_info_str, frm + current_pos,
             ffd->partition_info_str_len + 1);
      current_pos+= (ffd->partition_info_str_len + 1);
    }
    //extra_info_pos= current_pos;
  }
  ffd->legacy_db_type_1= (enum legacy_db_type)(uint) frm[3];
  ffd->legacy_db_type_2= (enum legacy_db_type)(uint) frm[61];
  //---READ COLUMN NAMES---
  ffd->columns= new column[ffd->column_count];
  current_pos= ffd->metadata_offset + ffd->metadata_length;
  end= current_pos + ffd->names_length;
  current_pos+= 1;
  for (uint i= 0; i < ffd->column_count; i++)
  {
    size_t start= current_pos;
    while (frm[current_pos++] != 255)
      ;
    size_t len= current_pos - start;
    ffd->columns[i].name.length= len;
    char *ts= c_malloc(len + 1);
    memcpy(ts, frm + start, len - 1);
    ts[len-1]= '\0';
    ffd->columns[i].name.str= ts;
  }
  //---READ LABEL INFORMATION---
  current_pos= end;
  end= current_pos + ffd->labels_length;
  ffd->labels= new label[ffd->column_count];
  
  current_pos+= 1;
  for (uint i=0;current_pos<end;)
  {
    size_t start= current_pos;
    while (frm[current_pos++] != 255)
      ;
    size_t len= current_pos - start;
    char *ts= c_malloc(len + 1);
    memcpy(ts, frm + start, len - 1);
    ts[len-1]= '\0';
    ffd->labels[i].names.push_back({ts, len});
    if (frm[current_pos] == 0)
    {
      i+=1;
      current_pos+= 2;
    } 
  }
  //---READ MORE COLUMN INFO---
  current_pos= ffd->metadata_offset;
  end= current_pos + ffd->metadata_length;
  for (uint i= 0; i < ffd->column_count; i++)
  {
    ffd->columns[i].length= uint2korr(frm + current_pos + 3);
    ffd->columns[i].flags= uint2korr(frm + current_pos + 8);
    ffd->columns[i].unireg_check= (uint) frm[current_pos + 10];
    ffd->columns[i].type= (enum enum_field_types)(uint) frm[current_pos + 13];
    ffd->columns[i].comment.length= uint2korr(frm + current_pos + 15);
    ffd->columns[i].charset_id=
        (frm[current_pos + 11] << 8) + frm[current_pos + 14];
    if (ffd->columns[i].charset_id == MYSQL_TYPE_GEOMETRY)
      ffd->columns[i].charset_id= 63;
    ffd->columns[i].defaults_offset= uint3korr(frm + current_pos + 5);
    ffd->columns[i].label_id= (uint) frm[current_pos + 12] - 1;
    current_pos+= 17;
  }
  //---READ DEFAULTS---
  ffd->null_bit= 1;
  if (ffd->handler_option & HA_PACK_RECORD)
    ffd->null_bit= 0;
  current_pos= ffd->defaults_offset;
  end= current_pos + ffd->defaults_length;
  for (uint i=0;i< ffd->column_count;i++)
  {
    bool auto_increment= ffd->columns[i].unireg_check == 15;
    if (f_no_default(ffd->columns[i].flags) || auto_increment)
    {
      ffd->columns[i].default_value= {NULL, 0};
      continue;
    }
    if (f_maybe_null(ffd->columns[i].flags))
    {
      uint ofst= ffd->null_bit / 8;
      uint null_byte= frm[current_pos + ofst];
      uint null_bit= ffd->null_bit % 8;
      ffd->null_bit++;
      if (null_byte & (1 << null_bit) && ffd->columns[i].unireg_check != 20)
        ffd->columns[i].default_value= {"NULL", 4};
    }
  }
  //---READ KEY INFORMATION---
  current_pos= ffd->keyinfo_offset;
  end= current_pos + ffd->keyinfo_length;
  ffd->key_count= frm[current_pos++];
  if (ffd->key_count < 128)
  {
    ffd->key_parts_count= frm[current_pos++];
  }
  else
  {
    ffd->key_count= (ffd->key_count & 0x7f) | (frm[current_pos++] << 7);
    ffd->key_parts_count= uint2korr(frm + current_pos);
  }
  current_pos+= 2;
  ffd->key_extra_length= uint2korr(frm + current_pos);
  current_pos+= 2;
  ffd->key_extra_info_offset= (uint)(current_pos + ffd->key_count * BYTES_PER_KEY +
                              ffd->key_parts_count * BYTES_PER_KEY_PART);
  ffd->keys= new key[ffd->key_count];
  t= current_pos;
  current_pos= ffd->key_extra_info_offset;
  end= current_pos + ffd->keyinfo_length;
  current_pos+= 1;
  for (uint i= 0; i < ffd->key_count; i++)
  {
    size_t start= current_pos;
    while (frm[current_pos++] != 255)
      ;
    size_t len= current_pos - start;
    ffd->keys[i].name.length= len-1;
    char *ts= c_malloc(len);
    memcpy(ts, frm + start, len-1);
    //ts[len-1]= '\0';
    ffd->keys[i].name.str= ts;
  }
  ffd->key_comment_offset= (uint)current_pos;
  current_pos= t;
  comment_pos= ffd->key_comment_offset;
  for (uint i= 0; i < ffd->key_count; i++)
  {
    ffd->keys[i].flags= uint2korr(frm + current_pos) ^ HA_NOSAME;
    current_pos+= 2;
    //uint2korr(frm + current_pos); // length, not used
    ffd->keys[i].parts_count= frm[current_pos++];
    ffd->keys[i].algorithm= (enum ha_key_alg)(uint) frm[current_pos++];
    ffd->keys[i].key_block_size= uint2korr(frm + current_pos);
    current_pos+= 2;
    if (ffd->keys[i].flags & HA_USES_COMMENT)
    {
      ffd->keys[i].comment.length= uint2korr(frm + comment_pos);
      comment_pos+= 2;
      char *ts= c_malloc(ffd->keys[i].comment.length);
      memcpy(ts, frm + comment_pos, ffd->keys[i].comment.length);
      ffd->keys[i].comment.str= ts;
      comment_pos+= ffd->keys[i].comment.length;
    }
    if (ffd->keys[i].flags & HA_USES_PARSER)
    {
      // read parser information
    }
    ffd->keys[i].field_number= uint2korr(frm + current_pos) & 0x3fff;
    current_pos+= 7;
    ffd->keys[i].length= uint2korr(frm + current_pos);
    current_pos+= 2;
    ffd->keys[i].is_unique= ffd->keys[i].flags & HA_NOSAME;
  }
  return 0;
err:
  printf("Do nothing...\n");
  return -1;
}

void print_column(frm_file_data *ffd, int c_id) 
{
  enum_field_types ftype= ffd->columns[c_id].type;
  uint length= ffd->columns[c_id].length;
  int label_id= ffd->columns[c_id].label_id;
  const Type_handler *handler= Type_handler::get_handler_by_real_type(ftype);
  Name type_name= handler->name();
  if (is_temporal_type_with_date(ftype) || ftype == MYSQL_TYPE_NEWDATE)
    printf("%s", type_name.ptr());
  else if (ftype == MYSQL_TYPE_ENUM || ftype == MYSQL_TYPE_SET)
  {
    printf("%s(", type_name.ptr());
    for (uint j=0;j<ffd->labels[label_id].names.size() - 1;j++)
    {
      LEX_CSTRING ts= ffd->labels[label_id].names.at(j);
      printf("'%s',", ts.str);
    }
    LEX_CSTRING ts=
        ffd->labels[label_id].names.at(ffd->labels[label_id].names.size() - 1);
    printf("'%s')", ts.str);
  }
  else
    printf("%s(%d)", type_name.ptr(), length);
  if(!f_maybe_null(ffd->columns[c_id].flags))
    printf(" NOT NULL");
  if (ffd->columns[c_id].unireg_check == 15)
    printf(" AUTO INCREMENT");
  if (ffd->columns[c_id].default_value.length != 0)
    printf(" DEFAULT %s", ffd->columns[c_id].default_value.str);
}

void print_keys(frm_file_data *ffd, uint k_id) 
{
  bool is_primary=false;
  if (!strcmp("PRIMARY", ffd->keys[k_id].name.str))
  {
    is_primary= true;
    printf("PRIMARY KEY");
  }
  else if (ffd->keys[k_id].is_unique)
    printf("UNIQUE KEY");
  else if (ffd->keys[k_id].flags & HA_FULLTEXT)
    printf("FULLTEXT KEY");
  else if (ffd->keys[k_id].flags & HA_SPATIAL)
    printf("SPATIAL KEY");
  else
    printf("KEY");
  if (ffd->keys[k_id].name.length!=0 && !is_primary)
    printf(" `%s`", ffd->keys[k_id].name.str);
  printf(" (`%s`)", 
      ffd->columns[ffd->keys[k_id].field_number].name.str);
}

static std::unordered_set<uint> default_cs{
    32, 11, 1,  63, 26, 51, 57, 59, 4,  40, 36, 95, 3, 97,
    98, 19, 24, 28, 92, 25, 16, 6,  37, 7,  22, 8,  9, 30,
    41, 38, 39, 13, 10, 18, 35, 12, 54, 56, 60, 33, 45};

void print_table_options(frm_file_data *ffd)
{
  if (ffd->engine_name.length != 0)
    printf(" ENGINE=%s", ffd->engine_name.str);
  if (ffd->table_cs_name.length != 0)
  {
    printf(" DEFAULT CHARSET=%s", ffd->table_cs_name.str);
    if (!default_cs.count(ffd->table_charset))
      printf(" COLLATE=%s", ffd->table_coll_name.str);
  }
}

int show_create_table(LEX_CSTRING table_name, frm_file_data *ffd)
{
  printf("CREATE TABLE `%s` (\n", table_name.str);
  for(uint i=0;i<ffd->column_count;i++)
  {
    printf("  `%s` ", ffd->columns[i].name.str);
    print_column(ffd, i);
    if (!(i == ffd->column_count - 1 && !ffd->key_count))
      printf(",");
    printf("\n");
  }
  for (uint i=0;i<ffd->key_count;i++)
  {
    printf("  ");
    print_keys(ffd, i);
    if (i != ffd->key_count - 1)
      printf(",");
    printf("\n");
  }
  printf(")");
  print_table_options(ffd);
  printf("\n");
  return 0;
}

int main(int argc, char **argv)
{
  MY_INIT(argv[0]);
  for (int i= 1; i < argc; i++)
  {
    const char *path= argv[i];
    uchar *frm;
    size_t len;
    frm_file_data *ffd= new frm_file_data();
    read_file(path, (const uchar **) &frm, &len);
    if (!is_binary_frm_header(frm))
    {
      printf("The .frm file is not a table...\n");
      continue;
    }
    parse(ffd, frm, len);
    LEX_CSTRING table_name= {NULL, 0};
    get_tablename(path, (char**)&table_name.str, &table_name.length);
    show_create_table(table_name, ffd);
  }
  return 0;
}
