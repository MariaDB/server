void abort(void);

int foo(int n) {
	int i;

	for (i = 0; i < 5; ++i) {
		if (i == n)
			goto error;
	}

	if (i == 1)
quit:
error:
		abort();

	while (i == 0)
		regret: abort();

	return 0;
}

int main(void) {
	int i = 0;

start:
	while (i < 100) {
		if (i == 42) goto end;
		i++;
		goto start;
		i++;
	}

end:
	return i + foo(i);
}
