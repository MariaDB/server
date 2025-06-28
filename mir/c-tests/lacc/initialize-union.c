union value {
	char v;
	long l;
	struct {
		long x, y;
	} point;
};

int main() {
	union value v = {1};

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return v.point.x;
#else
	return (int) v.point.x + 1;
#endif
}
