#include <stddef.h>

int printf(const char *, ...);

struct S1 {
	int a;
	struct {
		struct {
			char b;
			short c;
		} x;
		char d;
	};
} foo = {1, 2, 3, 4};

int main(void) {
	return printf("%d, %d (+ %lu), %d (+ %lu), %d (+ %lu)\n",
		foo.a,
		foo.x.b, offsetof(struct S1, x.b),
		foo.x.c, offsetof(struct S1, x.c),
		foo.d, offsetof(struct S1, d));
}
