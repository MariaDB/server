struct Empty {};
void abort(void);
struct Empty
f(struct Empty e1, int a, struct Empty e2, int b, struct Empty e3, int c, int d, int e, int f, int g)
{
	if(a != 1)
		abort();
	if(b != 2)
		abort();
	if(c != 3)
		abort();
	if(d != 4)
		abort();
	if(e != 5)
		abort();
	if(f != 6)
		abort();
	if(g != 7)
		abort();
	return e1;
}

int
main()
{
	struct Empty e;
	e = f(e, 1, e, 2, e, 3, 4, 5, 6, 7);
	return 0;
}
