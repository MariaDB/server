int foo() {
	short k = (0xDFB2L);
	long i = (((0x5405EA32 / (k))));
	return i;
}

long baz(void) {
	char s = 'a';
	return s / 3L;
}

int main() {
	short k = (0xDFB2L);
	int i = (((0x5405EA32L / (k))));
	return i + foo() + baz();
}
