struct point {
	int x, y;	/* %rax */
	long l;		/* %rdx */
};

struct point func(void)
{
	struct point p = {1, 2, 3};
	return p;
}

int main() {
	struct point p;

	p = func();
	return p.x + p.y + p.l;
}
