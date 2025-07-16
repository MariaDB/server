int main(void) {
	int a, b = 42;
	a = (1) ? (b+1) & ~1 : b;
	b = (0) ? (b+1) & ~1 : b;
	return a + b;
}
