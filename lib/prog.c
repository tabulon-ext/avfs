/*
    AVFS: A Virtual File System Library
    Copyright (C) 2000-2001  Miklos Szeredi (mszeredi@inf.bme.hu)

    This file can be distributed either under the GNU LGPL, or under
    the GNU GPL. See the file COPYING.LIB and COPYING. 
*/

#include "prog.h"
#include "avfs.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

char *strsignal(int sig);

void __av_init_proginfo(struct proginfo *pi)
{
    pi->prog = NULL;
    pi->pid = -1;

    pi->wd = NULL;
    pi->ifd = -1;
    pi->ofd = -1;
    pi->efd = -1;
}

static char *get_cmdline(const char *args[])
{
    char *cat = NULL;

    for(; *args != NULL; args++)
        cat = __av_stradd(cat, *args, " ", NULL);

    return cat;
}

int __av_start_prog(struct proginfo *pi)
{
    char *cmdline = get_cmdline(pi->prog);
    __av_log(AVLOG_DEBUG, "Starting program %s", cmdline);
    __av_free(cmdline);

    pi->pid = fork();
  
    if(pi->pid == -1) {
        __av_log(AVLOG_ERROR, "Fork failed: %s", strerror(errno));
        return -EIO;
    }
    if(pi->pid == 0) { /* Child */
        if(pi->wd != NULL) 
            chdir(pi->wd);
    
        setsid(); /* Don't want to kill my parent if something goes wrong */

        if(pi->ifd != -1) dup2(pi->ifd, 0);
        if(pi->ofd != -1) dup2(pi->ofd, 1);
        if(pi->efd != -1) dup2(pi->efd, 2);

        execvp(pi->prog[0], (char **) pi->prog);
        __av_log(AVLOG_ERROR, "Failed to exec %s", pi->prog[0]);
        _exit(1);
    }

    return 0;
}

int __av_wait_prog(struct proginfo *pi, int tokill, int check)
{
    int res;
    int retv;

    if(pi->pid == -1)
        return 1;

    if(check) {
        res = waitpid(pi->pid, &retv, WNOHANG);
        if(res == 0)
            return 0;
    }
    else {
        if(tokill)
            kill(pi->pid, SIGKILL);
        
        do res = waitpid(pi->pid, &retv, 0);
        while(res == -1 && errno == EINTR);
    }
    pi->pid = -1;

    if(res == -1) {
        __av_log(AVLOG_ERROR, "waitpid returned error: %s", strerror(errno));
        return -EIO;
    }

    if(WIFEXITED(retv)) {
        int val = WEXITSTATUS(retv);
        if(val == 0) {
            __av_log(AVLOG_DEBUG, "program %s exited normally", pi->prog[0]);
            return 1;
        }
        
        __av_log(AVLOG_ERROR, "program %s exited with error: %i", pi->prog[0],
                 val);
    }
    else if(WIFSIGNALED(retv))
        __av_log(AVLOG_ERROR, "program %s was killed by signal %s",
                 pi->prog[0],  strsignal(WTERMSIG(retv)));
    else
        __av_log(AVLOG_ERROR, "program %s killed with unknown reason",
                 pi->prog[0]);

    return -EIO;
}


