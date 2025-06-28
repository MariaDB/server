int printf(const char *, ...);

union obj {
	float f;
} g = {42.5678f};

int foo(union obj v) {
	float *p = &v.f;
	return printf("%f\n", *p);
}

int main(void) {
	return foo(g);
}
