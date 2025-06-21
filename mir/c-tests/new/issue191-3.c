void abort (void);
typedef struct C {
  int x;
} C;
typedef struct B {
  C c;
  C cs[4];
} B;
typedef struct A {
  B b;
  B bs[4];
} A;

int main () {
  A a1 = (A){.b.c.x = 1};
  A a2 = (A){.bs[0].c.x = 1};
  A a3 = (A){.bs[0].cs[0].x = 1};
  A a4 = (A){.bs = {[0].cs[0].x = 1, [1].cs[0].x = 2}};
  if (a1.b.c.x != 1 || a2.bs[0].c.x != 1 || a3.bs[0].cs[0].x != 1 || a4.bs[0].cs[0].x != 1
      || a4.bs[1].cs[0].x != 2)
    abort ();
  return 0;
}
