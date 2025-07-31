int puts(const char *);

void speak(const char *str) {
	puts(str);
}

int main(void) {
	int i = 42;
	(i) ? speak("Hello") : speak("World");
	return 0;
}
