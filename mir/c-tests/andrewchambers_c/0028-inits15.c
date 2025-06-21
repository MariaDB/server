

struct S {
	int a;
	struct {
		int b;
		int c;
		struct {
			int d;
			struct {
				int e;
			};
		};
		struct {
			int x;
			struct {
				int y;
			};
		} f;
		int g;
	};
};
struct S s = {1, 2, 3, 4, 5, {6, 7}, 8};

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
	if(s.e != 5)
		return 5;
	if(s.f.x != 6)
		return 6;
	if(s.f.y != 7)
		return 7;
	if(s.g != 8)
		return 8;
	return 0;
}
