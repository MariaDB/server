int printf(const char *, ...);

int main(void) {
	int foo = 4;
	int *bar = &foo;
	float f = *bar;

	return printf("%f\n", f);
}
