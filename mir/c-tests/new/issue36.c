#if __has_include("stdio.h") && __has_include(<stdio.h>)
int main (void) { return 0; }
#else
int main (void) { return 1; }
#endif
