

// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// contributed by Shakhno DV, Shakhno AV

#include <math.h>
#include <stdio.h>
#include <stdlib.h>


#define pi 3.141592653589793
#define solar_mass (4 * pi * pi)
#define days_per_year 365.24
#define NBODIES 5
#define DT 0.01

double x[NBODIES], y[NBODIES], z[NBODIES];
double vx[NBODIES], vy[NBODIES], vz[NBODIES];
double mass[NBODIES];


void advance(int n)
{
    double dx;
    double x1;
    double y1;
    double z1;
    double dy;
    double dz;
    double R;
    double mag;
    for (int k = 1; k <= n; ++k)
    {
        for (int i = 0; i < NBODIES; ++i)
        {
            x1 = x[i];
            y1 = y[i];
            z1 = z[i];
            for (int j = i + 1; j < NBODIES; ++j)
            {
                dx = x1 - x[j];
                R = dx * dx;
                dy = y1 - y[j];
                R += dy * dy;
                dz = z1 - z[j];
                R += dz * dz;
                R = sqrt(R);
                mag = DT / (R * R * R);
                vx[i] -= dx * mass[j] * mag;
                vy[i] -= dy * mass[j] * mag;
                vz[i] -= dz * mass[j] * mag;
                vx[j] += dx * mass[i] * mag;
                vy[j] += dy * mass[i] * mag;
                vz[j] += dz * mass[i] * mag;
            }
        }

        for (int i = 0; i < NBODIES; ++i)
        {
            x[i] += DT * vx[i];
            y[i] += DT * vy[i];
            z[i] += DT * vz[i];
        }
    }
}

double energy()
{
    double e = 0.0;
    for (int i = 0; i < NBODIES; ++i)
    {
        e += 0.5 *mass[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
        for (int j = i + 1; j < NBODIES; ++j)
        {
            double dx = x[i] - x[j];
            double dy = y[i] - y[j];
            double dz = z[i] - z[j];
            double distance = sqrt(dx * dx + dy * dy + dz * dz);
            e -= (mass[i] * mass[j]) / distance;
        }
    }
    return e;
}

void offset_momentum()
{
    double px = 0.0, py = 0.0, pz = 0.0;
    for (int i = 0; i < NBODIES; ++i)
    {
        px += vx[i] * mass[i];
        py += vy[i] * mass[i];
        pz += vz[i] * mass[i];
    }
    vx[0] = -px / solar_mass;
    vy[0] = -py / solar_mass;
    vz[0] = -pz / solar_mass;
}

void init()
{
    x[0] = 0;
    y[0] = 0;
    z[0] = 0;
    vx[0] = 0;
    vy[0] = 0;
    vz[0] = 0;
    mass[0] = solar_mass;
    x[1] = 4.84143144246472090e+00;
    y[1] = -1.16032004402742839e+00;
    z[1] = -1.03622044471123109e-01;
    vx[1] = 1.66007664274403694e-03 * days_per_year;
    vy[1] = 7.69901118419740425e-03 * days_per_year;
    vz[1] = -6.90460016972063023e-05 * days_per_year;
    mass[1] = 9.54791938424326609e-04 * solar_mass;
    x[2] = 8.34336671824457987e+00;
    y[2] = 4.12479856412430479e+00;
    z[2] = -4.03523417114321381e-01;
    vx[2] = -2.76742510726862411e-03 * days_per_year;
    vy[2] = 4.99852801234917238e-03 * days_per_year;
    vz[2] = 2.30417297573763929e-05 * days_per_year;
    mass[2] = 2.85885980666130812e-04 * solar_mass;
    x[3] = 1.28943695621391310e+01;
    y[3] = -1.51111514016986312e+01;
    z[3] = -2.23307578892655734e-01;
    vx[3] = 2.96460137564761618e-03 * days_per_year;
    vy[3] = 2.37847173959480950e-03 * days_per_year;
    vz[3] = -2.96589568540237556e-05 * days_per_year;
    mass[3] = 4.36624404335156298e-05 * solar_mass;
    x[4] = 1.53796971148509165e+01;
    y[4] = -2.59193146099879641e+01;
    z[4] = 1.79258772950371181e-01;
    vx[4] = 2.68067772490389322e-03 * days_per_year;
    vy[4] = 1.62824170038242295e-03 * days_per_year;
    vz[4] = -9.51592254519715870e-05 * days_per_year;
    mass[4] = 5.15138902046611451e-05 * solar_mass;
}
int main(int argc, char ** argv)
{
    int n = atoi(argv[1]);
    init();
    offset_momentum();
    printf("%.9f\n", energy());
    advance(n);
    printf("%.9f\n", energy());
    return 0;
}
