
#define MARIAFRM_VERSION "1.0"

#include <fstream>
#include <iterator>
#include <vector>

#include <my_global.h>
#include <unireg.h>
#include "mariafrm.h"
#include <table.h>
#include <handler.h>
#include <my_sys.h>
#include <m_string.h>

int read(const uchar **frm, size_t *len, const char *filename)
{
  std::ifstream input(filename, std::ios::binary); //May not be portable, will change it
  std::vector<uchar> buffer(std::istreambuf_iterator<char>(input), {});
  *frm= buffer.data();
  *len= buffer.size();
  return 0;
}

int get_charset(frm_file_data *ffd, uint cs_number)
{
  CHARSET_INFO *c= get_charset(cs_number, MYF(0));
  ffd->table_cs_name= c->cs_name;
  ffd->table_coll_name= c->coll_name;
  return 0;
}

int parse(frm_file_data *ffd, const uchar *frm, size_t len)
{
  size_t current_pos, end;
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
  //ffd->rtype= static_cast<row_type>(frm[40]);
  ffd->rtype= (enum row_type)(uint) ffd->row_format;
  ffd->key_block_size= uint2korr(frm + 62);
  ffd->handler_option= uint2korr(frm + 30);

  if (ffd->extrainfo_length)
  {
    current_pos= ffd->extrainfo_offset;
    end= current_pos + ffd->extrainfo_length;
    ffd->connect_string.length= uint2korr(frm + current_pos);
    current_pos+= 2;
    ffd->connection= (char *) my_safe_alloca(ffd->connect_string.length);
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
      ffd->partition_info_str=
          (char *) my_safe_alloca(ffd->partition_info_str_len + 1);
      memcpy(ffd->partition_info_str, frm + current_pos,
             ffd->partition_info_str_len + 1);
      current_pos+= (ffd->partition_info_str_len + 1);
    }
  }
  ffd->legacy_db_type_1= (enum legacy_db_type)(uint) frm[3];
  ffd->legacy_db_type_2= (enum legacy_db_type)(uint) frm[61];
  //---READ COLUMN INFORMATION---

  return 0;
err:
  printf("Do nothing...\n");
  return -1;
}

int show_create_table() { return 0; }
int show_create_view() { return 0; }

int main(int argc, char **argv)
{
  for (int i=1;i<argc;i++)
  {
    const char *filename= argv[i];
    uchar *frm;
    size_t len;
    frm_file_data *ffd= new frm_file_data();
    read((const uchar **) &frm, &len, filename);
    if (!is_binary_frm_header(frm))
    {
      printf("Not a valid .frm file");
      continue;
    }
    parse(ffd,frm, len);
  }
  return 0;
}
