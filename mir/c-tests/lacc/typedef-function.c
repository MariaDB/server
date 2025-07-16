int atoi(const char *nptr);
int isalnum(int c);

typedef int foo_fn(const char *);

int foo(foo_fn fn) {
	int (*p)(const char *) = fn;
	return p("10");
}

typedef int bar_fn(int);

int bar(bar_fn fn) {
	return !!fn('a');
}

int main(void) {
  return !(foo(atoi) + bar(isalnum) == 11);
}
