extern void printf (const char *, ...);
#if defined(__x86_64__)
register void *ret_addr asm ("r13");
#elif defined(__s390x__)
register void *ret_addr asm ("r7");
#else
register void *ret_addr asm ("r19");
#endif
void f (void) { __builtin_jret (ret_addr); }
int main (void) {
  ret_addr = &&lab;
  __builtin_jcall (f);
  return 1;
lab:
  printf ("ok\n");
  return 0;
}
