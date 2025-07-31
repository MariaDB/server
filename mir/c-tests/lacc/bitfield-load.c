int printf(const char *, ...);

union {
	int f1;
	signed f0 : 23;
} foo = {0xE2D5F285L};

int f = 0;

union {
	int f0 : 3;
	unsigned f1 : 2;
} b = {0x04};

int out(int n, unsigned long v) {
	return printf("%d: %lu\n", n, v);
}

int cast(void) {
	float f = foo.f0;
	double d = foo.f0;
	return printf("%f, %f\n", f, d);
}

int main(void) {
	if (b.f1) f += 1;
	if (b.f0 > 0) f += 2;

	out(1, foo.f1);
	out(2, b.f0);
	out(3, b.f1);

	cast();

	return printf("%d\n", f);
}
