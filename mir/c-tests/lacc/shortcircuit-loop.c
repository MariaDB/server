int foo(void) {
	while (1) {
		return 1;
	}
}

int bar(void) {
	int i = 0;
	do {
		if (i++)
			return 1;
	} while (1);
}

int baz(void) {
	int i;
	for (i = 0; 1; i++) {
		return 3;
	}
}

int main() {
	if (1)
		return foo() + bar() + baz();
}
