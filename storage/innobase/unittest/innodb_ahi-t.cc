#include "tap.h"

#ifndef SUX_LOCK_GENERIC
# define SUX_LOCK_GENERIC
#endif
#ifndef NO_ELISION
# define NO_ELISION
#endif
#define thd_kill_level(thd) 0
#define srv0mon_h
#define MONITOR_INC(x)
#define MONITOR_INC_VALUE(x,y)
#include "../btr/btr0sea.cc"
const size_t alloc_max_retries= 0;
const byte zero[16384]= { '\0', };
const byte *field_ref_zero= zero;
ulint srv_buf_pool_curr_size, srv_buf_pool_old_size, srv_buf_pool_size;
uint32_t srv_page_size_shift= 14;
ulong srv_page_size= 1 << 14;
dict_sys_t dict_sys;
buf_pool_t buf_pool;
bool buf_page_t::flag_accessed() noexcept { return false; }
buf_block_t *buf_pool_t::block_from(const void *ptr) noexcept
{ return nullptr; }
void buf_pool_t::clear_hash_index() noexcept {}

void buf_pool_t::free_block(buf_block_t*) noexcept {}
void dict_mem_table_free(dict_table_t*) {}
void dict_mem_index_free(dict_index_t*) {}
buf_block_t *buf_LRU_get_free_block(buf_LRU_get) { return nullptr; }
ibool dtuple_check_typed(const dtuple_t*) { return true; }
bool btr_cur_t::check_mismatch(const dtuple_t&,bool,ulint) noexcept
{ return false; }
buf_block_t *buf_page_get_gen(const page_id_t, ulint, rw_lock_type_t,
                              buf_block_t*,ulint,mtr_t*,dberr_t*) noexcept
{ return nullptr; }
bool buf_page_make_young_if_needed(buf_page_t*) { return false; }

mtr_t::mtr_t(trx_t *trx) : trx(trx) {}
mtr_t::~mtr_t()= default;
void mtr_t::start() {}
void mtr_t::commit() {}
void mtr_t::rollback_to_savepoint(ulint, ulint) {}
void small_vector_base::grow_by_1(void *, size_t) noexcept {}
void buf_inc_get(trx_t*) noexcept {}

void sql_print_error(const char*, ...) {}
ulint ut_find_prime(ulint n) { return n; }
void mem_heap_block_free(mem_block_info_t*, mem_block_info_t*){}
namespace ib { error::~error() {} fatal_or_error::~fatal_or_error() {} }
std::ostream &operator<<(std::ostream &out, const page_id_t) { return out; }

#ifdef UNIV_DEBUG
byte data_error;
void srw_lock_debug::SRW_LOCK_INIT(mysql_pfs_key_t) noexcept {}
void srw_lock_debug::destroy() noexcept {}
bool srw_lock_debug::have_wr() const noexcept { return false; }
bool srw_lock_debug::have_rd() const noexcept { return false; }
bool srw_lock_debug::have_any() const noexcept { return false; }
void srw_lock_debug::rd_unlock() noexcept {}
void srw_lock_debug::rd_lock(SRW_LOCK_ARGS(const char*,unsigned)) noexcept {}
void srw_lock_debug::wr_lock(SRW_LOCK_ARGS(const char*,unsigned)) noexcept {}
void srw_lock_debug::wr_unlock() noexcept {}
#endif

void page_hash_latch::read_lock_wait() noexcept {}
# ifndef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
template<> void pthread_mutex_wrapper<true>::wr_wait() noexcept {}
# endif
template<> void srw_lock_<true>::rd_wait() noexcept {}
template<> void srw_lock_<true>::wr_wait() noexcept {}
template<bool spin> void ssux_lock_impl<spin>::wake() noexcept {}
template<bool spin> void srw_mutex_impl<spin>::wake() noexcept {}
void page_hash_latch::write_lock_wait() noexcept {}

#ifdef UNIV_PFS_MEMORY
PSI_memory_key ut_new_get_key_by_file(uint32_t){ return PSI_NOT_INSTRUMENTED; }
PSI_memory_key mem_key_other, mem_key_std;
#endif

#ifdef UNIV_PFS_RWLOCK
template<bool spin>
void srw_lock_impl<spin>::psi_wr_lock(const char*, unsigned) noexcept {}
template<bool spin>
void srw_lock_impl<spin>::psi_rd_lock(const char*, unsigned) noexcept {}

void dict_sys_t::unlock() noexcept {}
void dict_sys_t::freeze(const char *, unsigned) noexcept {}
void dict_sys_t::unfreeze() noexcept {}
#endif /* UNIV_PFS_RWLOCK */

void ut_dbg_assertion_failed(const char *e, const char *file, unsigned line)
{
  fprintf(stderr, "%s:%u: Assertion %s failed\n", file, line, e ? e : "");
  abort();
}

int main(int, char **argv)
{
  MY_INIT(*argv);
  plan(42);

  btr_search.create();
  btr_search.free();

  dfield_t fields[2]= {{nullptr,0,0,UNIV_SQL_NULL,{0,DATA_VARCHAR,3,1,1}},
                       {(char*)"42",0,0,2,{0,DATA_CHAR,2,1,1}}};
  dtuple_t tuple2{0,2,2,0,fields,nullptr, ut_d(DATA_TUPLE_MAGIC_N) };
  dict_col_t cols[]={{}, {}, {DATA_NOT_NULL,DATA_CHAR,2,1,1,1,0,0,{nullptr,0}}};
  dict_field_t ifields[3]= {{}, {}, {}};
  dict_table_t table{};
  dict_index_t index{};
  index.table= &table;
  index.n_uniq= 3;
  index.n_nullable= 3;
  index.n_fields= 3;
  index.n_core_fields= 3;
  index.n_core_null_bytes= 1;
  index.fields= ifields;

  ifields[0].col= &cols[0];
  ifields[1].col= &cols[2];
  ifields[2].col= &cols[2];
  ifields[1].fixed_len= 2;
  ifields[2].fixed_len= 2;

  constexpr uint32_t crc42= 0x2e7d3dcb, crc3z42= 0x9a6e3c2c,
    crc2z= 0xf16177d2, crc3z= 0x6064a37a;

  {
    btr_cur_t cursor;
    cursor.page_cur.index= &index;
    cursor.n_bytes_fields= 2;

    ok(dtuple_fold(&tuple2, &cursor) == crc42, "dtuple_fold(NULL,'42')");
    table.flags= DICT_TF_COMPACT;
    ok(dtuple_fold(&tuple2, &cursor) == crc42, "dtuple_fold(NULL,'42')");
    fields[0].type.mtype= DATA_CHAR;
    ok(dtuple_fold(&tuple2, &cursor) == crc42, "dtuple_fold(NULL,'42')");
    table.flags= 0;
    ok(dtuple_fold(&tuple2, &cursor) == crc3z42, "dtuple_fold('\\0\\0\\0','42')");
    fields[0].type.mtype= DATA_VARCHAR;

    cursor.n_bytes_fields= 1;
    ok(dtuple_fold(&tuple2, &cursor) == 0, "dtuple_fold(NULL)");
    table.flags= DICT_TF_COMPACT;
    ok(dtuple_fold(&tuple2, &cursor) == 0, "dtuple_fold(NULL)");
    fields[0].type.mtype= DATA_CHAR;
    ok(dtuple_fold(&tuple2, &cursor) == 0, "dtuple_fold(NULL)");
    table.flags= 0;
    ok(dtuple_fold(&tuple2, &cursor) == crc3z, "dtuple_fold('\\0\\0\\0')");
    fields[0].type.mtype= DATA_VARCHAR;

    cursor.n_bytes_fields= 2 << 16;
    ok(dtuple_fold(&tuple2, &cursor) == 0, "dtuple_fold(NULL)");
    table.flags= DICT_TF_COMPACT;
    ok(dtuple_fold(&tuple2, &cursor) == 0, "dtuple_fold(NULL)");
    fields[0].type.mtype= DATA_CHAR;
    ok(dtuple_fold(&tuple2, &cursor) == 0, "dtuple_fold(NULL)");
    table.flags= 0;
    ok(dtuple_fold(&tuple2, &cursor) == crc2z, "dtuple_fold('\\0\\0')");
    fields[0].type.mtype= DATA_VARCHAR;
  }

  byte *page= static_cast<byte*>(aligned_malloc(16384, 16384));
  memset_aligned<16384>(page, 0, 16384);
  byte *rec= &page[256];
  page[PAGE_HEADER + PAGE_HEAP_TOP]= 1;
  page[PAGE_HEADER + PAGE_HEAP_TOP + 1]= 4 + 2;
  const byte r1_varchar[]= {2,0x80,0,0,0,2<<1|1,0,0, '4','2'};
  const byte r2_varchar[]= {0,2,0x80,0,0,0,0,2<<1,0,0, '4','2'};
  const byte r1_var3[]= {2,0x80,0x80,0,0,0,3<<1|1,0,0, '4','2'};
  const byte r2_var3[]= {0,2,0x80,0,0x80,0,0,0,0,3<<1,0,0, '4','2'};
  const byte r1_char[]={2+3,0x83,0,0,0,2<<1|1,0,0, 0,0,0,'4','2'};
  const byte r2_char[]= {0,2+3,0x80,3,0,0,0,2<<1,0,0, 0,0,0,'4','2'};
  const byte c[]= { 0,1,0,0,0,0,0, '4','2'};
  const byte c3[]= { 0,3,0,0,0,0,0, '4','2'};

  memcpy(rec - sizeof r1_varchar + 2, r1_varchar, sizeof r1_varchar);
  ok(rec_fold(rec, index, 2, false) == crc42, "rec_fold(NULL, '42')");
  ok(rec_fold(rec, index, 1, false) == 0, "rec_fold(NULL)");
  ok(rec_fold(rec, index, 2 << 16, false) == 0, "rec_fold(NULL)");
  memcpy(rec - sizeof r2_varchar + 2, r2_varchar, sizeof r2_varchar);
  ok(rec_fold(rec, index, 2, false) == crc42, "rec_fold(NULL, '42')");
  ok(rec_fold(rec, index, 1, false) == 0, "rec_fold(NULL)");
  ok(rec_fold(rec, index, 2 << 16, false) == 0, "rec_fold(NULL)");

  memcpy(rec - sizeof r1_var3 + 2, r1_var3, sizeof r1_var3);
  ok(rec_fold(rec, index, 3, false) == crc42, "rec_fold(NULL, NULL, '42')");
  ok(rec_fold(rec, index, 2, false) == 0, "rec_fold(NULL,NULL)");
  ok(rec_fold(rec, index, 1 | 2 << 16, false) == 0, "rec_fold(NULL,NULL)");
  memcpy(rec - sizeof r2_var3 + 2, r2_var3, sizeof r2_var3);
  ok(rec_fold(rec, index, 3, false) == crc42, "rec_fold(NULL, NULL, '42')");
  ok(rec_fold(rec, index, 2, false) == 0, "rec_fold(NULL,NULL)");
  ok(rec_fold(rec, index, 1 | 2 << 16, false) == 0, "rec_fold(NULL,NULL)");

  fields[0].type.mtype= DATA_CHAR;
  memcpy(rec - sizeof r1_char + 3 + 2, r1_char, sizeof r1_char);
  ok(rec_fold(rec, index, 2, false) == crc3z42, "rec_fold('\\0\\0\\0', '42')");
  ok(rec_fold(rec, index, 1, false) == crc3z, "rec_fold('\\0\\0\\0')");
  ok(rec_fold(rec, index, 2 << 16, false) == crc2z, "rec_fold('\\0\\0')");
  memcpy(rec - sizeof r2_char + 3 + 2, r2_char, sizeof r2_char);
  ok(rec_fold(rec, index, 2, false) == crc3z42, "rec_fold('\\0\\0\\0', '42')");
  ok(rec_fold(rec, index, 1, false) == crc3z, "rec_fold('\\0\\0\\0')");
  ok(rec_fold(rec, index, 2 << 16, false) == crc2z, "rec_fold('\\0\\0')");

  page[PAGE_HEADER + PAGE_N_HEAP]= 0x80;
  table.flags= DICT_TF_COMPACT;
  memcpy(rec - sizeof c + 2, c, sizeof c);
  ok(rec_fold(rec, index, 2, true) == crc42, "rec_fold(NULL, '42')");
  ok(rec_fold(rec, index, 1, true) == 0, "rec_fold(NULL)");
  ok(rec_fold(rec, index, 2 << 16, true) == 0, "rec_fold(NULL)");
  fields[0].type.mtype= DATA_VARCHAR;
  ok(rec_fold(rec, index, 2, true) == crc42, "rec_fold(NULL, '42')");
  ok(rec_fold(rec, index, 1, true) == 0, "rec_fold(NULL)");
  ok(rec_fold(rec, index, 2 << 16, true) == 0, "rec_fold(NULL)");

  memcpy(rec - sizeof c3 + 2, c3, sizeof c3);
  fields[0].type.mtype= DATA_CHAR;

  ifields[1].col= &cols[1];
  ifields[1].fixed_len= 0;

  ok(rec_fold(rec, index, 3, true) == crc42, "rec_fold(NULL, NULL, '42')");
  ok(rec_fold(rec, index, 2, true) == 0, "rec_fold(NULL, NULL)");
  ok(rec_fold(rec, index, 1 | 2 << 16, true) == 0, "rec_fold(NULL, NULL)");
  fields[0].type.mtype= DATA_VARCHAR;
  ok(rec_fold(rec, index, 3, true) == crc42, "rec_fold(NULL, NULL, '42')");
  ok(rec_fold(rec, index, 2, true) == 0, "rec_fold(NULL, NULL)");
  ok(rec_fold(rec, index, 1 | 2 << 16, true) == 0, "rec_fold(NULL, NULL)");
  aligned_free(page);

  my_end(MY_CHECK_ERROR);
  return exit_status();
}
