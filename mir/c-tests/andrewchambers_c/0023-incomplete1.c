struct X x;
int foo();

int main()
{
	return 0;
}

struct X {int v;};

int foo() 
{
	x.v = 0;
	return x.v;
}