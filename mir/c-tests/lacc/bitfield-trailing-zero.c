int printf(const char *, ...);

struct S1 {
	char c;
	int : 0;
	int : 0;
	int : 0;
};

struct S2 {
	long l;
	const signed : 0;
};

union U1 {
	int : 0;
	int : 0;
	char c;
	int : 0;
	int : 0;
	int : 0;
	int : 0;
};

struct S2 foo(int i) {
	struct S2 s = {0};
	s.l = i;
	return s;
}

int main(void) {
	struct S2 u = foo(42);
	return printf("%ld, %lu, %lu, %lu",
		u.l,
		sizeof(struct S1),
		sizeof(struct S2),
		sizeof(union U1));
}
