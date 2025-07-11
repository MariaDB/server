
struct s1 {
	int a;
	int b;
};

struct s2 {
	int a;
	int b;
	int c;
	int d;
};

struct s3 {
	int a;
	int b;
	int c;
	int d;
	int e;
	int f;
};

int
func(struct s1 a, struct s2 b, struct s3 c, struct s1 d)
{
	if(a.a != 1)
		return 1;
	if(a.b != 2)
		return 2;
	if(b.a != 3)
		return 3;
	if(b.b != 4)
		return 4;
	if(b.c != 5)
		return 5;
	if(b.d != 6)
		return 6;
	if(c.a != 7)
		return 7;
	if(c.b != 8)
		return 8;
	if(c.c != 9)
		return 9;
	if(c.d != 10)
		return 10;
	if(c.e != 11)
		return 11;
	if(c.f != 12)
		return 12;
	if(d.a != 13)
		return 13;
	if(d.b != 14)
		return 14;
	return 0;
}

int
main()
{
	struct s1 a;
	struct s2 b;
	struct s3 c;
	struct s1 d;
	
	a.a = 1;
	a.b = 2;
	b.a = 3;
	b.b = 4;
	b.c = 5;
	b.d = 6;
	c.a = 7;
	c.b = 8;
	c.c = 9;
	c.d = 10;
	c.e = 11;
	c.f = 12;
	d.a = 13;
	d.b = 14;	
	return func(a, b, c, d);
}
