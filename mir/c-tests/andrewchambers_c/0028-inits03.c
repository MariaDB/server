
struct S {
	int a;
	int b;
	int c;
};

struct S x = {1, 2, 3};

int
main()
{
	if(x.a != 1) 
		return 1;
	if(x.b != 2) 
		return 2;
	if(x.c != 3) 
		return 3;
	return 0;
}
