int f = 1;

int main(void) {
	1 && (f++, 1);
	0 || (f++, 1);
	return f;
}
