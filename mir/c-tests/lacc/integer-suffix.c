int printf(const char *str, ...);

int main(void) {
	return printf("%lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu\n",
		sizeof(42),
		sizeof(42l),
		sizeof(42L),
		sizeof(42u),
		sizeof(42U),
		sizeof(42ul),
		sizeof(42uLL),
		sizeof(42ULL),
		sizeof(42llU),
		sizeof(42LLu),
		sizeof(42LLU));
}

