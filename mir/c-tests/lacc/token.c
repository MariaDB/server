int puts(const char *);
int putchar(int);

const char foo[] = "\041\042 \043\052 The answer to everything is \x34\x32 (\?)";

char bar[] = "\"The End\"\0 Nothing to see here.";

char t1 = '\056';
char t2 = '\x53';
char t3 = '\n';
char t4 = '`';
char t5 = 'a';
char t6 = '\0';

int _printstuff__() {
	puts(foo);
	puts(bar);

	putchar(t1);
	t1 = '9';
	putchar(t1);
	putchar(t2);
	putchar(t3);
	putchar(t4);
	putchar(t5);
	putchar(t6);

	return t5;
}

int main() {
	return _printstuff__();
}
