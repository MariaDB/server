int printf(const char *, ...);

int main(void) {
	float f1 = 3.14f, f2, f3;
	double d1 = 2.71, d2, d3;

	f2 = f1 - (d1 - 4.2f);
	d2 = f2 + f1 + 3.63123;
	f3 = f2 / (d1 / 0.44f);
	d3 = d2 * f1 * 3.1;

	printf("f1 = %f, f2 = %f, f3 = %f\n", f1, f2, f3);
	printf("d1 = %f, d2 = %f, d3 = %f\n", d1, d2, d3);

	return f1 * d1;
}
