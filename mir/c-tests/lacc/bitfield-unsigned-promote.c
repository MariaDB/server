static struct {
	unsigned f1 : 23;
} f;

int main() {
	int h = (f.f1 > -1);
	return h;
}
