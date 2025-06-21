int main(void) {
	int a = 42,
	    b = (!0xDD8FB8CEL) ? 7 : 14,
	    c = (0xDD8FB8CEL || 0),
	    d = (0xDD8FB8CEL && 1);
	if (1) {
		a = 4;
	} else {
		a = 14;
	}

	return a + b + c + d;
}
