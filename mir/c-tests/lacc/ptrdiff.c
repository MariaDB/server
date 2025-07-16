struct point {
	int x, y;
} points[5];

char *str = "Hello world";

int main() {
	char *send = str + 8;
	struct point *pend = points + 3;

	unsigned long sdiff = send - str;
	unsigned long pdiff = pend - points;

	return sdiff + pdiff;
}
