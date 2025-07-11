#ifdef FOO
#  if FOO(1, 2)
#    error This should not happen
#  elif FOO(2, 3)
#    error Not this either
#  endif
#endif

int printf(const char *, ...);

int square(int x) {
	return x * x;
}

#define square(x) x

int main(void) {
	return printf("square=%d, %d, %d\n", square(square)(2), square(square
		(square(
			square)))
		(2),
		square(2));
}
