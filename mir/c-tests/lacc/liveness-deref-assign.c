int printf(const char *, ...);

int l;
int wat[] = {3, 4, 5};

const int *g;
const int **h = &g;

int main(void) {
	int *p;
	p = &wat[1];
	*h = (p = &l);
	*p = 1;

	return printf("%d\n", wat[1]);
}
