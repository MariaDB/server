static int foo(int n) {
	int i = 0;

	if (n > 0)
		do {
			i++;
		} while (i < n);
	else
		i = 42;
	return i;
}

int main(void) {
	return foo(0) + foo(1) + foo(13);
}
