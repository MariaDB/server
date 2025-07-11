int printf(const char *, ...);

static int n = 10;

int main(void) {
	int i = 1;
	for (; n;) {
		if (n == 7) {
			n--;
			continue;
		}
		if (n == 1)
			break;
		i += n--;
	}

	return printf("%d, %d\n", i, n);
}
