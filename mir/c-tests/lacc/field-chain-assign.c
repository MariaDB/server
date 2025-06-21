int printf(const char *, ...);

struct a {
  signed b : 3;
};

int c = 3680314112;
int *d = &c;

int main(void) {
  struct a e;
  *d = e.b = c;
  printf("{%d}\n", c);
  return 0;
}
