int crypto_sign_keypair(
  unsigned char *pk,
  unsigned char *pw, unsigned long long pwlen
);
int crypto_sign(
  unsigned char *sm,
  const unsigned char *m, unsigned long long mlen,
  const unsigned char *pw, unsigned long long pwlen
);
int crypto_sign_open(
  unsigned char *sm, unsigned long long smlen,
  const unsigned char *pk
);
