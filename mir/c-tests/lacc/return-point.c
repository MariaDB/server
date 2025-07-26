/* Takes up more than two eightbytes, thus must be returned by memory and not
 * registers.
 */
struct point {
    long x, y, z;
} points[] = {
    {1, 2, 3},
    {4, 5, 6}
};

struct point getPoint(int i) {
    return points[i];
}

int main() {
    struct point p = getPoint(1);
    return p.y;
}
