void (a) (b) {}
void f (void) {
  {
    {
      unsigned c = 3;
    d:
      a (c);
      goto d;
      c = 12;
      for (; c <= 2; ++c)
        if (5) goto d;
    }
  }
}

int main (void) { return 0; }
