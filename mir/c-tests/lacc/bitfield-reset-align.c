int printf(const char *, ...);

struct S0 {
	unsigned short f0;
	float f1;
	signed : 0;
	unsigned int f2;
	short f3;
	char f4;
	unsigned short f5;
} foo = {1};

int print(struct S0 s) {
	return printf("(%d, %f, %u, %d, %d, %d)\n",
		s.f0, s.f1, s.f2, s.f3, s.f4, s.f5);
}

int main(void) {
	struct S0 bar = {1, 2.0f};
	return print(foo)
		+ print(bar)
		+ printf("%lu\n", sizeof(struct S0));
}
