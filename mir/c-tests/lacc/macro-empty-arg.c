int printf(const char *, ...);

#define FOO(a, b, c) (a)
#define CONCAT(x, y, z) x z ## y + 1
#define STR(i, j) #i "hello" #j "world"

#define GLUE(a, b) a ## b
#define JOIN(a, b, c) GLUE(a, b c)

static int n = JOIN(1,,1);

int main(void) {
	int a = CONCAT(2,,);
	int b = CONCAT(,2,);
	int c = FOO(0 + a,  ,);
	return printf(STR(,c)) + printf("%d, %d, %d, %d\n", a, b, c, n);
}
