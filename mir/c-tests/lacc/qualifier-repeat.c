int printf(const char *, ...);

const const int a = 42;
static volatile volatile int b;

int main(void) {
	return a + b;
}
