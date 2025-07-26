#include <string.h>

int printf(const char *, ...);

typedef unsigned long long u64;

int nan_ne_assign(double x) {
	volatile double y = x;
	volatile double z = y;
	int rc;
	rc = (y != z);
	return rc;
}

int nan_eq_assign(double x) {
	volatile double y = x;
	volatile double z = y;
	int rc;
	rc = (y == z);
	return rc;
}

int nan_ne_jump(double x) {
	volatile double y = x;
	volatile double z = y;

	if (y != z) {
		return 1;
	}

	return 0;
}

int nan_eq_jump(double x) {
	volatile double y = x;
	volatile double z = y;

	if (y == z) {
		return 1;
	}

	return 0;
}

int nan_ne_return(double x) {
	volatile double y = x;
	volatile double z = y;

	return y != z;
}

int nan_eq_return(double x) {
	volatile double y = x;
	volatile double z = y;

	return y == z;
}

int main(void) {
	u64 x = (((u64)1)<<63)-1;
	double y;

	memcpy(&y, &x, 8);
	return printf("%d, %d, %d, %d, %d, %d\n",
		nan_ne_assign(y),
		nan_eq_assign(y),
		nan_ne_jump(y),
		nan_eq_jump(y),
		nan_ne_return(y),
		nan_eq_return(y));
}
