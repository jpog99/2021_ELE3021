#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]){
	int pid = fork();
	
	while(1){
		if(pid == 0) //child process
		{
			printf(1,"Child\n");
		}
		else if(pid > 0) //parent process
		{
			printf(1,"Parent\n");
		}
		else
		{
			printf(1,"Fork Error\n");
			exit();
		}
		yield();
	}
	return 0;
}
