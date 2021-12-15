/* Copyright (c) 2007 MySQL AB, 2008 Sun Microsystems, Inc.
   Use is subject to license terms.

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

#ifndef RPL_CONSTANTS_H
#define RPL_CONSTANTS_H

#include <my_sys.h>
#include <my_crypt.h>

/**
   Enumeration of the incidents that can occur for the server.
 */
enum Incident {
  /** No incident */
  INCIDENT_NONE = 0,

  /** There are possibly lost events in the replication stream */
  INCIDENT_LOST_EVENTS = 1,

  /** Shall be last event of the enumeration */
  INCIDENT_COUNT
};


/**
   Enumeration of the reserved formats of Binlog extra row information
*/
enum ExtraRowInfoFormat {

  /** Reserved formats  0 -> 63 inclusive */
  ERIF_LASTRESERVED =  63,

  /**
      Available / uncontrolled formats
      64 -> 254 inclusive
  */
  ERIF_OPEN1        =  64,
  ERIF_OPEN2        =  65,

  ERIF_LASTOPEN     =  254,

  /**
     Multi-payload format 255

      Length is total length, payload is sequence of
      sub-payloads with their own headers containing
      length + format.
  */
  ERIF_MULTI        =  255
};

/*
   1 byte length, 1 byte format
   Length is total length in bytes, including 2 byte header
   Length values 0 and 1 are currently invalid and reserved.
*/
#define EXTRA_ROW_INFO_LEN_OFFSET 0
#define EXTRA_ROW_INFO_FORMAT_OFFSET 1
#define EXTRA_ROW_INFO_HDR_BYTES 2
#define EXTRA_ROW_INFO_MAX_PAYLOAD (255 - EXTRA_ROW_INFO_HDR_BYTES)

enum enum_binlog_checksum_alg {
  BINLOG_CHECKSUM_ALG_OFF= 0,    // Events are without checksum though its generator
                                 // is checksum-capable New Master (NM).
  BINLOG_CHECKSUM_ALG_CRC32= 1,  // CRC32 of zlib algorithm.
  BINLOG_CHECKSUM_ALG_ENUM_END,  // the cut line: valid alg range is [1, 0x7f].
  BINLOG_CHECKSUM_ALG_UNDEF= 255 // special value to tag undetermined yet checksum
                                 // or events from checksum-unaware servers
};

#define BINLOG_CRYPTO_SCHEME_LENGTH 1
#define BINLOG_KEY_VERSION_LENGTH   4
#define BINLOG_IV_LENGTH            MY_AES_BLOCK_SIZE
#define BINLOG_IV_OFFS_LENGTH       4
#define BINLOG_NONCE_LENGTH         (BINLOG_IV_LENGTH - BINLOG_IV_OFFS_LENGTH)

struct Binlog_crypt_data {
  uint  scheme;
  uint  key_version, key_length, ctx_size;
  uchar key[MY_AES_MAX_KEY_LENGTH];
  uchar nonce[BINLOG_NONCE_LENGTH];

  int init(uint sch, uint kv)
  {
    scheme= sch;
    ctx_size= encryption_ctx_size(ENCRYPTION_KEY_SYSTEM_DATA, kv);
    key_version= kv;
    key_length= sizeof(key);
    return encryption_key_get(ENCRYPTION_KEY_SYSTEM_DATA, kv, key, &key_length);
  }

  void set_iv(uchar* iv, uint32 offs) const
  {
    memcpy(iv, nonce, BINLOG_NONCE_LENGTH);
    int4store(iv + BINLOG_NONCE_LENGTH, offs);
  }
};

#endif /* RPL_CONSTANTS_H */
