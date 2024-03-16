// buffer conditionvaribale
#include "semaphore.h"
struct buffer_cond{
    int x;
    int full;
    struct sleeplock buff_lock;
    struct cond_t inserted;
    struct cond_t deleted;
};
struct buffer_sem{
    int x;
    int full;
    // struct sem buff_lock_ins;
    // struct sem buff_lock_del;
    // struct sem inserted;
    // struct sem deleted;
};