static int i;

int foo(void) {
	return i = 42;
}

int main(void) {
	(void) foo();
	return i;
}
