#include <stdio.h>
#include <ctype.h>

static void
printstr(FILE *stream, const char *str)
{
    char c;

    while ((c = *str++) != '\0') {
        putc(c, stream);
        if (isprint(c) && c != '"' && c != '\\')
            putc(c, stream);
        else
            fprintf(stream, "\\x%x", (int) c);
    }
}

int main() {
    printstr(stdout, "heisann!\n");
    return 0;
}
