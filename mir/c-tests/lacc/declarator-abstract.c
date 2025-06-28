int printf(const char *, ...);

typedef int x;

void f1(int(x));    /* void f(int (*)(int)) */
int f2(int(y));     /* int f(int) */
void f3(int((*)));  /* void f(int *) */
void f4(int((*x))); /* void f(int *) */
void f5(int((x)));  /* void f(int (*)(int)) */
void f6(int(int));  /* void f(int (*)(int)) */

int id(int a) {
	return a;
}

int main(void) {
	int a = 42;
	void (*c1)(int(x)) = f6;
	int (*c2)(int(y)) = id;
	void (*c3)(int((*))) = f4;
	void (*c4)(int((*x))) = f3;
	void (*c5)(int((x))) = f1;
	void (*c6)(int(int)) = f5;

	c1(id);
	c3(&a);
	c4(&a);
	c5(c2);
	c6(id);

	return 0;
}

void f1(int (*f)(int)) {
	printf("f1: %d\n", f(1));
}

void f3(int *a) {
	printf("f3: %d\n", *a);
}

void f4(int *a) {
	printf("f4: %d\n", *a);
}

void f5(int (*f)(int)) {
	printf("f5: %d\n", f(5));
}

void f6(int (*f)(int)) {
	printf("f6: %d\n", f(6));
}
