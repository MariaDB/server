int
main()
{
	int x;

	x = 0;
	x += 1;
	if (x != 1)
		return 1;
	x -= 1;
	if(x != 0)
		return 1;
	x |= 3;
	if(x != 3)
		return 1;
	x &= 2;
	if(x != 2)
		return 1;
	x *= 2;
	if(x != 4)
		return 1;
	return 0;
}

