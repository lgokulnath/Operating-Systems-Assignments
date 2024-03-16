#include "kernel/types.h"
#include "user/user.h"

int num_items, num_prods, num_cons;

int produce(int index, int tid)
{
    return index + (num_items * tid);
}

int main(int argc, char *argv[])
{
    int i,j;
    // printf("starting condprodconstest\n");
    if (argc != 4)
    {
        fprintf(2, "syntax: condprodconstest number of items to be produced by each producer, number of producers, number of consumers.\nAborting...\n");
        exit(0);
    }

    num_items = atoi(argv[1]);
    num_prods = atoi(argv[2]);
    num_cons = atoi(argv[3]);
    buffer_cond_init();

    printf("Start time: %d\n\n", uptime());
    for (i = 0; i < num_prods; i++)
    {   int y=fork();
        if(y==-1){
            printf("fork failed\n");
            exit(0);
        }
        if (y == 0)
        {
            for (j = 0; j < num_items; j++){
                cond_produce(produce(j, i));
                // printf("Producer %d produced item %d\n", i, produce(j, i));
            }
            exit(0);
        }
        
    }
    for (i = 0; i < num_cons - 1; i++)
    {   int y=fork();
        if(y==-1){
            printf("fork failed\n");
            exit(0);
        }
        if (y == 0)
        {
            for (j = 0; j < (num_items * num_prods) / num_cons; j++)
                cond_consume();
            exit(0);
        }
    }
    for (j = 0; j < (num_items * num_prods) / num_cons; j++)
        cond_consume();
    for (i = 0; i < num_prods +num_cons-1 ; i++){
        int q;
        wait(&q);
        // printf("Child %d exited with status %d\n", i, q);
    }
    printf("\n\nEnd time: %d\n", uptime());
    exit(0);
}
