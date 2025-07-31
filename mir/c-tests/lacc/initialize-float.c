int printf(const char *, ...);

float f = 3.5f;
double d = 42.3;

struct {
	double x;
	float y, z;
} p = {4.2f, 9.90};

int main(void) {
	static double q;
	return printf("%f, %f, (%f, %f, %f), %f\n", f, d, p.x, p.y, p.z, q);
}
