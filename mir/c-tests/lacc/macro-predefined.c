#include <string.h>

char *date = __DATE__;
char *time = __TIME__;

int main(void) {
	return strlen(date) + strlen(time);
}
