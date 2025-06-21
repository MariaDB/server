#define foo(s) s + 1

int main(void) {
	int foo = 4;
	return foo(1) + foo;
}
