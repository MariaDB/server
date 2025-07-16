/* test for bug of pointers with mixed char sign */
typedef unsigned char uchar;
uchar *defns;
int str (const char *s) { return 0; }
int f (void) { return str (defns); }
int main (void) { return 0; }
