int main() {
	int a = 1;
	int b = a++;
	int c = --b;
	++a;
	--a;
	return a-- + b-- - ++c;
}
