int printf(const char *, ...);

int foo();

int bar(void) {
	return foo() + 1;
}

int foo(char *n) {
	return 42;
}

int (*baz)() = &foo;

int main(void) {
	int a = bar();
	return printf("%d\n", a);
}
