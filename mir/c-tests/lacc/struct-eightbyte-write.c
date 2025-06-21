int printf(const char *, ...);

struct S0 {
	float f0;
	int f1;
	float f2;
} c = {1.0f};

struct S0 *d = &c;
float e = 0xBEA32D2AL;
struct S0 f = {34.4f};

long g = 1L;
struct S0 *j = &f;
long *m = &g;

struct S0 foo(void) {
	return *d;
}

int main(void) {
	*m = 42;
	(*j) = foo();
	return printf("%ld, (%f, %d, %f)\n", g, f.f0, f.f1, f.f2);
}
