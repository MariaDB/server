int printf(const char *, ...);

float a = 42.42, b;
int c = 1L;
float *d = &b;

void fn1(void) {
	*d = (a = c);
}

int main(void) {
	fn1();
	return printf("%f\n", a);
}
