int printf(const char *, ...);

struct point {
	int x, y;
};

struct data {
	int *c;
	struct point *p;
};

static int foo(struct point *p) {
	return printf("(%d, %d)\n", p->x, p->y);
}

static int bar(int *ptr) {
	return printf("%d\n", *ptr);
}

int main(void) {
	int i = 42;
	struct point p = {2, 4};
	struct data d = {0};
	d.c = &i;
	d.p = &p;
	p.x = 5;
	p.y = 9;
	i = 2;
	return foo(d.p) + bar(d.c);
}
