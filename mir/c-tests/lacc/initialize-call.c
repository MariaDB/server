#include <stdio.h>
#include <stdarg.h>

static int id(int x) {
	return x;
}

void test1(int a, int b) {
	char x[] = {id(a), id(b + a)};
	printf("{%d, %d}\n", x[0], x[1]);
}

struct obj {
	short s[3];
};

static struct obj getobj(int a, int b, int c) {
	struct obj x = {id(a), (char) id(b) + id(1), id(c)};
	return x;
}

void test2(void) {
	struct {
		struct obj t;
	} x = {getobj(2, 8, 1)};
	printf("{%d, %d, %d}\n", x.t.s[0], x.t.s[1], x.t.s[2]);
}

void test3(int n, ...) {
	va_list args;

	va_start(args, n);
	{
		struct {
			char c;
			char x[3];
		} s = {'a', va_arg(args, int), va_arg(args, int), va_arg(args, int)};
		printf("{%c, {%c, %c, %c}}\n", s.c, s.x[0], s.x[1], s.x[2]);
	}
	va_end(args);
}

int main(void) {
	test1(4, 3);
	test2();
	test3(3, 'w', 'a', 't');
	return 0;
}
