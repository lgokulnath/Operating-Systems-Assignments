#include <kernel/types.h>
#include <kernel/stat.h>
#include <user/user.h>

// pipe 
int fd[2];

int checkInt(char* s)
{
    int len = strlen(s);
    int i = 0;
    if(len <= 0){
        return 0;
    }
    int neg = 0;
    if(s[i] == '-' || s[i] == '+') {
        if(s[i] == '-') neg = 1;
        i++;
    } 
    for(; i < len; i++) {
        if(s[i] >= '0' && s[i] <= '9') {
            continue;
        }
        else {
            return 0;
        }
    }
    return 1 + neg;
}

int checkPosInt(char* s)
{
    if(s[0] == '-') return 0;
    return checkInt(s);
}

void foo(int n)
{
    if(n == 0) return;
    int rc = fork();
    if(rc == -1) {
        printf("Fork failed. Aborting...\n");
        exit(1);
    }
    if(rc == 0) {
        int val;
        if(read(fd[0], &val, sizeof(int)) < 0) {
            printf("read failed. Aborting...\n");
            exit(1);
        }
        int pid = getpid();
        val += pid;
        printf("%d: %d\n", pid, val);
        if(write(fd[1], &val, sizeof(int)) < 0) {
            printf("write failed. Aborting...\n");
            exit(1);
        }
        foo(--n);
    }
    else {
        close(fd[0]);
        close(fd[1]);
        wait(0);
    }
    exit(0);

}


int main(int argc, char* argv[])
{
    if(argc != 3) {
        printf("usage: pipeline <pos integer> <any integer>\n");
        exit(1);
    }
    int check1 = checkPosInt(argv[1]);
    int check2 = checkInt(argv[2]);
    if(!(check1 && check2)) {
        printf("usage: pipeline <pos integer> <any integer>\n");
        exit(1);
    }
    int n = atoi(argv[1]);
    int x = atoi(argv[2]);
    if(check2 > 1) x = -atoi(argv[2] + 1);
    if(n == 0) {
        printf("usage: pipeline <pos integer> <any integer>\n");
        exit(1);
    }
    if(pipe(fd) < 0 ) {
        printf("Error in creating pipe. Aborting...\n");
        exit(1);
    }
    int pid = getpid();
    int val = x + pid;
    printf("%d: %d\n", pid, val);
    if(write(fd[1], &val, sizeof(int)) < 0) {
        printf("write failed, aborting...\n");
        exit(1);
    }
    foo(--n);
    exit(0);
}