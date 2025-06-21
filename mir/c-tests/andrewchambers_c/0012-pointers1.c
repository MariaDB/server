int g;

int
main()
{
	int  x;
	int *p;

	g = 1;
	x = 1;
	p = &x;
	*p = 0;
	if(x)
		return 1;
	
	p = &g;
	*p = 0;
	if(g)
		return 1;
	return 0;
}
