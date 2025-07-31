/* TODO: more types */

int
main()
{
	int x;

	x = 0;
	x = x + 2;        // 2
	x = x - 1;        // 1
	x = x * 6;        // 6
	x = x / 2;        // 3
	x = x % 2;        // 1
	x = x << 2;       // 4
	x = x >> 1;       // 2
	x = x | 255;      // 255
	x = x & 3;        // 3
	x = x ^ 1;        // 2
	if(x != 2)
		return 1;
	if(!(x == 2))
		return 2;
	if(x == 3)
		return 3;
	if(x != 2)
		return 4;
	if(!(x != 3))
		return 5;
	if(x < 1)
		return 6;
	if(x < 2)
		return 7;
	if(!(x < 3))
		return 8;
	if(!(x > 1))
		return 9;
	if(x > 2)
		return 10;
	if(x > 3)
		return 11;
	if(x <= 1)
		return 12;
	if(!(x <= 2))
		return 13;
	if(!(x <= 3))
		return 14;
	if(!(x >= 1))
		return 15;
	if(!(x >= 2))
		return 16;
	if(x >= 3)
		return 17;
	return 0;

}

