int puts(const char *s);
int printf(const char *, ...);

typedef unsigned int foo_t;

const char typedef *strptr;

const char *string = "Hello world!";

typedef void eat_function_t (int n);

typedef eat_function_t cookie_eat_function_t;

static void eat_cookie(int n) {
	while (n-->0) {
		printf("nom");
	}
}

static int eat_cookies(void) {
	eat_function_t *f1 = eat_cookie;
	cookie_eat_function_t *f2 = eat_cookie;

	eat_cookie(1);
	f1(2);
	f2(3);

	return 0;
}

int main(void) {
	foo_t retcode;
	strptr sp;

	sp = string;
	puts(sp);

	eat_cookies();

	retcode = 0;
	return retcode;
}
