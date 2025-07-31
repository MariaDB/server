int printf(const char *, ...);

struct s {
	float f;
} wat = {3.14f};

struct s foo(void) {
	return wat;
}

int main(void) {
	struct s bar = foo();
	return printf("%f\n", bar.f);
}
