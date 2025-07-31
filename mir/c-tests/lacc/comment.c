int printf(const char *, ...);

/* Hello!
// */

char *str = "a\"/*b\"\\\\";
char c = '"', d = '\'';

/*\
foo();
*/
/\
* bar(); *\
/

int foo/*	*/(int/**/a, int b) {
	int f = a/*\*//b;
	int g = a/ /**/b;
	return printf("%d, %d, %s, %s, %d, %d\n", f, g, "w ??=*/ /*t?\
??", str, c, d);
}

int main(void) {
	return /*//*/ foo(__LINE__ * 2, 2);;
}

