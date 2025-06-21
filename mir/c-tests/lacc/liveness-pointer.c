int printf(const char *, ...);

int k;
long g;
long *l = &g;

float h[] = {1.1f, 2.2f, 3.3f};

float *i = &h[1];
short j = 0;

int main(void) {
   int n = 5L;
   k = (n = (j));
   (*l) = (0x841EC489769724D4LL);
   (*i) = n;
   return printf("%f\n", h[1]);
}
