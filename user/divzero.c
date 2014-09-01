
// buggy program - causes a divide by zero exception

#include <inc/lib.h>

    void
umain(int argc, char **argv)
{
    int zero = 0;
    cprintf("1/0 is %08x!\n", 1/zero);
}

