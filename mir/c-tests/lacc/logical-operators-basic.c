int main()
{
	int a = 2;
	int b = 1;

	int c = a++ || 1;
	int d = b-- && (b = 10) && b-- && 0;
	return a + b + c + d;
}
