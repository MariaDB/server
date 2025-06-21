int
main()
{
	int x;
	
	x = 0;
	if(x++)
		return 1;
	if(x != 1)
		return 2;
	if(++x != 2)
		return 3;
	if(x != 2)
		return 4;
	if(--x != 1)
		return 5;
	if(x != 1)
		return 6;
	if(x-- != 1)
		return 7;
	if(x != 0)
		return 8;
	return 0;
}
