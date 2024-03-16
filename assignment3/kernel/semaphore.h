struct sem{
    int value;
    struct sleeplock sem_lk;
    struct cond_t sem_cv;
};