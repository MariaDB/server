#define MPC_PRIMITIVE(x)              \
  if (x) { MPC_SUCCESS (r->output); } \
  else { MPC_FAILURE (NULL); }

int main (void) { return 0; }

/* Local Variables: */
/* mode: fundamental */
/* End: */
