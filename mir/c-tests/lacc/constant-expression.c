#define isbit(b) ((b) < 8 ? ((1 << (b)) << 8) : ((1 << (b)) >> 8))

static int a = (2 >= 5);

enum wat {
	CAT = isbit(1)
};

int main(void) {
	return a + CAT;
}
