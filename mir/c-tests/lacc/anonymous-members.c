#include <stddef.h>

int printf(const char *, ...);

struct A {
	int a;
	union {
		int b;
		char c;
	};
} foo = {0};

union B {
	int a;
	struct {
		int b;
		char c;
	};
} bar = {42};

struct C {
	union {
		struct {
			int i, j;
		};
		struct {
			long k, l;
		} w;
	};
	int m;
} v1;

int main(void) {
	printf("struct A: %lu (%lu, %lu, %lu)\n",
		sizeof(foo),
		offsetof(struct A, a),
		offsetof(struct A, b),
		offsetof(struct A, c));

	printf("union B: %lu (%lu, %lu, %lu)\n",
		sizeof(bar),
		offsetof(union B, a),
		offsetof(union B, b),
		offsetof(union B, c));

	printf("struct C: %lu (i:%lu, j:%lu, w.k:%lu, w.l:%lu, m:%lu)\n",
		sizeof(v1),
		offsetof(struct C, i),
		offsetof(struct C, j),
		offsetof(struct C, w.k),
		offsetof(struct C, w.l),
		offsetof(struct C, m));

	return 0;
}
