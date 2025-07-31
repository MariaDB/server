void exit(int status);

int main() {
	int i = 2;
	switch (i) {
	case 1:
		return 1;
	case 2:
		return 2;
	default:
		exit(3);
	}
}
