int printf(const char *, ...);

float foo(double n) {
	return n * 2.0;
}

double bar(float n) {
	return n + foo((double) n);
}

int main(void) {
	return printf("r = %f\n", bar(3.14f));
}
