int puts(const char *);

char hello[] = "Hello World!";

char *how = ("How are you?" - 1) + 3;

char *foo(void)
{
	return "From foo with love";
}

int main(void)
{
	char arr[] = "How are you?";
	char *offst = "How are you?" + 5;
	puts(hello);
	puts(how);
	puts(arr);
	puts(foo());
	return 0;
}
