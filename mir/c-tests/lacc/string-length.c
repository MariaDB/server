static const char str[] =
	"Hello\0" " world,"
	"\0 this is dog!";

int main(void) {
	return sizeof(str) + sizeof("");
}
