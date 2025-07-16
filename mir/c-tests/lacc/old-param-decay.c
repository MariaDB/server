int printf(const char *, ...);

#define N 42

int foo(n, a)
int a[31][N];
int n;
{
	return sizeof(a) + sizeof(a[14]) + n;
}

int main(void) {
	int a[2][N] = {0};
	return printf("%d\n", foo(N, a));
}
