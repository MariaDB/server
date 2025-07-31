int printf(const char *, ...);

struct {
	signed f6 : 1;
} g = {0};
int f = 42;

int foo(void) {
	g.f6 ^= 4;
	if (g.f6) {
		(f)--;
	}
}

int main(void) {
	foo();
	return printf("%d, %d\n", g.f6, f);
}
