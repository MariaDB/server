struct s {
    int x;
    int y;
};

int 
main() {
    struct s v;
    v.x = 2;
    v.y = 1;
    if(v.x != 2)
    	return 1;
    if(v.y !=1)
    	return 1;
    return 0;
}

