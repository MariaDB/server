/* test for a bug in defining size of array with forward decl and initializer */
int c[];
int c[] = {1};
int f (void) { return sizeof (c); }
int main (void) { return 0; }
