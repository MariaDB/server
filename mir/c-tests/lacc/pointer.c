int printf(const char *, ...);

static int *p, *q;

/* Based on TCC test suite. */
int compare(void) {
	int i = -1;

	q += i;
	printf("a: %d %d %d %d %d %d\n",
		p == q, p != q, p < q, p <= q, p >= q, p > q);

	i = 0xf0000000;
	p += i;
	printf("b: %p %p %ld\n", q, p, p-q);

	p = (int *)((char *)p + 0xf0000000);
	printf("c: %d %d %d %d %d %d\n",
		p == q, p != q, p < q, p <= q, p >= q, p > q);

	p += 0xf0000000;
	printf("d: %p %p %ld\n", q, p, p-q);
	printf("e: %d %d %d %d %d %d\n",
		p == q, p != q, p < q, p <= q, p >= q, p > q);
	return 0;
}

int basic(void) {
	int a;
	int *b = &a;

	*b = 2;
	a = *b;
	return a + *b;
}

int main(void) {
	return basic() + compare();
}
