int printf (const char *, ...);

static int unsigned_char (void) {
  unsigned char a = 0xE8u;
  char b = 2;

  return printf ("%lu, %d, %d\n", sizeof (a << b), a << b, a >> b);
}

static int signed_char (void) {
  signed char a = 0x65;
  int b = 3;

  return printf ("%lu, %d, %d\n", sizeof (a << b), a << b, a >> b);
}

static int signed_long (void) {
  long long a = 0x7E00FF0F;
  short b = 33;

  return printf ("%lu, %lld, %lld\n", sizeof (a << b), a << b, a >> 4);
}

static int chained (void) {
  unsigned a = 0x7EABCDF4;
  return printf ("%u\n", a << 3 >> 1);
}

static int overflow (void) {
  long long a, b = 0x567895;
  a = 0x567895 << 10;
  b = b << 10;
  return printf ("%lld, %lld\n", a, b);
}

int main (void) {
  return unsigned_char () + signed_char () + signed_long () + chained () + overflow ();
}
