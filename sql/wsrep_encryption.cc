/* Copyright (C) 2019 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */\

#include "my_global.h"
#include "mysqld.h" // encrypt_binlog
#include "wsrep_encryption.h"
#include "wsrep_server_state.h"

static const unsigned int key_version_preamble_size = 8;
static bool encryption_used = false;

/**
 * Serialize key_version and key into buffer
 * 
 * @param input Container for serialized key
 * @param key Pointer to encryption key
 * @param size Size of key
 * @param version Key version
 */
static void wsrep_key_serialize(std::vector<unsigned char>& input, const void* key,
                                size_t size, unsigned int version)
{
  input.resize(key_version_preamble_size + size);
  memcpy(input.data(), (void *)&version, sizeof(version));
  memcpy(input.data() + key_version_preamble_size, key, size);
}

/**
 * De-serialize buffer into key_version, key_size & key_ptr
 * 
 * @param input Pointer to serialized key
 * @param size Size of serialized key
 * @param key_ptr Pointer to begining of encryption key
 * @param size Size of encryption key
 * @param version Key version
 * 
 * @return 0 when key is sucessfuly de-serialized. 1 otherwise.
 */
static int wsrep_key_deserialize(const void* input, const size_t& input_size,
                                 const unsigned char*& key_ptr, size_t& size,
                                 unsigned int& version)
{
  bool ret= 1;
  if (input_size >= key_version_preamble_size)
  {
    memcpy(&version, input, sizeof(version));
    key_ptr= static_cast<const unsigned char*>(input) + key_version_preamble_size;
    size= input_size - key_version_preamble_size;
    ret= 0;
  }
  return ret;
}

/**
 * Set encryption key. Serialize it and send to provider.
 * 
 * @param key Pointer to encryption key
 * @param size Length of encryption key
 * @param version Key version used
 */
static int wsrep_set_encryption_key(const void* key, size_t size, unsigned int version)
{
  std::vector<unsigned char> input;
  wsrep_key_serialize(input, key, size, version);
  return Wsrep_server_state::instance().set_encryption_key(input);
}

void wsrep_enable_encryption()
{
  unsigned char key[MY_AES_MAX_KEY_LENGTH];
  unsigned int  key_length;
  unsigned int  key_version;
  key_length= sizeof(key);
  if (encrypt_binlog)
  {
    key_version= encryption_key_get_latest_version(ENCRYPTION_KEY_SYSTEM_DATA);
    if (key_version == ENCRYPTION_KEY_VERSION_INVALID)
    {
      return;
    }
    if (key_version != ENCRYPTION_KEY_NOT_ENCRYPTED)
    {
      encryption_key_get(ENCRYPTION_KEY_SYSTEM_DATA, key_version, key, &key_length);
      if (wsrep_set_encryption_key(key, key_length, key_version))
        return;
      encryption_used= true;
    }
  }
}

int Wsrep_encryption_service::do_crypt(void**                ctx,
                                       wsrep::const_buffer&  key,
                                       const char            (*iv)[32],
                                       wsrep::const_buffer&  input,
                                       void*                 output,
                                       bool                  encrypt,
                                       bool                  last)
{

  const unsigned char* deserialized_key_ptr;
  size_t deserialized_key_size;
  unsigned int key_version;
  if (wsrep_key_deserialize(key.data(), key.size(), deserialized_key_ptr,
                            deserialized_key_size, key_version))
  {
     throw wsrep::runtime_error("Failed wsrep_key_deserialize()");
  }
  if (*ctx == NULL)
  {
    int mode= encrypt ? ENCRYPTION_FLAG_ENCRYPT : ENCRYPTION_FLAG_DECRYPT;
    *ctx = ::malloc(encryption_ctx_size(ENCRYPTION_KEY_SYSTEM_DATA,
                                        key_version));
    if (*ctx == NULL)
    {
      throw wsrep::runtime_error("Memory not allocated in do_crypt()");
    }
    if (encryption_ctx_init(*ctx,
                            deserialized_key_ptr,
                            deserialized_key_size,
                            (unsigned char*)iv, MY_AES_BLOCK_SIZE,
                            mode | ENCRYPTION_FLAG_NOPAD,
                            ENCRYPTION_KEY_SYSTEM_DATA,
                            key_version))
    {
      throw wsrep::runtime_error("Failed encryption_ctx_init()");
    }
  }
  unsigned int ctx_update_size= 0;
  if (encryption_ctx_update(*ctx, (const unsigned char*)input.data(),
                            input.size(),
                            (unsigned char *)output, &ctx_update_size))
  {
    throw wsrep::runtime_error("Failed encryption_ctx_update()");
  }
  unsigned int ctx_finish_size= 0;
  if (last)
  {
    if (encryption_ctx_finish(*ctx, (unsigned char *)output + ctx_update_size,
                              &ctx_finish_size))
    {
      throw wsrep::runtime_error("Failed encryption_ctx_finish()");
    }
    assert(ctx_update_size + ctx_finish_size == input.size());
    free(*ctx);
  }

  return ctx_update_size + ctx_finish_size;
}

bool Wsrep_encryption_service::encryption_enabled()
{
  return encryption_used;
}

