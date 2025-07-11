int printf (const char *, ...);

struct point {
  long long x, y;
} p;

static long long offset = (long long) &((struct point *) 0x8)->y;

int main (void) {
  int *ptr = &*((int *) 0x601044);
#ifdef _WIN32
  return printf ("%p, %lld\n", ptr, offset) != 21; /* head zeros */
#else
  return printf ("%p, %lld\n", ptr, offset) != 13;
#endif
}
