int
main()
{
	char *p;

	p = "foobar";
	if(p[1] != 111)
		return 1;
	if(p[6] != 0)
		return 1;
	return 0;
}