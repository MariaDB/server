struct point {
	int x, y; /* Both fit in %rax */
};

struct point func(void)
{
	struct point p = {1, 2};
	return p;
}

int main () {
	struct point p;

	p = func();
	return p.x + p.y;
}
