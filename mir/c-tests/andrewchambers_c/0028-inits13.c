
struct S {
	int a;
	struct {
		int b;
		int c;
	};
	int d;
};

struct S s = {.a = 1, 2, .c = 3, 4};

int
main()
{
	if(s.a != 1)
		return 1;
	if(s.b != 2)
		return 2;
	if(s.c != 3)
		return 3;
	if(s.d != 4)
		return 4;
	return 0;
}
