union flags {
	signed a : 13;
	const signed b : 1;
	unsigned long c;
};

union {
	long f0;
	signed f1 : 9;
} g = {1L};

int main(void) {
	union flags f = {2};
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return f.b + f.c + g.f1 != 3;
#else
	return f.b + f.c + g.f1 != 0x10000000000000;
#endif
}
