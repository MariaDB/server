int printf (const char *, ...);

union {
  long long i;
  float f;
} wat;

int main (void) {
  wat.i = 0x7FFFEFFEEF;
  wat.f = (float) wat.i;
  return printf ("%f\n", wat.f);
}
