int cvtfloat(void) {
	float f;
	unsigned char c = 0xff;
	f = c;
	return f;
}

int cvtdouble(void) {
	double d;
	d = 32;
	return d;
}

int main(void) {
	return cvtfloat() - cvtdouble();
}
