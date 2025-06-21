#define A() int main (void) { return 0; }
#if defined(__has_attribute) && __has_attribute(availability)
#endif

A();
