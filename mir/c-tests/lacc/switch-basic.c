int test(int a) {
	int b = 0;

	switch (a) {
	case 1: b = 2;
		break;
	case 2: b = 3;
	case 3: b = 0;
	default:
		b = 6;
		break;
	}

	return b;
}

int main()
{
	return test(1) + test(2) + test(3) + test(4);
}
