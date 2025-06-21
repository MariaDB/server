int puts(const char *);

int (*g1)(const char *) = puts;
int (*g2)(const char *) = &puts;
int (*g3)(const char *) = *puts;

static void foo(void) {
	puts("Foo to you too!");
}

int main(void) {
	void (*p1_foo)() = foo;
	void (*p2_foo)() = *foo;
	void (*p3_foo)() = &foo;
	void (*p4_foo)() = *&foo;
	void (*p5_foo)() = &*foo;
	void (*p6_foo)() = **foo;
	void (*p7_foo)() = **********************foo;

	(*p1_foo)();
	(*p2_foo)();
	(*p3_foo)();
	(*p4_foo)();
	(*p5_foo)();
	(*p6_foo)();
	(*p7_foo)();

	g1("1");
	g2("2");
	g3("3");

	return 0;
}
