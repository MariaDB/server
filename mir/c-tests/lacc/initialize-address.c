static int a = 42, *b = &a;

int *foo[] = {&a, &a, (int *) &b};
int **bar[] = {&foo[1], foo + 2};

int main(void) {
	static int f = 3, *p = &f;

	return *b + *foo[1] + *p;
}
