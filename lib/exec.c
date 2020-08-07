/* SPDX-License-Identifier: GPL-2.0 */
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "utils.h"
#include "namespace.h"

int cmd_exec(const char *cmd, char **argv, bool do_fork/*是否需要先fork*/,
	     int (*setup)(void *), void *arg)
{
	fflush(stdout);
	if (do_fork) {
		int status;
		pid_t pid;

		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(1);
		}

		if (pid != 0) {
			/* Parent  */
		    /*父进程只需要等子进程结束*/
			if (waitpid(pid, &status, 0) < 0) {
				perror("waitpid");
				exit(1);
			}

			if (WIFEXITED(status)) {
				return WEXITSTATUS(status);
			}

			exit(1);
		}
	}

	if (setup && setup(arg))
		return -1;

	//执行对应的命令
	if (execvp(cmd, argv)  < 0)
		fprintf(stderr, "exec of \"%s\" failed: %s\n",
				cmd, strerror(errno));
	_exit(1);
}
