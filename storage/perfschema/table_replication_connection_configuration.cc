/*
      Copyright (c) 2013, 2021, Oracle and/or its affiliates.

      This program is free software; you can redistribute it and/or modify
      it under the terms of the GNU General Public License, version 2.0,
      as published by the Free Software Foundation.

      This program is also distributed with certain software (including
      but not limited to OpenSSL) that is licensed under separate terms,
      as designated in a particular file or component or in included license
      documentation.  The authors of MySQL hereby grant you an additional
      permission to link the program and your derivative works with the
      separately licensed software that they have included with MySQL.

      This program is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
      GNU General Public License, version 2.0, for more details.

      You should have received a copy of the GNU General Public License
      along with this program; if not, write to the Free Software
      Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_replication_connection_configuration.cc
  Table replication_connection_configuration (implementation).
*/

//#define HAVE_REPLICATION

#include "my_global.h"
#include "table_replication_connection_configuration.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "slave.h"
//#include "rpl_info.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
//#include "rpl_msr.h"             /* Multisource replciation */

#ifdef HAVE_REPLICATION
THR_LOCK table_replication_connection_configuration::m_table_lock;

PFS_engine_table_share
table_replication_connection_configuration::m_share=
{
  { C_STRING_WITH_LEN("replication_connection_configuration") },
  &pfs_readonly_acl,
  table_replication_connection_configuration::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_connection_configuration::get_row_count, /* records */
  sizeof(pos_t), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE replication_connection_configuration("
  "CHANNEL_NAME VARCHAR(256) collate utf8_general_ci not null comment 'The replication channel used.',"
  "HOST CHAR(60) collate utf8_bin not null comment 'The host name of the source that the replica is connected to.',"
  "PORT INTEGER not null comment 'The port used to connect to the source.',"
  "USER CHAR(32) collate utf8_bin not null comment 'The user name of the replication user account used to connect to the source.',"
  "USING_GTID ENUM('NO','CURRENT_POS','SLAVE_POS') not null comment 'Whether replication is using GTIDs or not',"
  "SSL_ALLOWED ENUM('YES','NO','IGNORED') not null comment 'Whether SSL is allowed for the replica connection.',"
  "SSL_CA_FILE VARCHAR(512) not null comment 'Path to the file that contains one or more certificates for trusted Certificate Authorities (CA) to use for TLS.',"
  "SSL_CA_PATH VARCHAR(512) not null comment 'Path to a directory that contains one or more PEM files that contain X509 certificates for a trusted Certificate Authority (CA) to use for TLS.',"
  "SSL_CERTIFICATE VARCHAR(512) not null comment 'Path to the certificate used to authenticate the master.',"
  "SSL_CIPHER VARCHAR(512) not null comment 'Which cipher is used for encription.',"
  "SSL_KEY VARCHAR(512) not null comment 'Path to the private key used for TLS.',"
  "SSL_VERIFY_SERVER_CERTIFICATE ENUM('YES','NO') not null comment 'Whether the server certificate is verified as part of the SSL connection',"
  "SSL_CRL_FILE VARCHAR(255) not null comment 'Path to the PEM file containing one or more revoked X.509 certificates.',"
  "SSL_CRL_PATH VARCHAR(255) not null comment 'PATH to a folder containing PEM files containing one or more revoked X.509 certificates.',"
  "CONNECTION_RETRY_INTERVAL INTEGER not null comment 'The number of seconds between connect retries.',"
  "CONNECTION_RETRY_COUNT BIGINT unsigned not null comment 'The number of times the replica can attempt to reconnect to the source in the event of a lost connection.',"
  "HEARTBEAT_INTERVAL DOUBLE(10,3) unsigned not null COMMENT 'Number of seconds after which a heartbeat will be sent.',"
  "IGNORE_SERVER_IDS LONGTEXT not null comment 'Binary log events from servers (ids) to ignore.',"
  "REPL_DO_DOMAIN_IDS LONGTEXT not null comment 'Only apply binary logs from these domain ids.',"
  "REPL_IGNORE_DOMAIN_IDS LONGTEXT not null comment 'Binary log events from domains to ignore.')") },
  false  /* perpetual */
};

static char *convert_array_to_str(DYNAMIC_ARRAY *ids)
{
  char *buf;
  size_t sz, cur_len= 0;

  sz= (sizeof(ulong) * 3 + 1) * (1 + ids->elements);

  if (!(buf= (char *) my_malloc(PSI_INSTRUMENT_ME, sz, MYF(MY_WME))))
    return NULL;
  buf[0]= 0;

  for (uint i= 0; i < ids->elements; i++)
  {
    ulong domain_id;
    get_dynamic(ids, (void *) &domain_id, i);
    cur_len+= my_snprintf(buf + cur_len, sz, (i == 0 ? "%lu" : ", %lu"), domain_id);
    sz-= cur_len;
  }
  return buf;
}

PFS_engine_table* table_replication_connection_configuration::create(void)
{
  return new table_replication_connection_configuration();
}

table_replication_connection_configuration
  ::table_replication_connection_configuration()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

table_replication_connection_configuration
  ::~table_replication_connection_configuration()
{}

void table_replication_connection_configuration::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_replication_connection_configuration::get_row_count()
{
  /*
     We actually give the MAX_CHANNELS rather than the current
     number of channels
  */

 return master_info_index->master_info_hash.records;
}

int table_replication_connection_configuration::rnd_next(void)
{
  Master_info *mi;

  mysql_mutex_lock(&LOCK_active_mi);

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < master_info_index->master_info_hash.records;
       m_pos.next())
  {
    mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index);

    if (mi && mi->host[0])
    {
      make_row(mi);
      m_next_pos.set_after(&m_pos);
      mysql_mutex_unlock(&LOCK_active_mi);
      return 0;
    }
  }

  mysql_mutex_unlock(&LOCK_active_mi);
  return HA_ERR_END_OF_FILE;
}

int table_replication_connection_configuration::rnd_pos(const void *pos)
{
  Master_info *mi;
  int res= HA_ERR_RECORD_DELETED;

  mysql_mutex_lock(&LOCK_active_mi);

  set_position(pos);

  if ((mi= (Master_info *)my_hash_element(&master_info_index->master_info_hash, m_pos.m_index)))
  {
    make_row(mi);
    res= 0;
  }

  mysql_mutex_unlock(&LOCK_active_mi);
  return res;
}

void table_replication_connection_configuration::make_row(Master_info *mi)
{
  DBUG_ENTER("table_replication_connection_configuration::make_row");
  char * temp_store;
  bool error= false;

  m_row_exists= false;


  assert(mi != NULL);

  mysql_mutex_lock(&mi->data_lock);
  mysql_mutex_lock(&mi->rli.data_lock);

  m_row.channel_name_length= static_cast<uint>(mi->connection_name.length);
  memcpy(m_row.channel_name, mi->connection_name.str, m_row.channel_name_length);

  m_row.host_length= static_cast<uint>(strlen(mi->host));
  memcpy(m_row.host, mi->host, m_row.host_length);

  m_row.port= (unsigned int) mi->port;

  /* can't the user be NULL? */
  temp_store= (char*)mi->user;
  m_row.user_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.user, temp_store, m_row.user_length);

  if (mi->using_gtid == Master_info::USE_GTID_NO)
    m_row.using_gtid= PS_USE_GTID_NO;
  else if (mi->using_gtid == Master_info::USE_GTID_CURRENT_POS)
    m_row.using_gtid= PS_USE_GTID_CURRENT_POS;
  else
    m_row.using_gtid= PS_USE_GTID_SLAVE_POS;

#ifdef HAVE_OPENSSL
  m_row.ssl_allowed= mi->ssl? PS_SSL_ALLOWED_YES:PS_SSL_ALLOWED_NO;
#else
  m_row.ssl_allowed= mi->ssl? PS_SSL_ALLOWED_IGNORED:PS_SSL_ALLOWED_NO;
#endif

  temp_store= (char*)mi->ssl_ca;
  m_row.ssl_ca_file_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_ca_file, temp_store, m_row.ssl_ca_file_length);

  temp_store= (char*)mi->ssl_capath;
  m_row.ssl_ca_path_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_ca_path, temp_store, m_row.ssl_ca_path_length);

  temp_store= (char*)mi->ssl_cert;
  m_row.ssl_certificate_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_certificate, temp_store, m_row.ssl_certificate_length);

  temp_store= (char*)mi->ssl_cipher;
  m_row.ssl_cipher_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_cipher, temp_store, m_row.ssl_cipher_length);

  temp_store= (char*)mi->ssl_key;
  m_row.ssl_key_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_key, temp_store, m_row.ssl_key_length);

  if (mi->ssl_verify_server_cert)
    m_row.ssl_verify_server_certificate= PS_RPL_YES;
  else
    m_row.ssl_verify_server_certificate= PS_RPL_NO;

  temp_store= (char*)mi->ssl_crl;
  m_row.ssl_crl_file_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_crl_file, temp_store, m_row.ssl_crl_file_length);

  temp_store= (char*)mi->ssl_crlpath;
  m_row.ssl_crl_path_length= static_cast<uint>(strlen(temp_store));
  memcpy(m_row.ssl_crl_path, temp_store, m_row.ssl_crl_path_length);

  m_row.connection_retry_interval= (unsigned int) mi->connect_retry;

  m_row.connection_retry_count= master_retry_count; //(ulong) mi->retry_count;

  m_row.heartbeat_interval= (double)mi->heartbeat_period;

  m_row.ignore_server_ids= convert_array_to_str(&mi->ignore_server_ids);
  if (m_row.ignore_server_ids == NULL)
  {
    error= true;
    goto end;
  }
  m_row.ignore_server_ids_length= static_cast<uint>(strlen(m_row.ignore_server_ids));

  m_row.do_domain_ids_str=
    convert_array_to_str(&mi->domain_id_filter.m_domain_ids[Domain_id_filter::DO_DOMAIN_IDS]);
  if (m_row.do_domain_ids_str == NULL)
  {
    error= true;
    goto end;
  }
  m_row.do_domain_ids_str_length= static_cast<uint>(strlen(m_row.do_domain_ids_str));

  m_row.ignore_domain_ids_str=
    convert_array_to_str(&mi->domain_id_filter.m_domain_ids[Domain_id_filter::IGNORE_DOMAIN_IDS]);
  if (m_row.ignore_domain_ids_str == NULL)
  {
    error= true;
    goto end;
  }
  m_row.ignore_domain_ids_str_length=
    static_cast<uint>(strlen(m_row.ignore_domain_ids_str));

end:
  mysql_mutex_unlock(&mi->rli.data_lock);
  mysql_mutex_unlock(&mi->data_lock);
  if (!error)
    m_row_exists= true;
   DBUG_VOID_RETURN;
}

int table_replication_connection_configuration::read_row_values(TABLE *table,
                                                                unsigned char *,
                                                                Field **fields,
                                                                bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  assert(table->s->null_bytes == 0);

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /** connection_name */
        set_field_varchar_utf8(f, m_row.channel_name, m_row.channel_name_length);
        break;
      case 1: /** host */
        set_field_char_utf8(f, m_row.host, m_row.host_length);
        break;
      case 2: /** port */
        set_field_ulong(f, m_row.port);
        break;
      case 3: /** user */
        set_field_char_utf8(f, m_row.user, m_row.user_length);
        break;
      case 4: /** use_gtid */
        set_field_enum(f, m_row.using_gtid);
        break;
      case 5: /** ssl_allowed */
        set_field_enum(f, m_row. ssl_allowed);
        break;
      case 6: /**ssl_ca_file */
        set_field_varchar_utf8(f, m_row.ssl_ca_file,
                               m_row.ssl_ca_file_length);
        break;
      case 7: /** ssl_ca_path */
        set_field_varchar_utf8(f, m_row.ssl_ca_path,
                               m_row.ssl_ca_path_length);
        break;
      case 8: /** ssl_certificate */
        set_field_varchar_utf8(f, m_row.ssl_certificate,
                               m_row.ssl_certificate_length);
        break;
      case 9: /** ssl_cipher */
        set_field_varchar_utf8(f, m_row.ssl_cipher, m_row.ssl_cipher_length);
        break;
      case 10: /** ssl_key */
        set_field_varchar_utf8(f, m_row.ssl_key, m_row.ssl_key_length);
        break;
      case 11: /** ssl_verify_server_certificate */
        set_field_enum(f, m_row.ssl_verify_server_certificate);
        break;
      case 12: /** ssl_crl_file */
        set_field_varchar_utf8(f, m_row.ssl_crl_file,
                               m_row.ssl_crl_file_length);
        break;
      case 13: /** ssl_crl_path */
        set_field_varchar_utf8(f, m_row.ssl_crl_path,
                               m_row.ssl_crl_path_length);
        break;
      case 14: /** connection_retry_interval */
        set_field_ulong(f, m_row.connection_retry_interval);
        break;
      case 15: /** connect_retry_count */
        set_field_ulonglong(f, m_row.connection_retry_count);
        break;
      case 16:/** number of seconds after which heartbeat will be sent */
        set_field_double(f, m_row.heartbeat_interval);
        break;
      case 17: /** ignore_server_ids */
        set_field_longtext_utf8(f, m_row.ignore_server_ids,
                               m_row.ignore_server_ids_length);
        break;
      case 18: /** do_domain_ids */
        set_field_longtext_utf8(f, m_row.do_domain_ids_str,
            m_row.do_domain_ids_str_length);
        break;
      case 19: /** ignore_domain_ids */
        set_field_longtext_utf8(f, m_row.ignore_domain_ids_str,
            m_row.ignore_domain_ids_str_length);
        break;

      default:
        assert(false);
      }
    }
  }
  m_row.cleanup();
  return 0;
}
#endif
