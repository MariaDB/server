#include <stdio.h>

int main() {
	char c = 'a';
	int i = -1;
	long l;
	l = (c = i);

	printf("%ld\n", l);
	return 0;
}
