static int
	a = 3 << 3,
	b = 211 >> 2,
	c = 5 | 0,
	d = 7 & 3,
	e = ~5,
	f = !0;

int main(void) {
	return a + b + c + d + e + f;
}
