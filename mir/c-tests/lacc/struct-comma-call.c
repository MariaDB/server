int printf(const char *, ...);

struct S3 {
	unsigned f0;
	int f1;
	float f2;
};

char f = 41;
int h = 0;

void fn2(char p1, long p2) {
	h |= p1;
}

struct S3 fn3(void) {
	struct S3 foo = {0};
	return foo;
}

int main(void) {
	fn2((0x11 & f), (fn3(), 379));
	return printf("%d\n", h);
}
