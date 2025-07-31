int puts(const char *);

static char *str;

static void func(void) {
	puts(str);
}

static void (*getfunc(char *s))(void) {
	str = s;
	return func;
}

int arr[] = {1, 2};

int bar(void) {
	int *(a[2]);

	a[0] = &arr[0];
	a[1] = &arr[1];
	return *a[0] + *a[1];
}

int main(void) {
	void (*foo)(void) = getfunc("Hello World!");
	foo();
	return bar();
}
