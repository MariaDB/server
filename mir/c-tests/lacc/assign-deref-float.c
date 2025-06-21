int printf(const char *, ...);

float v;
double d;

int main(void) {
	float *p = &v, q = 3.14f;
	double *r = &d, s = 2.71;
	*p = q;
	*r = s;
	return printf("%f, %f\n", *p, *r);
}
