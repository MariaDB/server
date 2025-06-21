#define FOO PRINT
#define PRINT(m) printf("%s\n", m)
#define BAR printf("hello: "); PRINT

int printf(const char *, ...);

int main(void)
{
	FOO(


	"hi"

"ho"

);

	FOO("Hello");
#if 1
	BAR
	(
		"to you");
#endif
	return 0;
}
