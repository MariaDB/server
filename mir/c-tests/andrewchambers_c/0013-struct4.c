struct T {
	int x;
	int y;
};

int
main()
{
	struct T v, *p;
	
	p = &v;
	v.y = 2;
	if(p->y != 2)
		return 1;
	return 0;
}
