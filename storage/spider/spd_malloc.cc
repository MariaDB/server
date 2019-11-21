/* Copyright (C) 2012-2014 Kentoku Shiba

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

#define MYSQL_SERVER 1
#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_analyse.h"
#endif
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_malloc.h"

extern handlerton *spider_hton_ptr;

pthread_mutex_t spider_mem_calc_mutex;

const char *spider_alloc_func_name[SPIDER_MEM_CALC_LIST_NUM];
const char *spider_alloc_file_name[SPIDER_MEM_CALC_LIST_NUM];
ulong      spider_alloc_line_no[SPIDER_MEM_CALC_LIST_NUM];
ulonglong  spider_total_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
longlong   spider_current_alloc_mem[SPIDER_MEM_CALC_LIST_NUM];
ulonglong  spider_alloc_mem_count[SPIDER_MEM_CALC_LIST_NUM];
ulonglong  spider_free_mem_count[SPIDER_MEM_CALC_LIST_NUM];

void spider_merge_mem_calc(
  SPIDER_TRX *trx,
  bool force
) {
  uint roop_count;
  time_t tmp_time;
  DBUG_ENTER("spider_merge_mem_calc");
  if (force)
  {
    pthread_mutex_lock(&spider_mem_calc_mutex);
    tmp_time = (time_t) time((time_t*) 0);
  } else {
    tmp_time = (time_t) time((time_t*) 0);
    if (
      difftime(tmp_time, trx->mem_calc_merge_time) < 2 ||
      pthread_mutex_trylock(&spider_mem_calc_mutex)
    )
      DBUG_VOID_RETURN;
  }
  for (roop_count = 0; roop_count < SPIDER_MEM_CALC_LIST_NUM; roop_count++)
  {
    DBUG_ASSERT(!spider_alloc_func_name[roop_count] ||
      !trx->alloc_func_name[roop_count] ||
      spider_alloc_func_name[roop_count] == trx->alloc_func_name[roop_count]);
    DBUG_ASSERT(!spider_alloc_file_name[roop_count] ||
      !trx->alloc_file_name[roop_count] ||
      spider_alloc_file_name[roop_count] == trx->alloc_file_name[roop_count]);
    DBUG_ASSERT(!spider_alloc_line_no[roop_count] ||
      !trx->alloc_line_no[roop_count] ||
      spider_alloc_line_no[roop_count] == trx->alloc_line_no[roop_count]);
    if (trx->alloc_func_name[roop_count])
    {
      spider_alloc_func_name[roop_count] = trx->alloc_func_name[roop_count];
      spider_alloc_file_name[roop_count] = trx->alloc_file_name[roop_count];
      spider_alloc_line_no[roop_count] = trx->alloc_line_no[roop_count];
      spider_total_alloc_mem[roop_count] +=
        trx->total_alloc_mem_buffer[roop_count];
      trx->total_alloc_mem_buffer[roop_count] = 0;
      spider_alloc_mem_count[roop_count] +=
        trx->alloc_mem_count_buffer[roop_count];
      trx->alloc_mem_count_buffer[roop_count] = 0;
    }
    spider_current_alloc_mem[roop_count] +=
      trx->current_alloc_mem_buffer[roop_count];
    trx->current_alloc_mem_buffer[roop_count] = 0;
    spider_free_mem_count[roop_count] +=
      trx->free_mem_count_buffer[roop_count];
    trx->free_mem_count_buffer[roop_count] = 0;
  }
  pthread_mutex_unlock(&spider_mem_calc_mutex);
  trx->mem_calc_merge_time = tmp_time;
  DBUG_VOID_RETURN;
}

void spider_free_mem_calc(
  SPIDER_TRX *trx,
  uint id,
  size_t size
) {
  DBUG_ENTER("spider_free_mem_calc");
  DBUG_ASSERT(id < SPIDER_MEM_CALC_LIST_NUM);
  DBUG_PRINT("info",("spider trx=%p id=%u size=%llu",
    trx, id, (ulonglong) size));
  if (trx)
  {
    DBUG_PRINT("info",("spider calc into trx"));
    trx->current_alloc_mem[id] -= size;
    trx->current_alloc_mem_buffer[id] -= size;
    trx->free_mem_count[id] += 1;
    trx->free_mem_count_buffer[id] += 1;
  } else {
    DBUG_PRINT("info",("spider calc into global"));
    pthread_mutex_lock(&spider_mem_calc_mutex);
    spider_current_alloc_mem[id] -= size;
    spider_free_mem_count[id] += 1;
    pthread_mutex_unlock(&spider_mem_calc_mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_alloc_mem_calc(
  SPIDER_TRX *trx,
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no,
  size_t size
) {
  DBUG_ENTER("spider_alloc_mem_calc");
  DBUG_ASSERT(id < SPIDER_MEM_CALC_LIST_NUM);
  DBUG_PRINT("info",("spider trx=%p id=%u size=%llu",
    trx, id, (ulonglong) size));
  if (trx)
  {
    DBUG_PRINT("info",("spider calc into trx"));
    DBUG_ASSERT(!trx->alloc_func_name[id] ||
      trx->alloc_func_name[id] == func_name);
    DBUG_ASSERT(!trx->alloc_file_name[id] ||
      trx->alloc_file_name[id] == file_name);
    DBUG_ASSERT(!trx->alloc_line_no[id] ||
      trx->alloc_line_no[id] == line_no);
    trx->alloc_func_name[id] = func_name;
    trx->alloc_file_name[id] = file_name;
    trx->alloc_line_no[id] = line_no;
    trx->total_alloc_mem[id] += size;
    trx->total_alloc_mem_buffer[id] += size;
    trx->current_alloc_mem[id] += size;
    trx->current_alloc_mem_buffer[id] += size;
    trx->alloc_mem_count[id] += 1;
    trx->alloc_mem_count_buffer[id] += 1;
  } else {
    DBUG_PRINT("info",("spider calc into global"));
    pthread_mutex_lock(&spider_mem_calc_mutex);
    DBUG_ASSERT(!spider_alloc_func_name[id] ||
      spider_alloc_func_name[id] == func_name);
    DBUG_ASSERT(!spider_alloc_file_name[id] ||
      spider_alloc_file_name[id] == file_name);
    DBUG_ASSERT(!spider_alloc_line_no[id] ||
      spider_alloc_line_no[id] == line_no);
    spider_alloc_func_name[id] = func_name;
    spider_alloc_file_name[id] = file_name;
    spider_alloc_line_no[id] = line_no;
    spider_total_alloc_mem[id] += size;
    spider_current_alloc_mem[id] += size;
    spider_alloc_mem_count[id] += 1;
    pthread_mutex_unlock(&spider_mem_calc_mutex);
  }
  DBUG_VOID_RETURN;
}

void spider_free_mem(
  SPIDER_TRX *trx,
  void *ptr,
  myf my_flags
) {
  uint id, size;
  uchar *tmp_ptr = (uchar *) ptr;
  DBUG_ENTER("spider_free_mem");
  tmp_ptr -= ALIGN_SIZE(sizeof(uint));
  size = *((uint *) tmp_ptr);
  tmp_ptr -= ALIGN_SIZE(sizeof(uint));
  id = *((uint *) tmp_ptr);
  my_free(tmp_ptr, my_flags);

  spider_free_mem_calc(trx, id, size);
  DBUG_VOID_RETURN;
}

void *spider_alloc_mem(
  SPIDER_TRX *trx,
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no,
  size_t size,
  myf my_flags
) {
  uchar *ptr;
  DBUG_ENTER("spider_alloc_mem");
  size += ALIGN_SIZE(sizeof(uint)) + ALIGN_SIZE(sizeof(uint));
  if (!(ptr = (uchar *) my_malloc(size, my_flags)))
    DBUG_RETURN(NULL);

  spider_alloc_mem_calc(trx, id, func_name, file_name, line_no, size);
  *((uint *) ptr) = id;
  ptr += ALIGN_SIZE(sizeof(uint));
  *((uint *) ptr) = size;
  ptr += ALIGN_SIZE(sizeof(uint));
  DBUG_RETURN(ptr);
}

void *spider_bulk_alloc_mem(
  SPIDER_TRX *trx,
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no,
  myf my_flags,
  ...
) {
  uchar **tmp_ptr, *top_ptr, *current_ptr;
  uint total_size;
  va_list args;
  DBUG_ENTER("spider_bulk_alloc_mem");
  total_size = ALIGN_SIZE(sizeof(uint)) + ALIGN_SIZE(sizeof(uint));
  va_start(args, my_flags);
  while (va_arg(args, char **))
    total_size += ALIGN_SIZE(va_arg(args, uint));
  va_end(args);

  if (!(top_ptr = (uchar *) my_malloc(total_size, my_flags)))
    DBUG_RETURN(NULL);

  spider_alloc_mem_calc(trx, id, func_name, file_name, line_no, total_size);
  *((uint *) top_ptr) = id;
  top_ptr += ALIGN_SIZE(sizeof(uint));
  *((uint *) top_ptr) = total_size;
  top_ptr += ALIGN_SIZE(sizeof(uint));

  current_ptr = top_ptr;
  va_start(args, my_flags);
  while ((tmp_ptr = (uchar **) va_arg(args, char **)))
  {
    *tmp_ptr = current_ptr;
    current_ptr += ALIGN_SIZE(va_arg(args, uint));
  }
  va_end(args);
  DBUG_RETURN((void *) top_ptr);
}

#define SPIDER_STRING_CALC_MEM \
  if (mem_calc_inited) \
  { \
    uint32 new_alloc_mem = \
      (this->str.is_alloced() ? this->str.alloced_length() : 0); \
    if (new_alloc_mem != current_alloc_mem) \
    { \
      if (new_alloc_mem > current_alloc_mem) \
        spider_alloc_mem_calc(spider_current_trx, id, func_name, file_name, \
          line_no, new_alloc_mem - current_alloc_mem); \
      else \
        spider_free_mem_calc(spider_current_trx, id, \
          current_alloc_mem - new_alloc_mem); \
      current_alloc_mem = new_alloc_mem; \
    } \
  }

spider_string::spider_string(
) : str(), next(NULL)
{
  DBUG_ENTER("spider_string::spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  mem_calc_inited = FALSE;
  DBUG_VOID_RETURN;
}

spider_string::spider_string(
  uint32 length_arg
) : str(length_arg), next(NULL)
{
  DBUG_ENTER("spider_string::spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  mem_calc_inited = FALSE;
  DBUG_VOID_RETURN;
}

spider_string::spider_string(
  const char *str,
  CHARSET_INFO *cs
) : str(str, cs), next(NULL)
{
  DBUG_ENTER("spider_string::spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  mem_calc_inited = FALSE;
  DBUG_VOID_RETURN;
}

spider_string::spider_string(
  const char *str,
  uint32 len,
  CHARSET_INFO *cs
) : str(str, len, cs), next(NULL)
{
  DBUG_ENTER("spider_string::spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  mem_calc_inited = FALSE;
  DBUG_VOID_RETURN;
}

spider_string::spider_string(
  char *str,
  uint32 len,
  CHARSET_INFO *cs
) : str(str, len, cs), next(NULL)
{
  DBUG_ENTER("spider_string::spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  mem_calc_inited = FALSE;
  DBUG_VOID_RETURN;
}

spider_string::spider_string(
  const String &str
) : str(str), next(NULL)
{
  DBUG_ENTER("spider_string::spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  mem_calc_inited = FALSE;
  DBUG_VOID_RETURN;
}

spider_string::~spider_string()
{
  DBUG_ENTER("spider_string::~spider_string");
  DBUG_PRINT("info",("spider this=%p", this));
  if (mem_calc_inited)
    free();
  DBUG_VOID_RETURN;
}

void spider_string::init_mem_calc(
  uint id,
  const char *func_name,
  const char *file_name,
  ulong line_no
) {
  DBUG_ENTER("spider_string::init_mem_calc");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(!mem_calc_inited);
  this->id = id;
  this->func_name = func_name;
  this->file_name = file_name;
  this->line_no = line_no;
  if (str.is_alloced())
  {
    current_alloc_mem = str.alloced_length();
    spider_alloc_mem_calc(spider_current_trx, id, func_name, file_name,
      line_no, current_alloc_mem);
  } else
    current_alloc_mem = 0;
  mem_calc_inited = TRUE;
  DBUG_VOID_RETURN;
}

void spider_string::mem_calc()
{
  DBUG_ENTER("spider_string::mem_calc");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

String *spider_string::get_str()
{
  DBUG_ENTER("spider_string::get_str");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(&str);
}

void spider_string::set_charset(
  CHARSET_INFO *charset_arg
) {
  DBUG_ENTER("spider_string::set_charset");
  DBUG_PRINT("info",("spider this=%p", this));
  str.set_charset(charset_arg);
  DBUG_VOID_RETURN;
}

CHARSET_INFO *spider_string::charset() const
{
  DBUG_ENTER("spider_string::charset");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.charset());
}

uint32 spider_string::length() const
{
  DBUG_ENTER("spider_string::length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.length());
}

uint32 spider_string::alloced_length() const
{
  DBUG_ENTER("spider_string::alloced_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.alloced_length());
}

char &spider_string::operator [] (uint32 i) const
{
  DBUG_ENTER("spider_string::operator []");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str[i]);
}

void spider_string::length(
  uint32 len
) {
  DBUG_ENTER("spider_string::length");
  DBUG_PRINT("info",("spider this=%p", this));
  str.length(len);
  DBUG_VOID_RETURN;
}

bool spider_string::is_empty() const
{
  DBUG_ENTER("spider_string::is_empty");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.is_empty());
}

const char *spider_string::ptr() const
{
  DBUG_ENTER("spider_string::ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.ptr());
}

char *spider_string::c_ptr()
{
  DBUG_ENTER("spider_string::c_ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  char *res = str.c_ptr();
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

char *spider_string::c_ptr_quick()
{
  DBUG_ENTER("spider_string::c_ptr_quick");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.c_ptr_quick());
}

char *spider_string::c_ptr_safe()
{
  DBUG_ENTER("spider_string::c_ptr_safe");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  char *res = str.c_ptr_safe();
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

LEX_STRING spider_string::lex_string() const
{
  DBUG_ENTER("spider_string::lex_string");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.lex_string());
}

void spider_string::set(
  String &str,
  uint32 offset,
  uint32 arg_length
) {
  DBUG_ENTER("spider_string::set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  str.set(str, offset, arg_length);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

void spider_string::set(
  char *str,
  uint32 arg_length,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !this->str.is_alloced()) ||
    current_alloc_mem == this->str.alloced_length());
  this->str.set(str, arg_length, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

void spider_string::set(
  const char *str,
  uint32 arg_length,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !this->str.is_alloced()) ||
    current_alloc_mem == this->str.alloced_length());
  this->str.set(str, arg_length, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

bool spider_string::set_ascii(
  const char *str,
  uint32 arg_length
) {
  DBUG_ENTER("spider_string::set_ascii");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !this->str.is_alloced()) ||
    current_alloc_mem == this->str.alloced_length());
  bool res = this->str.set_ascii(str, arg_length);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

void spider_string::set_quick(
  char *str,
  uint32 arg_length,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set_quick");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !this->str.is_alloced()) ||
    current_alloc_mem == this->str.alloced_length());
  this->str.set_quick(str, arg_length, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

bool spider_string::set_int(
  longlong num,
  bool unsigned_flag,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set_int");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.set_int(num, unsigned_flag, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::set(
  longlong num,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.set(num, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::set(
  ulonglong num,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.set(num, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::set_real(
  double num,
  uint decimals,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set_real");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.set_real(num, decimals, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

void spider_string::chop()
{
  DBUG_ENTER("spider_string::chop");
  DBUG_PRINT("info",("spider this=%p", this));
  str.chop();
  DBUG_VOID_RETURN;
}

void spider_string::free()
{
  DBUG_ENTER("spider_string::free");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  str.free();
  if (mem_calc_inited && current_alloc_mem)
  {
    spider_free_mem_calc(spider_current_trx, id, current_alloc_mem);
    current_alloc_mem = 0;
  }
  DBUG_VOID_RETURN;
}

bool spider_string::alloc(uint32 arg_length)
{
  bool res;
  DBUG_ENTER("spider_string::alloc");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  res = str.alloc(arg_length);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::real_alloc(uint32 arg_length)
{
  bool res;
  DBUG_ENTER("spider_string::real_alloc");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  res = str.real_alloc(arg_length);
/*
  if (mem_calc_inited && !res && arg_length)
*/
  if (mem_calc_inited && !res)
  {
    DBUG_ASSERT(!current_alloc_mem);
    spider_alloc_mem_calc(spider_current_trx, id, func_name, file_name,
      line_no, str.alloced_length());
    current_alloc_mem = str.alloced_length();
  }
  DBUG_RETURN(res);
}

bool spider_string::realloc(uint32 arg_length)
{
  bool res;
  DBUG_ENTER("spider_string::realloc");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  res = str.realloc(arg_length);
  if (mem_calc_inited && !res && current_alloc_mem < str.alloced_length())
  {
    spider_alloc_mem_calc(spider_current_trx, id, func_name, file_name,
      line_no, str.alloced_length() - current_alloc_mem);
    current_alloc_mem = str.alloced_length();
  }
  DBUG_RETURN(res);
}

void spider_string::shrink(
  uint32 arg_length
) {
  DBUG_ENTER("spider_string::shrink");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  str.shrink(arg_length);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

bool spider_string::is_alloced()
{
  DBUG_ENTER("spider_string::is_alloced");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.is_alloced());
}

spider_string &spider_string::operator = (
  const String &s
) {
  DBUG_ENTER("spider_string::operator =");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  str = s;
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(*this);
}

bool spider_string::copy()
{
  DBUG_ENTER("spider_string::copy");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.copy();
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::copy(
  const spider_string &s
) {
  DBUG_ENTER("spider_string::copy");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.copy(s.str);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::copy(
  const String &s
) {
  DBUG_ENTER("spider_string::copy");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.copy(s);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::copy(
  const char *s,
  uint32 arg_length,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::copy");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.copy(s, arg_length, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::needs_conversion(
  uint32 arg_length,
  CHARSET_INFO *cs_from,
  CHARSET_INFO *cs_to,
  uint32 *offset
) {
  DBUG_ENTER("spider_string::needs_conversion");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.needs_conversion(arg_length, cs_from, cs_to, offset));
}

bool spider_string::copy_aligned(
  const char *s,
  uint32 arg_length,
  uint32 offset,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::copy_aligned");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.copy_aligned(s, arg_length, offset, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::set_or_copy_aligned(
  const char *s,
  uint32 arg_length,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::set_or_copy_aligned");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.set_or_copy_aligned(s, arg_length, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::copy(
  const char *s,
  uint32 arg_length,
  CHARSET_INFO *csfrom,
  CHARSET_INFO *csto,
  uint *errors
) {
  DBUG_ENTER("spider_string::copy");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.copy(s, arg_length, csfrom, csto, errors);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  const spider_string &s
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(s.str);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  const String &s
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(s);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  const char *s
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(s);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  LEX_STRING *ls
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(ls);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  const char *s,
  uint32 arg_length
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(s, arg_length);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  const char *s,
  uint32 arg_length,
  CHARSET_INFO *cs
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(s, arg_length, cs);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append_ulonglong(
  ulonglong val
) {
  DBUG_ENTER("spider_string::append_ulonglong");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append_ulonglong(val);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  IO_CACHE *file,
  uint32 arg_length
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(file, arg_length);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append_with_prefill(
  const char *s,
  uint32 arg_length,
  uint32 full_length,
  char fill_char
) {
  DBUG_ENTER("spider_string::append_with_prefill");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append_with_prefill(s, arg_length, full_length,
    fill_char);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

int spider_string::strstr(
  const String &search,
  uint32 offset
) {
  DBUG_ENTER("spider_string::strstr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.strstr(search, offset));
}

int spider_string::strrstr(
  const String &search,
  uint32 offset
) {
  DBUG_ENTER("spider_string::strrstr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.strrstr(search, offset));
}

bool spider_string::replace(
  uint32 offset,
  uint32 arg_length,
  const char *to,
  uint32 length
) {
  DBUG_ENTER("spider_string::replace");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.replace(offset, arg_length, to, length);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::replace(
  uint32 offset,
  uint32 arg_length,
  const String &to
) {
  DBUG_ENTER("spider_string::replace");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.replace(offset, arg_length, to);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  char chr
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(chr);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::fill(
  uint32 max_length,
  char fill
) {
  DBUG_ENTER("spider_string::fill");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.fill(max_length, fill);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

void spider_string::strip_sp()
{
  DBUG_ENTER("spider_string::strip_sp");
  DBUG_PRINT("info",("spider this=%p", this));
  str.strip_sp();
  DBUG_VOID_RETURN;
}

uint32 spider_string::numchars()
{
  DBUG_ENTER("spider_string::numchars");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.numchars());
}

int spider_string::charpos(
  int i,
  uint32 offset
) {
  DBUG_ENTER("spider_string::charpos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.charpos(i, offset));
}

int spider_string::reserve(
  uint32 space_needed
) {
  DBUG_ENTER("spider_string::reserve");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  int res = str.reserve(space_needed);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

int spider_string::reserve(
  uint32 space_needed,
  uint32 grow_by
) {
  DBUG_ENTER("spider_string::reserve");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  int res = str.reserve(space_needed, grow_by);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

void spider_string::q_append(
  const char c
) {
  DBUG_ENTER("spider_string::q_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.q_append(c);
  DBUG_VOID_RETURN;
}

void spider_string::q_append(
  const uint32 n
) {
  DBUG_ENTER("spider_string::q_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.q_append(n);
  DBUG_VOID_RETURN;
}

void spider_string::q_append(
  double d
) {
  DBUG_ENTER("spider_string::q_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.q_append(d);
  DBUG_VOID_RETURN;
}

void spider_string::q_append(
  double *d
) {
  DBUG_ENTER("spider_string::q_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.q_append(d);
  DBUG_VOID_RETURN;
}

void spider_string::q_append(
  const char *data,
  uint32 data_len
) {
  DBUG_ENTER("spider_string::q_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.q_append(data, data_len);
  DBUG_VOID_RETURN;
}

void spider_string::write_at_position(
  int position,
  uint32 value
) {
  DBUG_ENTER("spider_string::write_at_position");
  DBUG_PRINT("info",("spider this=%p", this));
  str.write_at_position(position, value);
  DBUG_VOID_RETURN;
}

void spider_string::qs_append(
  const char *str,
  uint32 len
) {
  DBUG_ENTER("spider_string::qs_append");
  DBUG_PRINT("info",("spider this=%p", this));
  this->str.qs_append(str, len);
  DBUG_VOID_RETURN;
}

void spider_string::qs_append(
  double d
) {
  DBUG_ENTER("spider_string::qs_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.qs_append(d);
  DBUG_VOID_RETURN;
}

void spider_string::qs_append(
  double *d
) {
  DBUG_ENTER("spider_string::qs_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.qs_append(d);
  DBUG_VOID_RETURN;
}

void spider_string::qs_append(
  const char c
) {
  DBUG_ENTER("spider_string::qs_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.qs_append(c);
  DBUG_VOID_RETURN;
}

void spider_string::qs_append(
  int i
) {
  DBUG_ENTER("spider_string::qs_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.qs_append(i);
  DBUG_VOID_RETURN;
}

void spider_string::qs_append(
  uint i
) {
  DBUG_ENTER("spider_string::qs_append");
  DBUG_PRINT("info",("spider this=%p", this));
  str.qs_append(i);
  DBUG_VOID_RETURN;
}

char *spider_string::prep_append(
  uint32 arg_length,
  uint32 step_alloc
) {
  DBUG_ENTER("spider_string::prep_append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  char *res = str.prep_append(arg_length, step_alloc);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append(
  const char *s,
  uint32 arg_length,
  uint32 step_alloc
) {
  DBUG_ENTER("spider_string::append");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  bool res = str.append(s, arg_length, step_alloc);
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

void spider_string::append_escape_string(
  const char *st,
  uint len
) {
  DBUG_ENTER("spider_string::append_escape_string");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  str.length(str.length() + escape_string_for_mysql(
    str.charset(), (char *) str.ptr() + str.length(), 0, st, len));
  DBUG_VOID_RETURN;
}

bool spider_string::append_for_single_quote(
  const char *st,
  uint len
) {
  DBUG_ENTER("spider_string::append_for_single_quote");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
#ifdef SPIDER_HAS_APPEND_FOR_SINGLE_QUOTE
  bool res = str.append_for_single_quote(st, len);
#else
  String ststr(st, len, str.charset());
  bool res = append_escaped(&str, &ststr);
#endif
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append_for_single_quote(
  const String *s
) {
  DBUG_ENTER("spider_string::append_for_single_quote");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
#ifdef SPIDER_HAS_APPEND_FOR_SINGLE_QUOTE
  bool res = str.append_for_single_quote(s);
#else
  bool res = append_escaped(&str, (String *) s);
#endif
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

bool spider_string::append_for_single_quote(
  const char *st
) {
  DBUG_ENTER("spider_string::append_for_single_quote");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
#ifdef SPIDER_HAS_APPEND_FOR_SINGLE_QUOTE
  bool res = str.append_for_single_quote(st);
#else
  String ststr(st, str.charset());
  bool res = append_escaped(&str, &ststr);
#endif
  SPIDER_STRING_CALC_MEM;
  DBUG_RETURN(res);
}

void spider_string::swap(
  spider_string &s
) {
  DBUG_ENTER("spider_string::swap");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(mem_calc_inited);
  DBUG_ASSERT((!current_alloc_mem && !str.is_alloced()) ||
    current_alloc_mem == str.alloced_length());
  str.swap(s.str);
  SPIDER_STRING_CALC_MEM;
  DBUG_VOID_RETURN;
}

bool spider_string::uses_buffer_owned_by(
  const String *s
) const
{
  DBUG_ENTER("spider_string::uses_buffer_owned_by");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.uses_buffer_owned_by(s));
}

bool spider_string::is_ascii() const
{
  DBUG_ENTER("spider_string::is_ascii");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(str.is_ascii());
}
