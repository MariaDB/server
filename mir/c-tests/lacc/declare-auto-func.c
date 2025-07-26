int main(void) {
	int a = 42, foo(int a), b = 2;
	return foo(a + b);
}

int bar(void) {
	int foo(int b);
	return foo(4);
}

int foo(int n) {
	int baz(int);
	return baz(n);
}

int baz(int a) {
	return a + a;
}
