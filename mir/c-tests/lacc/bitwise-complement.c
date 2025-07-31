int main () {
  int i = 23;
  unsigned char c = 3;

  return (unsigned char) (~i + ~c) != 228;
}
