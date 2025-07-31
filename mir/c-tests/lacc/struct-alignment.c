struct point {
#ifdef _WIN32
  long long a, b;
#else
  long a, b;
#endif
  short d;
  int c;
  char e;
};

struct foo {
  struct point point;
  char c[7];
  union {
    char c;
    void *p;
    struct {
      int i;
    } s;
  } val;
};

int main () { return sizeof (struct foo); }
