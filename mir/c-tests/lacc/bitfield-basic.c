int printf(const char *, ...);

union A {
	int:3;
	char b;
} m = {'b'};

union {
	int f0;
	unsigned f1 : 25;
	signed f2 : 4;
} f[] = {0x35CEB2D7L};

struct fields {
	signed int foo:7;
	unsigned int bar: (1 + 1);
	unsigned int:0;
	unsigned int baz:4;
	int:0;
};

int main(void) {
	struct fields test = {0};
	struct fields *ref = &test;

	test.foo = 127;
	test.baz = ref->foo;
	ref->bar = 3;
	ref->foo += 1;
	test.bar += 1;

	printf("size: %lu\n", sizeof(test));
	printf("%d, %d, %d\n", test.foo, ref->bar, test.baz);
	printf("f: %d, %d, %d\n", f[0].f0, f[0].f1, f[0].f2);
	printf("union: {%d} :: %lu\n", m.b, sizeof(union A));

	return 0;
}
