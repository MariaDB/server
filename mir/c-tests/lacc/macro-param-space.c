#define	declare(name) extern int name (int)

declare(foo);

int main(void) {
	return foo(2);
}

int foo(int n) {
	return n + 1;
}
