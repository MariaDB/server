
struct Foo {
	int a;
	int b;
};

int
main()
{
	struct Foo v1, v2;
	
	v1.a = 1;
	v1.b = 6;
	v2 = v1;
	if(v2.a != 1)
		return 1;
	if(v2.b != 6)
		return 2;
	return 0;
}
