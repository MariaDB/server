int printf(const char *, ...);

char foo[5] = "Hei";
char bar[][6] = {"Hello", '\0'};
char *baz = {"Hello" + 1};

void check(void) {
	printf("foo: %s, %lu\n", foo, sizeof(foo));
	printf("bar[0]: %s, bar[1]: %s, size: %lu\n", bar[0], bar[1], sizeof(bar));
	printf("baz: %s, %lu\n", baz, sizeof(baz));
}

void func(void) {
	char a[7] = ("wat");
	char b[3] = {"Hello"[1], 46};
	char c[] = {"World" "hello"};

	printf("b = {%c, %c, %c}, size = %lu\n", b[0], b[1], b[2], sizeof(b));
	printf("a = %s, size = %lu\n", a, sizeof(a));
	printf("c = %s, size = %lu\n", c, sizeof(c));
}

int main(void) {
	check();
	func();
	return 0;
}
