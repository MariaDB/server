typedef struct {
} Foo;
typedef struct {
  int data[0];
} Boo;

void f (Foo foo, Boo b) {}

int main () {
  Boo b = {};
  f ((Foo){}, b);
  return 0;
}
