int printf(const char *, ...);

static int a = 2, b = 3, c = 4;

int main(void) {
	int d = !(a == b);
	int e = !(c - 1 != b);
	int f = !(a > e);
	int g = !(f <= c);

	return printf("%d, %d, %d, %d\n", d, e, f, g);
}
