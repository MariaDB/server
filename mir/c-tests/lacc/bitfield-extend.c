struct fields {
  signed int foo : 5;
  unsigned int bar : 1;
};

int main (void) {
  struct fields test = {0};

  test.foo--;
  test.bar = 1;

  return (unsigned char) (test.foo - test.bar) != 254;
}
