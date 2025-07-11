
int fact(int n)
{
	if (n)
		return n * fact(n - 1);
	return 1;
}

int main()
{
	return fact(5);
}
