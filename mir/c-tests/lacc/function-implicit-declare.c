int main(void) {
	return bar();
}

extern int bar(void);

int bar(void) {
	return 42;
}
