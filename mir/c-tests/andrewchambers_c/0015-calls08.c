
struct Foo {
	int a;
	int b;
	int c;
	int d;
};

/* on amd64 this tests a 2 reg struct arg being demoted to memory. */
int f(int a, int b, int c, int d, int e, struct Foo s, int f)
{
	if(a != 1)
		return 1;
	if(b != 2)
		return 2;
	if(c != 3)
		return 3;
	if(d != 4)
		return 4;
	if(e != 5)
		return 5;
	if(s.a != 6)
		return 6;
	if(s.b != 7)
		return 7;
	if(s.c != 8)
		return 8;
	if(s.d != 9)
		return 9;
	if(f != 10)
		return 10;
	return 0;
}

int
main()
{
	struct Foo s;
	
	s.a = 6;
	s.b = 7;
	s.c = 8;
	s.d = 9;
	return f(1, 2, 3, 4, 5, s, 10);
}

