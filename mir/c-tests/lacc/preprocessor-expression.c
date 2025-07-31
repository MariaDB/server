#define A 9223372036854775807L
#define B 2147483647

int a =
#if (A >= B)
   1
#else
   2
#endif
;

#if (0xFFFFFFFFFFFFFFFF <= 42)
#  error Unsigned compare failed!
#endif

int main(void) {
   return a;
}
