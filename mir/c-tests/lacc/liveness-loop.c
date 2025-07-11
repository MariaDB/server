int printf(const char *, ...);

float h, g = 1.1;
int i = 2, j = 0;

int main(void) {
	h = (g = i);
	for (; j < 5; j++) { }

	return printf("%f\n", g);
}
