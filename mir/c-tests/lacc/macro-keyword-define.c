#define double int

#ifndef double
#error no way
#endif

#if double + int != defined(int)
#error this should be false
#endif

int main(void) {
	double d = 42;
	return sizeof(d);
}
