int printf(const char *, ...);

int g;
int *i = &g;
float j;

int main(void) {
	(*i) = 0xCFBCE008L;
	j = (0x0p1 > (*i));
	return printf("%d, %f\n", *i, j);
}
