int 
main()
{
	int x;
	
	for(x = 0; x < 10 ; x = x + 1)
		;
	if(x != 10)
		return 1;
	return 0;
}

