/* test for enum const scope */
struct {
  enum { DEPS_NONE = 0, DEPS_USER, DEPS_SYSTEM } style;
} s;

int main (void) { return DEPS_NONE != 0; }
