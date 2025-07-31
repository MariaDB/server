int printf(const char *, ...);

float g, i = 0;
double h, j = 0;

int main(void) {
	g = (-i);
	h = (-j);
	return printf("%f, %f\n", g, h);
}
