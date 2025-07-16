int printf(const char *, ...);

typedef struct Foo Bar;

int main(void) {
	int bar[5];
	long a = sizeof(volatile char) + sizeof(bar);
	long b = sizeof (a = a + 2);
	unsigned long c = sizeof(Bar *);
	unsigned long d = sizeof(int (*)(char)) + sizeof(struct {char a;});

	printf("sizeof(sizeof(int)) = %lu\n", sizeof(sizeof(int)));
	printf("sizeof a = %lu", sizeof a);
	printf("a = %ld, b = %ld, c = %lu, d = %lu\n", a, b, c, d);
	return 0;
}
