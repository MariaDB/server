enum {
	FOO = 1,
	BAR = 2
};

extern int foo(enum { FOO = 4, BAR = 5 } pp, int arr[][FOO]);

int main(void) {
	return FOO;
}
