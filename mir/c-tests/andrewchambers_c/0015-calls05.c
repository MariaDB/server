
struct Foo1 {
	char a;
};

int f1(struct Foo1 v1, struct Foo1 v2)
{
	return v1.a + v2.a;
}

struct Foo2 {
	int a;
	int b;
};

int f2(struct Foo2 v)
{
	return v.a + v.b;
}

int
main()
{
	struct Foo1 v1, v2;
	
	v1.a = 1;
	v2.a = 2;
	if(f1(v1, v2) != 3)
		return 1;

	struct Foo2 v;
	
	v.a = 1;
	v.b = 2;
	if(f2(v) != 3)
		return 2;
	return 0;
}

