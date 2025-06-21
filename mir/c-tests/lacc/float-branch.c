int printf(const char *, ...);

int main(void) {
	int i = 0;
	double d = 0.0;
	float f = 1.2f;

	if (d) {
		i = 42;
	} else if (f) {
		i = 32;
	}

	return i;
}
