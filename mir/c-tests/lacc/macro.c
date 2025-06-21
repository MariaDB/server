int puts(const char *);
int printf(const char *, ...);

#define FOO(a, b) (a b - 1)

#define BAR(x) __FILE__, *d = #x, e = x ;
#define max(a, b) ((a) > (b) ? (a) : (b))

/*
 * Some comments to confuse line counting.
 */

int main(void) {
	int a = 42, b; /*     Single line comment. */
	char *c = BAR(
		__LINE__)

	printf("max=%d\n", max(max(2, -1), 1));
	puts(c);
	puts(d);

	puts(__FILE__);
#if (__STDC_VERSION__ >= 199901)
	puts(__func__);
#endif
	b = __LINE__ + FOO ( 1 + a *, 2 ) + e;
	return printf("%d\n", b);
}
