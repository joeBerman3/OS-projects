#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    int before = memsize();
    printf("Memory size before allocation: %d bytes\n", before);

    char *p = malloc(20 * 1024);
    if(p == 0){
        printf("malloc failed\n");
        exit(1);
    }

    int after_alloc = memsize();
    printf("Memory size after allocation: %d bytes\n", after_alloc);

    free(p);

    int after_free = memsize();
    printf("Memory size after free: %d bytes\n", after_free);

    exit(0);
}
