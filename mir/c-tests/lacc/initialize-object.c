#include <stdio.h>

int main(void) {
	int a = 42;
	short b = {a + 1};

	return printf("%d\n", b);
}
