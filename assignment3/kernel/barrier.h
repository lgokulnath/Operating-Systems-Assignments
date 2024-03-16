#include "sleeplock.h"
struct barrier
{
    /* data */
    int free;   // track whether this barrier is free
    int num;    // no of processes waiting on the barrier
    struct sleeplock blk;
    struct cond_t cv;
};

