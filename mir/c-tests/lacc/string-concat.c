int puts(const char *s);

char str[] =
	"Hel" \
	"lo"
	" "
	"wo" "rld"    "!";

int main(void) {
	puts(str);
	return sizeof(str);
}
