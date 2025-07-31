
int x;
int x = 0;
int x;

int main();

void *
foo()
{
	return &main;
}

int
main()
{
	x = 0;
	return x;
}
