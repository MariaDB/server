/* Copyright (c) 2009, 2010, Oracle and/or its affiliates.
   Copyright (c) 2012, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* support for Services */
#include <service_versions.h>
#include <mysql/service_wsrep.h>
#include <mysql/service_thd_mdl.h>

struct st_service_ref {
  const char *name;
  uint version;
  void *service;
};

static struct my_snprintf_service_st my_snprintf_handler = {
  my_snprintf,
  my_vsnprintf
};

static struct thd_alloc_service_st thd_alloc_handler= {
  thd_alloc,
  thd_calloc,
  thd_strdup,
  thd_strmake,
  thd_memdup,
  thd_make_lex_string
};

static struct thd_wait_service_st thd_wait_handler= {
  thd_wait_begin,
  thd_wait_end
};

static struct progress_report_service_st progress_report_handler= {
  thd_progress_init,
  thd_progress_report,
  thd_progress_next_stage,
  thd_progress_end,
  set_thd_proc_info
};

static struct kill_statement_service_st thd_kill_statement_handler= {
  thd_kill_level
};

static struct thd_timezone_service_st thd_timezone_handler= {
  thd_TIME_to_gmt_sec,
  thd_gmt_sec_to_TIME
};

static struct my_sha2_service_st my_sha2_handler = {
  my_sha224,
  my_sha224_multi,
  my_sha224_context_size,
  my_sha224_init,
  my_sha224_input,
  my_sha224_result,
  my_sha256,
  my_sha256_multi,
  my_sha256_context_size,
  my_sha256_init,
  my_sha256_input,
  my_sha256_result,
  my_sha384,
  my_sha384_multi,
  my_sha384_context_size,
  my_sha384_init,
  my_sha384_input,
  my_sha384_result,
  my_sha512,
  my_sha512_multi,
  my_sha512_context_size,
  my_sha512_init,
  my_sha512_input,
  my_sha512_result,
};

static struct my_sha1_service_st my_sha1_handler = {
  my_sha1,
  my_sha1_multi,
  my_sha1_context_size,
  my_sha1_init,
  my_sha1_input,
  my_sha1_result
};

static struct my_md5_service_st my_md5_handler = {
  my_md5,
  my_md5_multi,
  my_md5_context_size,
  my_md5_init,
  my_md5_input,
  my_md5_result
};

static struct logger_service_st logger_service_handler= {
  logger_init_mutexes,
  logger_open,
  logger_close,
  logger_vprintf,
  logger_printf,
  logger_write,
  logger_rotate
};

static struct thd_autoinc_service_st thd_autoinc_handler= {
  thd_get_autoinc
};

static struct thd_rnd_service_st thd_rnd_handler= {
  thd_rnd,
  thd_create_random_password
};

static struct base64_service_st base64_handler= {
  my_base64_needed_encoded_length,
  my_base64_encode_max_arg_length,
  my_base64_needed_decoded_length,
  my_base64_decode_max_arg_length,
  my_base64_encode,
  my_base64_decode
};

static struct thd_error_context_service_st thd_error_context_handler= {
  thd_get_error_message,
  thd_get_error_number,
  thd_get_error_row,
  thd_inc_error_row,
  thd_get_error_context_description
};

static struct wsrep_service_st wsrep_handler = {
  get_wsrep_recovery,
  wsrep_consistency_check,
  wsrep_is_wsrep_xid,
  wsrep_xid_seqno,
  wsrep_xid_uuid,
  wsrep_on,
  wsrep_prepare_key_for_innodb,
  wsrep_thd_LOCK,
  wsrep_thd_TRYLOCK,
  wsrep_thd_UNLOCK,
  wsrep_thd_query,
  wsrep_thd_retry_counter,
  wsrep_thd_ignore_table,
  wsrep_thd_trx_seqno,
  wsrep_thd_is_aborting,
  wsrep_set_data_home_dir,
  wsrep_thd_is_BF,
  wsrep_thd_is_local,
  wsrep_thd_self_abort,
  wsrep_thd_append_key,
  wsrep_thd_client_state_str,
  wsrep_thd_client_mode_str,
  wsrep_thd_transaction_state_str,
  wsrep_thd_transaction_id,
  wsrep_thd_bf_abort,
  wsrep_thd_order_before,
  wsrep_handle_SR_rollback,
  wsrep_thd_skip_locking,
  wsrep_get_sr_table_name,
  wsrep_get_debug,
  wsrep_commit_ordered,
  wsrep_thd_is_applying,
  wsrep_OSU_method_get,
  wsrep_thd_has_ignored_error,
  wsrep_thd_set_ignored_error,
  wsrep_report_bf_lock_wait,
  wsrep_thd_kill_LOCK,
  wsrep_thd_kill_UNLOCK,
  wsrep_thd_set_PA_unsafe
};

static struct thd_specifics_service_st thd_specifics_handler=
{
  thd_key_create,
  thd_key_delete,
  thd_getspecific,
  thd_setspecific
};

static struct encryption_scheme_service_st encryption_scheme_handler=
{
 encryption_scheme_encrypt,
 encryption_scheme_decrypt
};

static struct my_crypt_service_st crypt_handler=
{
  my_aes_crypt_init,
  my_aes_crypt_update,
  my_aes_crypt_finish,
  my_aes_crypt,
  my_aes_get_size,
  my_aes_ctx_size,
  my_random_bytes
};

static struct my_print_error_service_st my_print_error_handler=
{
  my_error,
  my_printf_error,
  my_printv_error
};

struct json_service_st json_handler=
{
  json_type,
  json_get_array_item,
  json_get_object_key,
  json_get_object_nkey,
  json_escape_string,
  json_unescape_json
};

static struct thd_mdl_service_st thd_mdl_handler=
{
  thd_mdl_context
};

static struct st_service_ref list_of_services[]=
{
  { "base64_service",              VERSION_base64,              &base64_handler },
  { "debug_sync_service",          VERSION_debug_sync,          0 }, // updated in plugin_init()
  { "encryption_scheme_service",   VERSION_encryption_scheme,   &encryption_scheme_handler },
  { "encryption_service",          VERSION_encryption,          &encryption_handler },
  { "logger_service",              VERSION_logger,              &logger_service_handler },
  { "my_crypt_service",            VERSION_my_crypt,            &crypt_handler},
  { "my_md5_service",              VERSION_my_md5,              &my_md5_handler},
  { "my_print_error_service",      VERSION_my_print_error,      &my_print_error_handler},
  { "my_sha1_service",             VERSION_my_sha1,             &my_sha1_handler},
  { "my_sha2_service",             VERSION_my_sha2,             &my_sha2_handler},
  { "my_snprintf_service",         VERSION_my_snprintf,         &my_snprintf_handler },
  { "progress_report_service",     VERSION_progress_report,     &progress_report_handler },
  { "thd_alloc_service",           VERSION_thd_alloc,           &thd_alloc_handler },
  { "thd_autoinc_service",         VERSION_thd_autoinc,         &thd_autoinc_handler },
  { "thd_error_context_service",   VERSION_thd_error_context,   &thd_error_context_handler },
  { "thd_kill_statement_service",  VERSION_kill_statement,      &thd_kill_statement_handler },
  { "thd_rnd_service",             VERSION_thd_rnd,             &thd_rnd_handler },
  { "thd_specifics_service",       VERSION_thd_specifics,       &thd_specifics_handler },
  { "thd_timezone_service",        VERSION_thd_timezone,        &thd_timezone_handler },
  { "thd_wait_service",            VERSION_thd_wait,            &thd_wait_handler },
  { "wsrep_service",               VERSION_wsrep,               &wsrep_handler },
  { "json_service",                VERSION_json,                &json_handler },
  { "thd_mdl_service",             VERSION_thd_mdl,             &thd_mdl_handler }
};
