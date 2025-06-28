int printf(char *, ...);

static int g;

long fn8(void) {
	return 0x800000000;
}

short fn2(void) {
	return (short)0x70000;
}

char fn1(void) {
	return (char)0x600;
}

int main(void) {
	if (fn8()) {
		g += 8;
	}

	if (fn2()) {
		g += 2;
	}

	if (fn1()) {
		g += 1;
	}

	return printf("%d\n", g);
}
