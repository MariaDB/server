int printf(const char *, ...);

struct {
	unsigned f2 : 13;
	signed f3 : 19;
	int f5;
	signed f6 : 17;
	signed f7 : 7;
} f = {30, 374, 1UL, -269, -2};

int main(void) {
	return printf("{%u, %d, %d, %d, %d} size=%lu\n",
		f.f2, f.f3, f.f5, f.f6, f.f7, sizeof(f));
}
