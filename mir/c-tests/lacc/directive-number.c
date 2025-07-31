#if 'A' == '\301'
#error Wrong
#endif

int main (void) { return (unsigned char) '\301' != 193; }
