#include <string.h>
#include "crypto_sign.h"
#include "crypto_hash_sha512.h"
#include "ge.h"
#include "sc.h"

int crypto_sign(
  unsigned char *sm,
  const unsigned char *m,unsigned long long mlen,
  const unsigned char *pw,unsigned long long pwlen
)
{
  unsigned char az[64];
  unsigned char nonce[64];
  unsigned char hram[64];
  ge_p3 A, R;

  crypto_hash_sha512(az,pw,pwlen);
  az[0] &= 248;
  az[31] &= 63;
  az[31] |= 64;

  memmove(sm + 64,m,mlen);
  memmove(sm + 32,az + 32,32);
  crypto_hash_sha512(nonce,sm + 32,mlen + 32);

  ge_scalarmult_base(&A,az);
  ge_p3_tobytes(sm + 32,&A);

  sc_reduce(nonce);
  ge_scalarmult_base(&R,nonce);
  ge_p3_tobytes(sm,&R);

  crypto_hash_sha512(hram,sm,mlen + 64);
  sc_reduce(hram);
  sc_muladd(sm + 32,hram,az,nonce);

  return 0;
}
