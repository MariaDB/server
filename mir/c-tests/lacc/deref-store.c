int printf(const char *, ...);

char *name;

int main(void) {
	char text[] = "ab";

	printf("%s\n", text);
	name = text;

	*name = '4';
	printf("%s\n", name);
	return 0;
}
