

union U {
	int a;
	struct {
		int b;
		int c;
	};
	int d;
};

union U u = {.b = 1, 2};

int
main()
{
	if(u.a != 1)
		return 1;
	if(u.b != 1)
		return 2;
	if(u.c != 2)
		return 3;
	if(u.d != 1)
		return 4;
	return 0;
}
