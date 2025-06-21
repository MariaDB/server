#include <stdio.h>

static struct point {
	char c;
	int d;
} obj = {'a', 0xaabbccdd};
static char sc;

int main() {
	sc = 'a';
	printf("%c\n%d\n%c\n", obj.c, obj.d, sc);
	return 0;
}
