struct point {
	char c;
	struct {
		int a, b;
	} inner;
	int x, y, z, w;
};

int main()
{
	int z[][2] = {
		{ 1 }, { 2 }, { 3 }, { 4 }
	};
	struct point p = { 'a', { 1 }, 2 };
	return
		z[0][0] + z[0][1] + z[2][0] +
		p.c + p.inner.a + p.inner.b + p.x + p.w;
}
