union foo {
  int a;
#ifdef _WIN32
  long long b;
#else
  long b;
#endif
};

int main () {
  union foo bar;
  bar.a = 1;
  bar.b = 8;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return sizeof (bar) + sizeof (union foo) + bar.a + bar.b != 32;
#else
  return sizeof (bar) + sizeof (union foo) + bar.a + bar.b != 24;
#endif
}
