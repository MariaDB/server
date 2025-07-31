int
main()
{
	int x;

	x = 0;
	x = x ? 0 : 2;
	if(x != 2)
		return 1;
	return x == 2 ? 0 : 1;
}