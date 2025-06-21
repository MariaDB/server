int printf(const char *s, ...);

int main(void) {
	int i = 10;

	do {
		if (i % 3 == 0)
			continue;
		if (i == 2)
			break;
		printf("%d\n", i);
	} while (i--);
	return 0;
}
