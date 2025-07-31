int printf(const char *, ...);

int max(a, b)
register int a, b;
{
	return (a > b) ? a : b;
}

int print(line, message)
int line;
const char *message;
{
	return printf("(%d): %s\n", line, message);
}

int main(void) {
	return print(max(3, -5), "Hello world");
}
