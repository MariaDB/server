/* Too large to fit in two registers, pass address of result object in %rdi. */
struct mem {
	int x, y;
	char c;
	long l;
};

struct mem func(void)
{
	struct mem p = {1, 2, 'a', 3};
	return p;
}

int main() {
	struct mem p;

	p = func();
	return p.x + p.y + p.c + p.l;
}
