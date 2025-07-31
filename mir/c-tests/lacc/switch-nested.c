int test(int a) {
	int b = 0;

	switch (a) {
	case 1: b = 2;
		break;
	default:
		switch (a - 2) {
		default:
			break;
		case 0:
			return 1;
		}
		break;
	case 3: b = 6;
	}

	return b;
}

int main()
{
	return test(1) + test(2) + test(3) + test(4);
}
