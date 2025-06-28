int printf(const char *, ...);

static int c;

int foo(int n) {
	c += n;
	return 3;
}

int main(void) {
	switch (foo(42)) {
	case 1:
		return 1;
	case 2:
		return 6;
	default:
		break;
	}

	return printf("%d\n", c);
}
