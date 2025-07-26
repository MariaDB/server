int printf(const char *, ...);

long g = 0xE52DL;

int main(void) {
	int k = (-(unsigned)(2UL)) % g;
	unsigned j = (unsigned char) (-2ul);
	int i = (char) (-2458658ul);
	return printf("%d, %u, %d\n", k, j, i);
}
