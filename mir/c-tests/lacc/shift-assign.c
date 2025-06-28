int main(void) {
	int a = 5;
	int b = 0x10030;

	a <<= 1;
	b >>= a;
	return a + b;
}
