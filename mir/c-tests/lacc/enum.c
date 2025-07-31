enum token {
	FOO,
	BAR = 23,
	BAZ = BAR + 4
};

int foo(enum token tok)
{
	return tok;
}

int main() {
	enum BOOL { TRUE, FALSE } truth = FALSE;
	enum token { FOO = 1 } shadow = 0;
	enum token t = FOO;
	int a;

	t = 3 + truth;
	a = t;

	return BAZ - a;
}
