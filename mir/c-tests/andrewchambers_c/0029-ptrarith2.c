
int x[] = {1, 2, 3};

int
main()
{
	int *p;
	
	p = &x[0];
	if(*(p + 1) != 2)
		return 1;
	if(*(2 + p) != 3)
		return 1;
	return 0;
}

