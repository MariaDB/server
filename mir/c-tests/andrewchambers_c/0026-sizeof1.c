
int main()
{
	if(sizeof(0) != 4)
		return 1;
	if(sizeof(char) != 1)
		return 2;
	if(sizeof(int) != 4)
		return 3;
	return 0;
}

