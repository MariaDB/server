int printf(const char *, ...);

struct fields {
	int a : 16;
	int b : 4;
	int c : 1;
	int d : 3;
};

struct fields foo(void) {
	struct fields t = {5342, 3, 0, 2};
	return t;
}

struct fields bar(void) {
	struct fields t = {87};
	return t;
}

int main(void) {
	struct fields u = foo(), v = bar();

	return printf("{%d, %d, %d, %d}, {%d, %d, %d, %d}\n",
		u.a, u.b, u.c, u.d, v.a, v.b, v.c, v.d);
}
