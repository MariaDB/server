int printf(const char *, ...);

int main(void) {
	int a = -1;
	int b = 2;

	int c = a == b == 2;
	int d = c < 2 > c >= 1;
	int e = d && c || 1;
	int f = (1, 2);

	int g[2];

	c = !c + +a != -7;
	(g)[b - 1] = 1;

	printf("%lu\n", sizeof(+(char) a));

	return - -c + d + e + f;
}
