#include <stddef.h>

int printf(const char *, ...);

struct point {
	int x;
	long y;
};

int main(void) {
	return printf("%lu\n", offsetof(struct point, y));
}
