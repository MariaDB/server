
int
foo(int a, int b, int c, int d, int e, int f,
	int g, int h, int i, int j, int k, int l)
{
	if(a != 1)
		return 0;
	if(f != 2)
		return 0;
	if(l != 2)
		return 0;
	return a+b+c+d+e+f+g+h+i+j+k+l;
}

int
main()
{
	int x;

	x = foo(1,1,1,1,1,2,1,1,1,1,1,2);
	if(x != 14)
		return 1;
	return 0;
}