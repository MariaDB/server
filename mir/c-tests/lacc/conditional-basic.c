int main()
{
	int a = 1;
	int b = 2;

	int c = (a + 1) ? (b - 2) ? b-- : b++ : 42;
	int d = 0 ? 1 : 2;
	return c + a + b + d;
}
