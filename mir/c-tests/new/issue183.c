typedef struct Empty {
} Empty;
static Empty empty = {};
typedef struct Foo {
  int x;
  Empty nothing;
} Foo;
static Foo f (Empty nothing) {
  Foo m = {0};
  return m;
}
int main (int argc, char** argv) {
  f (empty);
  return 0;
}
