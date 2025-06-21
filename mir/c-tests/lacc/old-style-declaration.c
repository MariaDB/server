int printf(const char *, ...);

int foo(int n, const char *str);

int foo(n, str)
int n;
const char *str;
{
	return printf("%d, %s\n", n, str);
}

int main(void) {
	return foo(42, "Hello World");
}
