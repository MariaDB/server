struct s {
	char *text;
};

struct s *var;

int main() {
	struct s foo = {"Hello"};
	var = &foo;
	return var->text[0];
}
