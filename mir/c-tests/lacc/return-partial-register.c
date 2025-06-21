int printf(const char *, ...);

struct point {
	char a;
	char b;
	char c;
};

struct point foo(int a) {
	struct point p = {0};
	p.a = a;
	return p;
}

struct point bar(struct point p) {
	struct point q;;
	q.a = p.a;
	q.b = p.b;
	q.c = p.c;
	return q;
}

int main(void) {
	struct point
		p = foo(42),
		q = bar(p);
	return printf("(%d, %d, %d), (%d, %d, %d)\n",
		p.a, p.b, p.c, q.a, q.b, q.c);
}
