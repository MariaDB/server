int main(int argc, char *argv[])
{
	int i, j;
	for (i = 1; i & 7; i = i + 1) {
		if (i & 2)
			continue;
		for (j = 1; j & 3; j = j + 1) {
			if ((j + i) ^ 3)
				break;
		}
	}
	return i + j;
}
