int printf(const char *, ...);

static int foo;

static int max(int a, int b) {
	return a + b;
}

#define SUM(a, b) (a + b) 
#define foo SUM(1, foo)

#define max(x, y) SUM(x, max(1, y))

int main(void) {
	int b = foo;
	int c = max(1, foo);
	int d = max(1, max(2, foo));
	int e = max(
		foo + 3,
		max(foo,
			foo - 2))
		+ foo;
	return printf("%d, %d, %d, %d\n", b, c, d, e);
}
