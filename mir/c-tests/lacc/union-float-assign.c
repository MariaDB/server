int printf(const char *, ...);

union U {
	float f0;
} foo;

static union U *bar = &foo;

int main(void) {
	union U w = {3.14f};
	*bar = w;
	return printf("%f\n", foo.f0);
}
