#include <stdarg.h>
#include <stdio.h>

int sum(int n, ...) {
	int value = 0, i;
	va_list args;

	va_start(args, n);
	for (i = 0; i < n; ++i)
		value += va_arg(args, int);

	va_end(args);

	return value;
}

struct car {
	char a, b, c;
};

int foo(int n, ...) {
	struct car c;
	va_list args;

	va_start(args, n);
	c = va_arg(args, struct car);
	n = c.a + c.b + c.c;
	va_end(args);
	return n;
}

int main(void) {
	struct car c = {1, 2, 3};
	printf("sum: %d\n", sum(7, 10, 8, 42, 1, 2, 3, 4));
	printf("car: %d\n", foo(1, c));
	return 0;
}
