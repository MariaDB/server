int printf(const char *, ...);

float f = 3.14f;
double g = 2.71;

int main(void) {
	long double d = f;
	long double e = g;
	return printf("%Lf, %Lf\n", d, e);
}
