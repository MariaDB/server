struct S0 {
  long f0;
  int f1;
  int f2;
};
static const long *g_612;
static struct S0 *volatile g_1277; /* VOLATILE GLOBAL g_1277 */
static struct S0 func_1 (void) {   /* block id: 0 */
  struct S0 l_1257 = {0UL, 0x48F2127FL, 0xEA81D353L};
  if (((void *) 0 == &g_612)) { /* block id: 588 */
    return l_1257;
  } else { /* block id: 590 */
    return (*g_1277);
  }
}

int main (void) { return 0; }
