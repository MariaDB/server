int puts(const char *);
int (*foo)(const char *);

void danger(void) {
	((void (*)(void))0xdeadbeef)();
}

int main(void) {
	int (*bar)(const char *);
	foo = puts;
	bar = puts;
	foo("Hello");
	bar("How are you?");
	return 0;
}
