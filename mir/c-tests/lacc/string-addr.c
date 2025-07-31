int puts(const char *);

/* Note that *c is still incomplete after this assignment. */
char (*c)[] = &"Hello";

int main(void)
{
	puts(*c);
	return 0;
}
