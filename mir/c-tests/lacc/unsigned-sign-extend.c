int putchar(int c);

int main(void) {
	char *data = "Hello World!", *ptr;
	int offset = 4;
	int diff = -2;

	ptr = data + offset + diff - diff;

	return putchar(*ptr);
}
