/*
 Copyright (c) 2015, 2017, MariaDB Corporation.

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

/*
  The very minimal subset of OpenSSL's EVP* functions.
  Just enough for my_crypt.cc to work.

  On the other hand, where it has to implement OpenSSL functionality,
  it tries to be compatible (e.g. same flags and struct member names).
*/

#include <openssl/ssl.h>
#include "aes.hpp"

using yaSSL::yaERR_remove_state;
using yaSSL::yaRAND_bytes;

#define EVP_CIPH_ECB_MODE     0x1U
#define EVP_CIPH_CBC_MODE     0x2U
#define EVP_CIPH_NO_PADDING 0x100U

/*
  note that TaoCrypt::AES object is not explicitly put into EVP_CIPHER_CTX.
  That's because we need to control when TaoCrypt::AES constructor and
  destructor are called.
*/
typedef struct
{
  ulong flags;
  int encrypt;
  int key_len;
  int buf_len;
  int final_used;
  uchar tao_buf[sizeof(TaoCrypt::AES)];   // TaoCrypt::AES object
  uchar buf[TaoCrypt::AES::BLOCK_SIZE];   // last partial input block
  uchar final[TaoCrypt::AES::BLOCK_SIZE]; // last decrypted (output) block
} EVP_CIPHER_CTX;

typedef struct {
  TaoCrypt::Mode mode;
  TaoCrypt::word32 key_len;
} EVP_CIPHER;

#define gen_cipher(mode, MODE, len)                                     \
  static const EVP_CIPHER *EVP_aes_ ## len ## _ ## mode()               \
  { static const EVP_CIPHER c={TaoCrypt::MODE, len/8}; return &c; }

gen_cipher(ecb,ECB,128)
gen_cipher(ecb,ECB,192)
gen_cipher(ecb,ECB,256)
gen_cipher(cbc,CBC,128)
gen_cipher(cbc,CBC,192)
gen_cipher(cbc,CBC,256)

static inline TaoCrypt::AES *TAO(EVP_CIPHER_CTX *ctx)
{
  return (TaoCrypt::AES *)(ctx->tao_buf);
}

static void EVP_CIPHER_CTX_init(EVP_CIPHER_CTX *ctx)
{
  ctx->final_used= ctx->buf_len= ctx->flags= 0;
}

static int EVP_CIPHER_CTX_cleanup(EVP_CIPHER_CTX *ctx)
{
  TAO(ctx)->~AES();
  return 1;
}

static int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int pad)
{
  if (pad)
    ctx->flags&= ~EVP_CIPH_NO_PADDING;
  else
    ctx->flags|= EVP_CIPH_NO_PADDING;
  return 1;
}

static int EVP_CipherInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *cipher,
                             void *, const uchar *key, const uchar *iv, int enc)
{
  new (ctx->tao_buf) TaoCrypt::AES(enc ? TaoCrypt::ENCRYPTION
                                       : TaoCrypt::DECRYPTION, cipher->mode);
  TAO(ctx)->SetKey(key, cipher->key_len);
  if (iv)
    TAO(ctx)->SetIV(iv);
  ctx->encrypt= enc;
  ctx->key_len= cipher->key_len;
  ctx->flags|= cipher->mode == TaoCrypt::CBC ? EVP_CIPH_CBC_MODE : EVP_CIPH_ECB_MODE;
  return 1;
}

static int EVP_CIPHER_CTX_key_length(const EVP_CIPHER_CTX *ctx)
{
  return ctx->key_len;
}

static int EVP_CIPHER_CTX_iv_length(const EVP_CIPHER_CTX *ctx)
{
  return ctx->flags & EVP_CIPH_ECB_MODE ? 0 : TaoCrypt::AES::BLOCK_SIZE;
}

static void do_whole_blocks(EVP_CIPHER_CTX *ctx, uchar *out, int *outl,
                            const uchar *in, int inl)
{
  DBUG_ASSERT(inl);
  DBUG_ASSERT(inl % TaoCrypt::AES::BLOCK_SIZE == 0);
  if (ctx->encrypt || (ctx->flags & EVP_CIPH_NO_PADDING))
  {
    TAO(ctx)->Process(out, in, inl);
    *outl+= inl;
    return;
  }
  /* 'final' is only needed when decrypting with padding */
  if (ctx->final_used)
  {
    memcpy(out, ctx->final, TaoCrypt::AES::BLOCK_SIZE);
    *outl+= TaoCrypt::AES::BLOCK_SIZE;
    out+= TaoCrypt::AES::BLOCK_SIZE;
  }
  inl-= TaoCrypt::AES::BLOCK_SIZE;
  TAO(ctx)->Process(out, in, inl);
  *outl+= inl;
  TAO(ctx)->Process(ctx->final, in + inl, TaoCrypt::AES::BLOCK_SIZE);
  ctx->final_used= 1;
}

static int EVP_CipherUpdate(EVP_CIPHER_CTX *ctx, uchar *out, int *outl,
                            const uchar *in, int inl)
{
  *outl= 0;
  if (ctx->buf_len)
  {
    int prefixl= TaoCrypt::AES::BLOCK_SIZE - ctx->buf_len;
    if (prefixl > inl)
    {
      memcpy(ctx->buf + ctx->buf_len, in, inl);
      ctx->buf_len+= inl;
      return 1;
    }
    memcpy(ctx->buf + ctx->buf_len, in, prefixl);
    do_whole_blocks(ctx, out, outl, ctx->buf, TaoCrypt::AES::BLOCK_SIZE);
    in+= prefixl;
    inl-= prefixl;
    out+= *outl;
  }
  ctx->buf_len= inl % TaoCrypt::AES::BLOCK_SIZE;
  inl-= ctx->buf_len;
  memcpy(ctx->buf, in + inl, ctx->buf_len);
  if (inl)
    do_whole_blocks(ctx, out, outl, in, inl);
  return 1;
}

static int EVP_CipherFinal_ex(EVP_CIPHER_CTX *ctx, uchar *out, int *outl)
{
  if (ctx->flags & EVP_CIPH_NO_PADDING)
    return ctx->buf_len == 0;

  // PKCS#7 padding
  *outl= 0;
  if (ctx->encrypt)
  {
    int v= TaoCrypt::AES::BLOCK_SIZE - ctx->buf_len;
    memset(ctx->buf + ctx->buf_len, v, v);
    do_whole_blocks(ctx, out, outl, ctx->buf, TaoCrypt::AES::BLOCK_SIZE);
    return 1;
  }
  int n= ctx->final[TaoCrypt::AES::BLOCK_SIZE - 1];
  if (ctx->buf_len || !ctx->final_used ||
      n < 1 || n > TaoCrypt::AES::BLOCK_SIZE)
    return 0;
  *outl= TaoCrypt::AES::BLOCK_SIZE - n;
  memcpy(out, ctx->final, *outl);
  return 1;
}

