#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void Uptime(void){
	int ans = uptime();
	printf("%d\n",ans);
}
int main(int argc, char*argv[]){
	Uptime();
	exit(0);
}