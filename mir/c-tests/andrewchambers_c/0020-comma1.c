int x;

int
foo()
{
	x = x + 1;
	return 1;
}

int
bar()
{
	x = x + 2;
	return 2;
}

main()
{
	int v;

	x = 0;
	v = foo(),bar();
	if(v != 1)
		return 1;
	if(x != 3)
		return 2;
	return 0;
}