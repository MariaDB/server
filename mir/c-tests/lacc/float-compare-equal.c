int printf(const char *, ...);

int g = -4L, i = 0;
float *f;

void foo(int *p1) {
	f = (void *) p1;
	*f = (i == *f);
}

int main(void) {
	foo(&g);
	return printf("%d\n", g);
}
