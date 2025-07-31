int printf(const char *, ...);

union U3 {
	int f0;
	signed f1 : 8;
} g = {0xB37CF3EAL};

int fn3(union U3 p1) { return p1.f1; }

int main(void) {
	int i = fn3(g);
	return printf("%d\n", i);
}
