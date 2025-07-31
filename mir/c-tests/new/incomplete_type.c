extern char ofname[];
char ofname[1024];
int main (void) { return sizeof (ofname) != 1024; }
