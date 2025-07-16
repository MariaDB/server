
struct Foo {
	int a;
	int b;
	int c;
	int d;
	int e;
	int f;
};

struct Foo
func()
{
	struct Foo v;
	v.a = 1;
	v.b = 2;
	v.c = 3;
	v.d = 4;
	v.e = 5;
	v.f = 6;
	return v;
}

int
main()
{
	struct Foo v;

	v = func();
	if(v.a != 1)
		return 1;
	if(v.b != 2)
		return 2;
	if(v.c != 3)
		return 3;
	if(v.d != 4)
		return 4;
	if(v.e != 5)
		return v.e;
	if(v.f != 6)
		return 6;
	return 0;
}
