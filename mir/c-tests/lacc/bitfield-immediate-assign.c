int printf(const char *, ...);

struct {
	unsigned f1 : 2;
} b;

int foo(void) {
	int a, d = -10;
	a = (d < (b.f1 = 2));
	return printf("foo: %u, %d\n", b.f1, a);
}

int h, i;
unsigned j;

struct {
	signed f0 : 20;
	unsigned f1 : 20;
} f;

int bar(void) {
	i = (f.f0 = (h = 0x20CF9BFEL));
	j = (f.f1 = (h = 0x20CF9BFEL));
	return printf("bar: (%d, %d), (%u, %u)\n", f.f0, i, f.f1, j);
}

int main(void) {
	return foo() + bar();
}
