static char *read_file (const char *name) { /* we read only text files by this func */
  FILE *f;
  size_t flen, rlen;
  char *str;

  if ((f = fopen (name, "r")) == NULL) {
    perror (name);
    exit (1);
  }
  fseek (f, 0, SEEK_END);
  flen = ftell (f);
  rewind (f);
  str = (char *) malloc (flen + 1);
  rlen = fread (str, 1, flen, f);
#ifndef _MSC_VER
  if (rlen != flen) {
    fprintf (stderr, "file %s was changed\n", name);
    free (str);
    exit (1);
  }
#endif
  str[rlen] = 0;
  return str;
}
