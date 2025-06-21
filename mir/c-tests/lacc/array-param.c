int printf(const char *s, ...);

struct obj {
	char val[8];
	int len;
};

struct obj func(struct obj in) {
	struct obj out = {{1, 2, 3}, 3};
	printf("%d, %d, %d (%d)\n", in.val[0], in.val[1], in.val[7], in.len);
	return out;
}

int main(void) {
	struct obj foo = {{5, 7, 32, 1, 4, 1, 1, 4}, 13};
	foo = func(foo);
	func(foo);
	return sizeof(foo);
}
