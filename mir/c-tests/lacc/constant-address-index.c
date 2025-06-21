static void
	*p = (void*) &((char*)50)[2],
	*q = (void*) &((char*)50)[-2];

int main(void) {
	unsigned long v = (unsigned long) p + (unsigned long) q;
	return v;
}
