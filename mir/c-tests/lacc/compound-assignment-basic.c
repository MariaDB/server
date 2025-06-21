int main() {
	int a = 1, b = 2, c = 3, d = 4, e = 5;
	int f = 0x03, g = 0x03, h = 0x07;

	a += b;
	c -= c += d;
	e *= 2;
	e /= 5;
	d %= a - 1;
	f &= 0x11;
	g |= 0x11;
	h ^= 0x11;

	return a + b + c + d + e + f + g + h;
}
