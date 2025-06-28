int printf(const char *, ...);
int h = 0xC3F30B91L;

int main(void) {
  	int f = (0x47C1471CL > (0x7DL ^ h));
  	printf("%d\n", f);
	return printf("%d, %d, %d, %f, %f, %d\n",
		(0xF2C889DD98AE1F63LL >= 0xAD2086BDL),
		(0x000010001D2086BDLL == 0x1D2086BDL),
		(0xF2C889DD98AE1F63LL > 0xAD2086BDL),
		8.5867834283 * 3.14f,
		8.5867834283 / 3.14f,
		3.14 == 3.14f);
}
