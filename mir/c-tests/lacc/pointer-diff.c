int printf(const char *, ...);

int diff(char **first, char **last) {
	return printf("%ld\n", last - first);
}

int main(void) {
	char *DataList[4];
	return diff(DataList, DataList + 4);
}
