int x;

int
foo(int *p1)
{
    *p1 = 0;
    return 0;
}

int
main()
{
    x = 6;
    foo(&x);
    return x;
}
