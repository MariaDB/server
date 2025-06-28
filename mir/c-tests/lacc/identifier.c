static int a;
extern int c;

int foo() {
	static int b;
	b = 1;
	return b;
}

int c;

int main(void) {
    extern int a;
    static int b;
    {
    	static int a = 2;
    	b = a;
    }
    c = 1;
    return a + b + c + foo();
}


int c;

static int a = 3;

extern int u;
int u;

int v;
extern int v;

static int w(void);
int w(void);

static int x(void);
extern int x(void);
