
int
foo()
{
	return 0;
}

int
main()
{
	int (*pf)();

	pf = &foo;
	return pf();
}
