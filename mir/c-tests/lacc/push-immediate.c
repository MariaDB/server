int printf(const char *, ...);

int foo(int a, int b, int c, int d, int e, int f, unsigned int g) {
	return printf("%d, %d, %d, %d, %d, %d, %u\n", a, b, c, d, e, f, g);
}

int main(void) {
	return foo(1, 2, 3, 4, 5, 6, 0xFF000000u);
}
