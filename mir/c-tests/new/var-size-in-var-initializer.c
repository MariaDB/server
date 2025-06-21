void *malloc (int size);
static int *f (void) {
  int *new_pair = malloc (sizeof (*new_pair));
  return new_pair;
}
int main (void) { return 0; }
