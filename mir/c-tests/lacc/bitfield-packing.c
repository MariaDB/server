int printf(const char *, ...);

struct fields {
	int : 31;
	signed int a : 7;
	int : 3;
	int b : 2;
	int : 26;
	int : 31;
	unsigned int c : (1 + 7);
	unsigned int : 0;
	unsigned int d : 4;
} f = {-1, 1, 0x56, 3};

int print_values(int n) {
	return printf("%d: {a = %d, b = %d, c = %d, d = %d}\n", n,
		f.a, f.b, f.c, f.d);
}

int main(void) {
	int *ref = (int *) &f;

	print_values(0);

	ref[0] = 0x12345678;
	ref[1] = 0xABCDEF01;
	ref[2] = 0x98172534;
	ref[3] = 0x62738452;
	ref[4] = 0x01923475;
	ref[5] = 0x49130626;

	print_values(1);

	return sizeof(struct fields);
}
