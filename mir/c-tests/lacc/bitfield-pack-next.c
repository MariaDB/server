int printf(const char *, ...);

struct id {
	unsigned char hash[10];
};

struct obj {
	unsigned int a : 4;
	unsigned int b : 4;
	struct id oid;
};

static int foo(struct obj *o) {
	char *arr = (char *) o;
	int i;
	for (i = 0; i < sizeof(*o); ++i) {
		printf("%d ", arr[i]);
	}

	printf(" (%lu)\n", sizeof(*o));
	return 0;
}

int main(void) {
	struct obj o = {0x3, 0x4, {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'}};
	return foo(&o);
}
