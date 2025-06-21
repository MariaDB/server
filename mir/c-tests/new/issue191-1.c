typedef struct Boo {
  float x;
} Boo;

typedef struct Foo {
  Boo data[4];
} Foo;

void abort (void);
int main () {
  Foo foo = (Foo){.data[1] = {42}};
  if (foo.data[1].x != 42) abort ();
  return 0;
}
