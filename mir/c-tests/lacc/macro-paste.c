int puts (const char *);

#define FOO(s) s##_f##u##nc
#define STR(s) #s
#define CAT(a, b) STR (a##b)

extern int printf (const char *, ...);
#define puts(s) printf ("%s\n", s)
int foo_func (void) { return puts (CAT (foo, 5)); }

#define glue(a, b) a##b
#define cat(a, b) glue (a, b)
#define HELLOWORLD "hello"
#define WORLD WORLD ", world"

int test (void) { return puts (glue (HELLO, WORLD)) + puts (cat (HELLO, WORLD)); }

int main (void) { return !(FOO (foo) () + test () == 24); }
