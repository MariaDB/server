
struct S {
	int a;
	int b;
};

struct S x[1] = {{1, 2}};

int
main()
{
	if(x[0].a != 1) 
		return 1;
	if(x[0].b != 2) 
		return 2;
	return 0;
}
