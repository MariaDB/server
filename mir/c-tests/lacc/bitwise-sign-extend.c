int printf (const char *, ...);

char f = -1L;
long long g = 0x9D3BBD11537C3E80LL;

int main (void) { return printf ("%lld, %lld, %lld\n", (g & f), (g ^ f), (g | f)); }
