
struct Foo {
	int a;
	int b;
	int c;
	int d;
	int e;
};

int f(struct Foo s)
{
	if(s.a != 1)
		return 1;
	if(s.b != 2)
		return 2;
	if(s.c != 3)
		return 3;
	if(s.d != 4)
		return 4;
	if(s.e != 5)
		return 5;
	return 0;
}

int
main()
{
	struct Foo s;
	
	s.a = 1;
	s.b = 2;
	s.c = 3;
	s.d = 4;
	s.e = 5;
	return f(s);
}

