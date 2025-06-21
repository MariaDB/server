struct T {
	struct T *p;
	int x;
};

int
main()
{
	struct T a, b;
	b.p = &a;
	b.p->x = 42;
	if(a.x != 42)
		return 1;
	return 0;
}
