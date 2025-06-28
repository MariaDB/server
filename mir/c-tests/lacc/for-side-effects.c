int printf(const char *, ...);

static int foo(int i) {
	return printf("%d\n", i);
}

int main(void) {
	int i;

	for (i = 0, foo(1); i < 5; i++, foo(3))
		;

	return 0;
}
