
int main()
{
	char *a, *b;
	
	a = "abcdef";
	if(*(a + 2) != 'c')
		return 1;
	if(*(2 + a) != 'c')
		return 2;
	return 0;
}
