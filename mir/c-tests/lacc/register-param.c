int printf(const char *, ...);

static int foo(register char const *str, register int i) {
	char c = str[0];
	return printf("%s, %d, %c\n", str, i, c);
}

int main(void) {
	int i = 42;
	return foo("Hello", i);
}
