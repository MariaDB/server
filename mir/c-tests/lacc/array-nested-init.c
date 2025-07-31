int foo[4][3][2] = {{{0}}};

int main(void) {
	return foo[3][2][1] + sizeof(foo);
}
