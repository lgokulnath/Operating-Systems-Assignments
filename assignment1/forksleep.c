#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"


int isposnum(char num[]){
	int size=strlen(num);
	if(size<=0){
		return 0;
	}
	for(int j=0;num[j]!=0;j++){
		if(!('0'<=num[j]&&'9'>=num[j])){
			return 0;
		}
	}
	return 1;
}

void Forksleep(int m,int n){
	if(n==0){
		int rc = fork();
		if(rc == -1) {
			printf("fork failed. Aborting...\n");
			exit(1);
		}
		else if(rc==0){
			sleep(m);
			printf("%d: Child.\n",getpid());
		}	
		else{
			printf("%d: Parent.\n",getpid());
			wait((int *) 0);
		}
	}
	else{
		int rc = fork();
		if(rc == -1) {
			printf("fork failed. Aborting...\n");
			exit(1);
		}
		else if(rc==0){
            printf("%d: Child.\n",getpid());
        }       
        else{	
			sleep(m);
            printf("%d: Parent.\n",getpid());
        }	
	}
	return ;
}

int main(int argc,char*argv[]){
	if(argc != 3){
		printf("usage: forksleep m n\n");
		exit(0);
	}
	int m=1,n=0;
	if(isposnum(argv[1])){
		m=atoi(argv[1]);
		if(m<=0){
			printf("m should be positive\n");
			exit(0);
		}
	}
	else{
		printf("m should be positive integer\n");
		exit(0);
	}
	if(isposnum(argv[2])){
    	n=atoi(argv[2]);
        if(!(n==0||n==1)){
            printf("n should be 0 or 1\n");
			exit(0);
        }
    }
    else{
        printf("n should be 0 or 1\n");
		exit(0);
    }
	Forksleep(m,n);
	exit(0);
}