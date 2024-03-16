#include <kernel/types.h>
#include <kernel/stat.h>
#include <user/user.h>

int primes[]={2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59,61,67,71,73,79,83,89,97};

int gp[2];
int n;


void foo(int i)
{
    
    if(fork() == 0) {
        int num;
        int cnt = 0;
        
        
        if(read(gp[0], &num, sizeof(int)) < 0) {
                printf("read failed. Aborting...\n");
                exit(1);
        }
        
        if(num == 0 || num == 1) {
            return;
        }
        while(num % primes[i] == 0) {
            num /= primes[i];
            //sleep(100);
            printf("%d, ", primes[i]);
            cnt++;
        }
        if(cnt > 0) {
            printf("[%d]\n", getpid());
        }
        if(write(gp[1], &num, sizeof(int)) < 0) {
                printf("write failed. Aborting...\n");
                exit(1);
        }
        
        foo(i+1);
           
    }
    else {
       //
        close(gp[0]);
        close(gp[1]);

        wait(0);
    }
}

int main(int argc, char* argv[])
{
    if(argc != 2) {
        printf("Usage: primefactors n\n");
        exit(1);
    }
    
    n = atoi(argv[1]);
    
    if(pipe(gp) < 0) {
        printf("Error in creating pipe, Aborting...\n");
        exit(1);
    }
    
    int i = 0;
    int cnt = 0;
    while(n % primes[i] == 0) {
        n /= primes[i];
        printf("%d, ", primes[i]);
        cnt++;
    }
    if(cnt > 0) {
        printf("[%d]\n", getpid());
    }
    // printf("Writing %d to pipe\n", n);
    if(write(gp[1], &n, sizeof(int)) < 0) {
        printf("write error\n");
    }
   
    foo(1);
    exit(0);
}