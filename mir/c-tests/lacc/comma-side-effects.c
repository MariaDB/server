int printf(const char *, ...);

int f;
int *g = &f, *h = &f;
char i;

void fn3(void) {
	*g |= 0x1B0231B7L;
}

int main(void) {
	*h = 4L;
	(fn3(), i);

	return printf("%d, %d\n", *g, *h);
}
