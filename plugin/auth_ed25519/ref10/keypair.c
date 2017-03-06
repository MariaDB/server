#include <string.h>
#include "crypto_sign.h"
#include "crypto_hash_sha512.h"
#include "ge.h"

int crypto_sign_keypair(
  unsigned char *pk,
  unsigned char *pw, unsigned long long pwlen
)
{
  unsigned char az[64];
  ge_p3 A;

  crypto_hash_sha512(az,pw,pwlen);
  az[0] &= 248;
  az[31] &= 63;
  az[31] |= 64;

  ge_scalarmult_base(&A,az);
  ge_p3_tobytes(pk,&A);

  return 0;
}
