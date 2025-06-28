int foo(void) {
	int f = 2, g = 0;
	int h = (g < 7) ^ (1 || (f = 1));
	int j = (g < 7) ^ (0 && (f = 1));
	return f + h + j;
}

int bar(void) {
	int i = 1, j = 1;
	if (13L && i) {
		i = 4;
	}
	if (0 || j) {
		j = 7;
	}
	return i + j;
}

int baz() {
	int i = 1, j = 1;
	j = (0 || (j + 1));
	return i + j;
}

int main(void) {
	char a = 1, b = 2;

	int t1 = (a == 0 && (b = 0));
	int t2 = a || 1 || (b = 1);

	return b + foo() + bar() + baz();
}
