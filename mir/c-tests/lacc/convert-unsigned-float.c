int printf (const char *, ...);

unsigned char a = 0xF1u, b = 0x89;
unsigned c = 0xC2E0CF57u, d = 0x678;
unsigned long long e = 0xC2E0CF57C2E0CF57UL, f = 0x456789L;

int main (void) {
  float f1 = a, f2 = b, f3 = c, f4 = d, f5 = e, f6 = f;
  double d1 = a, d2 = b, d3 = c, d4 = d, d5 = e, d6 = f;

  return printf ("(%f, %f, %f, %f, %f, %f), (%f, %f, %f, %f, %f, %f)\n", f1, f2, f3, f4, f5, f6, d1,
                 d2, d3, d4, d5, d6);
}
