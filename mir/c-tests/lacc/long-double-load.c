int printf(const char *, ...);

static long double arr[] = {2.9L, 3.14L, 3, 2.1, 4.8f};
static long double *ref = &arr[3];

int test_array(void) {
	int i;
	long double *ptr = ref;

	printf("%Lf\n", *ptr);
	*ptr = arr[3];
	printf("%Lf\n", *ptr);
	for (i = 0; i < 4; ++i) {
		arr[i] += arr[i + 1];
		printf("%d: %Lf\n", i, arr[i]);
	}

	return printf("%Lf\n", *ref);
}

int test_cast_signed(void) {
	char c1 = 'a';
	short c2 = 0x5768;
	int c3 = 0x76959876;
	long c4 = 0x0976589757652309;
	long double d1 = c1, d2 = c2, d3 = c3, d4 = c4;
	return printf("signed: %Lf, %Lf, %Lf, %Lf\n", d1, d2, d3, d4);
}

int test_cast_unsigned(void) {
	unsigned char c1 = 'a';
	unsigned short c2 = 0xF768u;
	unsigned int c3 = 0xF6959876u;
	unsigned long c4 = 0xF976589757652309ul;
	long double d1 = c1, d2 = c2, d3 = c3, d4 = c4;
	return printf("unsigned: %Lf, %Lf, %Lf, %Lf\n", d1, d2, d3, d4);
}

int main(void) {
	test_array();
	test_cast_signed();
	test_cast_unsigned();
	return 0;
}
