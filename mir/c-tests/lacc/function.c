int puts(const char *);

int noop(void);

static int foo(char *list[5]) {
	puts(list[0]);
	puts(list[1]);
	return sizeof(list[6]);
}

int prints(int n, ...) {
	return n;
}

static int foo(char *list[]);

char s1[] = "Hello";
char *s2 = "World";

int main() {
	int size = 0;
	char *words[2];
	words[0] = s1;
	words[1] = s2;

	size = foo(words);
	size = size + sizeof(words);
	size = size + prints(2, "hei", "hoi");

	return size;
}
