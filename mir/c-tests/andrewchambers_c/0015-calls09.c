
struct Foo {
	int a;
};

struct Foo
func()
{
	struct Foo v;
	v.a = 123;
	return v;
}

int
main()
{
	struct Foo v;
	v = func();
	if(v.a != 123)
		return 1;
	return 0;
}
