#include "avfscoda.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/time.h>

#include <sys/stat.h>

#include <linux/coda.h>


/* Keep file lookups cached for at least this many seconds: */
#define KEEPTIME 600

/* Flush attribute caches after this many seconds */
#define FLUSHTIME 2

/* Keep at most this many looked up files cached: */
#define MAXFILES 5000

/* Check looked up files after this many operations: */
#define CHECKNUM 1000

/* Maximum number of child processes running: */
#define MAXUSERS 10



#define MAXMSGLEN 1045


struct operation {
    struct operation *next;
    union inputArgs *req;
	
    char ibuf[MAXMSGLEN];
};


struct userinfo {
    uid_t uid;
    gid_t gid;
    volatile pid_t serverpid;
    int pipout;
    int pipin;
    time_t lastuse;
    int terminated;
    struct operation *ops;
};

static struct userinfo currusers[MAXUSERS];


static int codafd;
static const char *codadir;
static FILE *logfile;

static int numfids;
static int checknum;

static int debugmode;

struct openfile {
    pid_t pid;
    char *tmpfile;
    int use;
    int wuse;
	
    struct openfile *next;
};

struct fileinfo {
    struct fileinfo *subdir;
    struct fileinfo *next;
	
    char *name;
    char *path;
	
    time_t lasttime;
	
    struct openfile *ofs;
};

static struct fileinfo rootinfo;
static int needflush;

static void log(const char *fmt, ...)
{
    if(debugmode) {
	va_list ap;
	
	va_start(ap, fmt);
	vfprintf(logfile, fmt, ap);
	va_end(ap);
    }
}

static struct fileinfo *find_name(struct fileinfo *parentdir,
                                  const char *name)
{
    struct fileinfo *fi;

    for(fi = parentdir->subdir; fi != NULL; fi = fi->next) {
        if(strcmp(fi->name, name) == 0) {
            return  fi;
        }
    }

    return NULL;
}

static struct fileinfo *remove_name(struct fileinfo *parentdir,
                                    const char *name)
{
    struct fileinfo *fi, **fip;

    for(fip = &parentdir->subdir; *fip != NULL; fip = &(*fip)->next) {
        fi = *fip;
        if(strcmp(fi->name, name) == 0) {
            *fip = fi->next;
            free(fi->name);
            free(fi->path);
            fi->next = NULL;
            fi->name = NULL;
            fi->path = NULL;

            return fi;
        }
    }

    return NULL;
}

static void add_name(struct fileinfo *fi, struct fileinfo *parentdir,
                     const char *name)
{
    char buf[1024];

    fi->next = parentdir->subdir;
    parentdir->subdir = fi;
		
    fi->name = strdup(name);
	
    sprintf(buf, "%s/%s", parentdir->path, name);
    fi->path = strdup(buf);

}

static struct fileinfo *get_file(struct fileinfo *parentdir, const char *name)
{
    struct fileinfo *fi;
	
    fi = find_name(parentdir, name);
    if(fi != NULL)
        return fi;
	
    fi = malloc(sizeof(*fi));
    if(fi == NULL) {
        return NULL;
    }
	
    numfids ++;

    fi->subdir = NULL;
    fi->lasttime = time(NULL);
    fi->ofs = NULL;

    add_name(fi, parentdir, name);    
	
    return fi;
}

static void put_file(struct fileinfo *fi)
{
    if(fi->name != NULL || fi->ofs != NULL)
        return;

    if(fi->subdir != NULL) {
        log("Deleted nonepty dir\n");
        return;
    }

    free(fi);
    numfids --;
}


static struct fileinfo *look_info(ViceFid *id)
{
    struct fileinfo *fi;
	
    if ((id->Volume != 0) || (id->Vnode != 0)) {
        log("Bad handle passed %lx/%lx/%lx\n", 
            id->Volume, id->Vnode, id->Unique );
        clean_exit(1);
    }
	
    log("ptr: %p\n", (void *) id->Unique);
	
    if(id->Unique == 0) 
        return &rootinfo;
    else {
        fi =  (struct fileinfo *) id->Unique;
        fi->lasttime = time(NULL);
		
        return fi;
    }
}

static char *look_name(ViceFid *id)
{
    struct fileinfo *fi;
	
    fi = look_info(id);
    log("path: %s\n", fi->path);
	
    return fi->path;
}



static void reset_signal_handlers()
{
    struct sigaction sa;
	
    sa.sa_handler = SIG_DFL;
    sigemptyset(&(sa.sa_mask));
    sa.sa_flags = 0;
	
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
}


static void send_to_kernel(union outputArgs *rep, int size)
{
    int ret;
	
    log("%i bytes\n",  size);
    ret = write(codafd, rep, size);
    if(ret == -1 || ret != size) {
        log("Error writing to device: %s\n", strerror(errno));
    }
}

static struct fileinfo *create_file(const char *filename, ViceFid *parentid,
                                    ViceFid *newid)
{
    struct fileinfo *fi;

    fi = look_info(parentid);    
    fi = get_file(fi, filename);
    if(fi == NULL) 
        return NULL;

    newid->Volume = 0;
    newid->Vnode = 0;
    newid->Unique = (int) fi;

    return fi;
}


static void purge_file(struct fileinfo *fi)
{
    union outputArgs rep;

    log("=================================================================\n");
    log("Cleaning out %p\n", fi);
    log("CODA_PURGEFID\n");
    
    rep.oh.opcode = CODA_PURGEFID;
    rep.oh.result = 0;
    rep.oh.unique = 0;
    rep.coda_purgefid.CodaFid.Volume = 0;
    rep.coda_purgefid.CodaFid.Vnode  = 0;
    rep.coda_purgefid.CodaFid.Unique = (int) fi;
    
    send_to_kernel(&rep, sizeof(rep.coda_purgefid));
}

static void zap_file(struct fileinfo *fi)
{
    union outputArgs rep;

    log("=================================================================\n");
    log("Cleaning out %p\n", fi);
    log("CODA_ZAPFILE\n");
    
    rep.oh.opcode = CODA_ZAPFILE;
    rep.oh.result = 0;
    rep.oh.unique = 0;
    rep.coda_zapfile.CodaFid.Volume = 0;
    rep.coda_zapfile.CodaFid.Vnode  = 0;
    rep.coda_zapfile.CodaFid.Unique = (int) fi;
    
    send_to_kernel(&rep, sizeof(rep.coda_zapfile));
}

#if 0
static void zap_dir(struct fileinfo *fi)
{
    union outputArgs rep;

    log("=================================================================\n");
    log("Cleaning out %p\n", fi);
    log("CODA_ZAPDIR\n");
    
    rep.oh.opcode = CODA_ZAPDIR;
    rep.oh.result = 0;
    rep.oh.unique = 0;
    rep.coda_zapdir.CodaFid.Volume = 0;
    rep.coda_zapdir.CodaFid.Vnode  = 0;
    rep.coda_zapdir.CodaFid.Unique = (int) fi;
    
    send_to_kernel(&rep, sizeof(rep.coda_zapdir));
}
#endif


static void clean_up_dir(struct fileinfo *dir, time_t oldtime)
{
    struct fileinfo **fip, *fi;
	
    for(fip = &dir->subdir; *fip != NULL; ) {
        fi = *fip;
		
        if(fi->subdir != NULL) 
            clean_up_dir(fi, oldtime);
		
        if(fi->subdir == NULL && (!oldtime || fi->lasttime < oldtime)) {
            *fip = fi->next;

            free(fi->name);
            free(fi->path);
            fi->next = NULL;
            fi->name = NULL;
            fi->path = NULL;

            purge_file(fi);
            put_file(fi);
        }
        else 
            fip = &(*fip)->next;
    }
}

static void clean_up_names()
{
    clean_up_dir(&rootinfo, time(NULL) - KEEPTIME);
}


static void open_file(union inputArgs *req, struct openfile *of)
{
    int ret;
    union outputArgs rep;
    struct stat stbuf;
	
    rep.oh.opcode = req->ih.opcode;
    rep.oh.unique = req->ih.unique;
	
    ret = stat(of->tmpfile, &stbuf);
    if(ret == -1) 
        rep.oh.result = errno;
    else {
        rep.oh.result = 0;
        rep.coda_open.dev = stbuf.st_dev;
        rep.coda_open.inode = stbuf.st_ino;
		
        log("dev: %lli, ino: %li\n", rep.coda_open.dev, rep.coda_open.inode);
        log("size: %li\n", stbuf.st_size);
        of->use ++;
        if((req->coda_open.flags & (C_O_WRITE | C_O_TRUNC)) != 0)
            of->wuse ++;
    }
	
    send_to_kernel(&rep, sizeof(rep.coda_open));
}

static void del_file(const char *tmpname)
{
    int res;
	
    /* Coda holds on to the inode, so to free up space: */
    truncate(tmpname, 0);
    res = unlink(tmpname);
    if(res == -1)
        fprintf(stderr, "unlink(%s) failed (%s)\n", 
                tmpname, strerror(errno));
}

static void close_file(struct openfile *of, struct openfile **ofp)
{
    
    if(of->use > 0) of->use --;
    if(of->use == 0 && of->tmpfile != NULL) {
        del_file(of->tmpfile);
        free(of->tmpfile);
        *ofp = of->next;
        free(of);
    }
}


static void reply(union inputArgs *req, int res)
{
    union outputArgs rep;
	
    rep.oh.opcode = req->ih.opcode;
    rep.oh.unique = req->ih.unique;
    rep.oh.result = res;
	
    send_to_kernel(&rep, sizeof(rep.oh));
}

static void check_servers()
{
    int i;
	
    checknum ++;
    if(numfids > MAXFILES && checknum > CHECKNUM) {
        clean_up_names();
        checknum = 0;
    }
	
    for(i = 0; i < MAXUSERS; i++) {
        if(currusers[i].serverpid == 0) {
            /* FIXME: reply to the pending messages */
			
            close(currusers[i].pipout);
            close(currusers[i].pipin);
			
            currusers[i].serverpid = -1;
        }
    }
}


static void process_answer(struct userinfo *user)
{
    int numread;
    char obuf[MAXMSGLEN];
    union outputArgs *rep = (union outputArgs *) obuf;
    struct operation *op, **opp;
    struct fileinfo *fi;
    struct openfile *of, **ofp;
    char *filename;
    int insize;

    if(!needflush)
        needflush = time(NULL);

    numread = read(user->pipin, &insize, sizeof(insize));
    if(numread == -1) {
        log("Error reading from device: %s\n", strerror(errno));
        return;
    }
    if(insize > MAXMSGLEN || insize <= 0) {
        log("Error: illegal size");
        return;
    }
	
    numread = read(user->pipin, obuf, insize);
    if(numread == -1) {
        log("Error reading from child [%i/%i]: %s\n", 
            user->uid, user->gid, strerror(errno));
        return;
    }
	
    log("+ %i/%i [%i] +++++++++++++++++++++++++++++++++++++++++++++++++++\n",
        user->uid, user->gid, user->serverpid);
    log("%i (%i) bytes: opcode: %li, result: %i, unique: %li\n", 
        numread, insize, rep->oh.opcode, rep->oh.result, rep->oh.unique);
	
    for(opp = &user->ops; *opp != NULL; opp = &(*opp)->next) 
        if((*opp)->req->ih.unique == rep->oh.unique) break;
	
    op = *opp;
	
    if(op == NULL)
        log("Operation not found!!!!\n");
    else {
        log("Found operation: %i\n", op->req->ih.unique);
		
        switch(rep->oh.opcode) {
        case CODA_OPEN:
            if(rep->oh.result == 0) {
                fi = look_info(&op->req->coda_open.VFid);
				
                for(of = fi->ofs; of != NULL; of = of->next) 
                    if(of->pid == op->req->ih.pid) break;
				
                if(of == NULL) {
                    log("Output file not found!!!\n");
                    reply(op->req, ENOENT);
                }
                else {
                    open_file(op->req, of);
                }
            }
            else 
                send_to_kernel(rep, numread);
            break;
        
        case CODA_CLOSE:
            fi = look_info(&op->req->coda_close.VFid);
            
            for(ofp = &fi->ofs; *ofp != NULL; ofp = &(*ofp)->next)
                if((*ofp)->pid == op->req->ih.pid) break;
            
            of = *ofp;

            if(of == NULL) {
                log("Output file not found!!!\n");
                reply(op->req, ENOENT);
            }
            else {
                close_file(of, ofp);
                put_file(fi);
            }

            send_to_kernel(rep, numread);
	    zap_file(fi);
            break;
			
        case CODA_LOOKUP:
            if(rep->oh.result == 0) {
                filename = (char *) op->req + op->req->coda_lookup.name;
                fi = create_file(filename, &op->req->coda_lookup.VFid,
                                 &rep->coda_lookup.VFid);
                if(fi == NULL)
                    rep->oh.result = ENOMEM;
            }
            send_to_kernel(rep, numread);
            break;

        case CODA_CREATE:
            if(rep->oh.result == 0) {
                filename = (char *) op->req + op->req->coda_create.name;
                fi = create_file(filename, &op->req->coda_create.VFid,
                                 &rep->coda_create.VFid);
                if(fi == NULL)
                    rep->oh.result = ENOMEM;
            }
            send_to_kernel(rep, numread);
            break;

        case CODA_MKDIR:
            if(rep->oh.result == 0) {
                filename = (char *) op->req + op->req->coda_mkdir.name;
                fi = create_file(filename, &op->req->coda_mkdir.VFid,
                                 &rep->coda_mkdir.VFid);
                if(fi == NULL)
                    rep->oh.result = ENOMEM;
            }
            send_to_kernel(rep, numread);
            break;

        case CODA_REMOVE:
            if(rep->oh.result == 0) {
                filename = (char *) op->req + op->req->coda_remove.name;
                fi = look_info(&op->req->coda_remove.VFid);
                fi = remove_name(fi, filename);
                if(fi != NULL) {
                    purge_file(fi);
                    put_file(fi);
                }
            }
            send_to_kernel(rep, numread);
            break;

        case CODA_RMDIR:
            if(rep->oh.result == 0) {
                filename = (char *) op->req + op->req->coda_rmdir.name;
                fi = look_info(&op->req->coda_rmdir.VFid);
                fi = remove_name(fi, filename);
                if(fi != NULL)
                    put_file(fi);
            }
            send_to_kernel(rep, numread);
            break;
            
        case CODA_RENAME:
            if(rep->oh.result == 0) {
                char *newname;
                struct fileinfo *newfi;

                newname = (char *) op->req + op->req->coda_rename.destname;
                newfi = look_info(&op->req->coda_rename.destFid);
                fi = remove_name(newfi, newname);
                if(fi != NULL) {
                    purge_file(fi);
                    put_file(fi);
                }

                filename = (char *) op->req + op->req->coda_rename.srcname;
                fi = look_info(&op->req->coda_rename.sourceFid);
                fi = remove_name(fi, filename);
                if(fi != NULL)
                    add_name(fi, newfi, newname);
            }
            send_to_kernel(rep, numread);
            break;

        default:
            send_to_kernel(rep, numread);
			
        }
		
        *opp = op->next;
        free(op);
    }
	
    if(user->ops != NULL) {
        log("Remaining operations: ");
        for(op = user->ops; op != NULL; op = op->next)
            log("%i ", op->req->ih.unique);
        log("\n");
    }
	
	
}

static void process_child_answer()
{
    fd_set rfds;
    int ret;
    int maxfd;
    int i;
	
    check_servers();
	
    FD_ZERO(&rfds);
	
    maxfd = 0;
	
    for(i = 0; i < MAXUSERS; i++) {
        if(currusers[i].serverpid > 0) {
            int pipfd = currusers[i].pipin;
			
            FD_SET(pipfd, &rfds);
            if(pipfd > maxfd) maxfd = pipfd;
        }
    }
	
    ret = select(maxfd+1, &rfds, NULL, NULL, NULL);
    if(ret == -1) {
        if(errno != EINTR) 
            log("Select failed: %s\n", strerror(errno));
    }
    else {
        for(i = 0; i < MAXUSERS; i++) {
            if(currusers[i].serverpid > 0) {
                int pipfd = currusers[i].pipin;
				
                if(FD_ISSET(pipfd, &rfds)) 
                    process_answer(&currusers[i]);
            }
        }
    }
}

static void kill_child()
{
    struct userinfo *user;
    time_t oldesttime;
    int oldesti;
    int i;
    pid_t pid;
	
    oldesttime = 0;
    oldesti = -1;
	
    do {
        for(i = 0; i < MAXUSERS; i++) {
            user = currusers + i;
            if(user->serverpid == -1) return;
            if(user->serverpid == 0) {
                check_servers();
                return;
            }
			
            if(user->ops == NULL) {
                if(oldesti == -1 || 
                   user->lastuse < oldesttime) {
                    oldesttime = user->lastuse;
                    oldesti = i;
                }
            }
        }
		
        if(oldesti == -1) {
            /* If every child is busy then block */
            process_child_answer();
        }
    } while(oldesti == -1);
	
    user = currusers + oldesti;
	
    /* FIXME: This is a mess, because user->serverpid can change to 0 
       when SIGCHLD is received */
    pid = user->serverpid;
    if(pid > 0) {
        if(!user->terminated) {
            kill(pid, SIGTERM);
            user->terminated = 1;
        }
        else {
            log("kill(%i, SIGKILL)\n", pid);
            kill(pid, SIGKILL);
        }
    }
	
    /* FIXME: Need to wait for the death of the child or max 1 second. 
       How can this be done? */
    if(user->serverpid > 0)
        sleep(1);
	
    check_servers();
}


static int new_child(struct userinfo *user, uid_t uid, gid_t gid)
{
    int pipout[2];
    int pipin[2];
    int pid;
    int i;
    gid_t list[32];
    int num;
	
    if(pipe(pipout) == -1) {
        log("Could not open pipe for child: %s\n", strerror(errno));
        return -1;
    }
    if(pipe(pipin) == -1) {
        close(pipout[0]);
        close(pipout[1]);
        return -1;
    }
	
    user->serverpid = pid = fork();
    if(pid == -1) {
        close(pipout[0]);
        close(pipout[1]);
        close(pipin[0]);
        close(pipin[1]);
        log("Could not fork child: %s\n", strerror(errno));
        return -1;
    }
	
    user->pipout = pipout[1];
    user->pipin = pipin[0];
	
    if(pid == 0) {
        /* Child */
		
        reset_signal_handlers();
		
        /* Close everything, except the current pipes */
        for(i = 0; i < MAXUSERS; i++) 
            if(currusers[i].serverpid >= 0) {
                close(currusers[i].pipout);
                close(currusers[i].pipin);
            }
		
        close(codafd);
#if 0
        fclose(logfile);
#endif
        /* Don't want any troublesome signals from the child */
        setsid(); 
		
        /* FIXME: What is the proper way of dealing with
           supplementary groups? */
        list[0] = gid;
        num = 1;
        setgroups(num, list);
		
#if 0
        {
            struct passwd *pwbuf;
			
            /* FIXME: This messes up gdb. Why? */
            pwbuf = getpwuid(uid);
            if(pwbuf != NULL)
                initgroups(pwbuf->pw_name, gid);
        }
#endif
		
        setgid(gid);
        setuid(uid);
		
        fprintf(stderr, "Child process: %i/%i\n", getuid(), getgid());
		
        num = getgroups(32, list);
        fprintf(stderr, "Supplementary groups: ");
        for(i = 0; i < num; i++) fprintf(stderr, "%i, ", list[i]);
        fprintf(stderr, "\n");
		
        child_process(pipout[0], pipin[1]);
        exit(0);
    }
	
    /* Parent */
	
    close(pipout[0]);
    close(pipin[1]);
	
    user->uid = uid;
    user->gid = gid;
    user->terminated = 0;
    user->lastuse = 0;
    user->ops = NULL;
	
    return 0;
}

static struct userinfo *get_user(uid_t uid, gid_t gid)
{
    int i;
    struct userinfo *user = NULL;
	
    for(i = 0; i < MAXUSERS; i++) {
        if(currusers[i].serverpid > 0 && currusers[i].uid == uid &&
           currusers[i].gid == gid) {
            user = &currusers[i];
            break;
        }
    }
	
    if(user == NULL) {
        /* Create child */
        do {
            /* Find a free slot */
            for(i = 0; i < MAXUSERS; i++)
                if(currusers[i].serverpid == -1) break;
			
            if(i == MAXUSERS) {
				/* No free slots, must kill a child */
                kill_child();
            }
        } while(i == MAXUSERS);
		
        user = currusers + i;
        if(new_child(user, uid, gid) == -1)
            return NULL;
    }
	
    user->lastuse = time(NULL);
    return user;
}

static void send_to_child(union inputArgs *req, int reqsize, char *path1,
			  char *path2)
{
    struct operation *op;
    uid_t uid = req->ih.cred.cr_fsuid;
    gid_t gid = req->ih.cred.cr_fsgid;
    struct userinfo *user;
    int msgsize;
    struct child_message msg;
    char *message, *mp;
    int msgoff;
    int res;
	
    user = get_user(uid, gid);
    if(user == NULL) {
        reply(req, ENOMEM);
        return;
    }
	
    msg.reqsize = reqsize;
    msg.path1size = path1 ? strlen(path1) + 1 : 0;
    msg.path2size = path2 ? strlen(path2) + 1 : 0;
    msgoff = sizeof(struct child_message);
	
    msgsize = sizeof(int) + msgoff + msg.reqsize + msg.path1size + 
        msg.path2size;
	
    message = malloc(msgsize);
    if(message == NULL) {
        reply(req, ENOMEM);
        return;
    }
	
    op = malloc(sizeof(struct operation));
    if(op == NULL) {
        free(message);
        reply(req, ENOMEM);
        return;
    }
	
    memcpy(op->ibuf, req, reqsize);
    op->req = (union inputArgs *) op->ibuf;
	
    mp = message;
	
    *(int *) mp = (msgsize - sizeof(int));
    mp += sizeof(int);
	
    memcpy(mp, &msg, msgoff);
    mp += msgoff;
	
    memcpy(mp, req, msg.reqsize);
    mp += msg.reqsize;
	
    log("****** opcode: %li\n", req->ih.opcode);
    log("****** msgsize: %i, msgoff: %i, msg.reqsize: %i, \n", 
        msgsize, msgoff, msg.reqsize);
	
    if(path1) {
        memcpy(mp, path1, msg.path1size);
        mp += msg.path1size;
    }
	
    if(path2) {
        memcpy(mp, path2, msg.path2size);
        mp += msg.path2size;
    }
	
    res = write(user->pipout, message, msgsize);
    free(message);
	
    if(res != msgsize) {
        free(op);
        log("Error writing to child: %s\n", strerror(errno));
		
        reply(req, errno);
    }
    else {
        op->next = user->ops;
        user->ops = op;
    }
}

static void send_with_path(union inputArgs *req, int reqsize, char *filename,
                           ViceFid *id, char *path2)
{
    char pathbuf[1024];
    struct fileinfo *fi;
    char *path;

    fi = look_info(id);
    path = fi->path;
		
    sprintf(pathbuf, "%s/%s", path, filename);

    log("path1: %s, path2: %s\n", pathbuf, path2 ? path2 : "(null)");

    /* FIXME: */
    if(strcmp(pathbuf+1, codadir) == 0) 
            reply(req, ENOENT);
    else 
            send_to_child(req, reqsize, pathbuf, path2);
}


static void coda_flush()
{
    union outputArgs rep;
	
    log("=================================================================\n");
    log("CODA_FLUSH\n");
	
    rep.oh.opcode = CODA_FLUSH;
    rep.oh.result = 0;
    rep.oh.unique = 0;
	
    send_to_kernel(&rep, sizeof(rep.oh));
}

void run_exit()
{
    int i;
	
    for(i = 0; i < MAXUSERS; i++) {
        if(currusers[i].serverpid > 0) {
            kill(currusers[i].serverpid, SIGTERM);
        }
    }
	
    /* FIXME: should wait until the children are all dead */
	
    coda_flush();
    close(codafd);
    unmount_coda(codadir, 0);
}

void user_child(pid_t pid)
{
    int i;
	
    for(i = 0; i < MAXUSERS; i++) {
        if(currusers[i].serverpid == pid) {
            log("Child %i (%i/%i) exited\n", 
                pid, currusers[i].uid, 
                currusers[i].gid);
			
            currusers[i].serverpid = 0;
			
            return;
        }
    }
	
    log("Unknown child %i exited\n", pid);
}

static void process_kernel_req()
{
    char ibuf[MAXMSGLEN];
    char pathbuf[1024];
    union inputArgs *req = (union inputArgs *) ibuf;
    union outputArgs rep;
    struct openfile *of, **ofp;
    int numread;
    char *path;
    char *filename, *filename2;
    struct fileinfo *fi;
    struct operation **opp, *op;
    int i;
	
    numread = read(codafd, ibuf, MAXMSGLEN);
    if(numread == -1) {
        log("Error reading from device: %s\n", strerror(errno));
        clean_exit(1);
    }
	
    log("=================================================================\n");
    log("%i bytes: opcode: %li, unique: %li\n", 
        numread, req->ih.opcode, req->ih.unique);
	
    switch (req->ih.opcode) {
    case CODA_ROOT:
        log("CODA_ROOT\n");
		
        rep.oh.opcode = req->ih.opcode;
        rep.oh.unique = req->ih.unique;
        rep.oh.result = 0;
        rep.coda_root.VFid.Volume = 0;
        rep.coda_root.VFid.Vnode  = 0;
        rep.coda_root.VFid.Unique = 0;           /* 0 means root */
		
        send_to_kernel(&rep, sizeof(rep.coda_root));
        break;
		
    case CODA_GETATTR:
        log("CODA_GETATTR\n");
        path = look_name(&req->coda_getattr.VFid);
        send_to_child(req, numread, path, NULL); 
        break;
		
    case CODA_ACCESS:
        log("CODA_ACCESS, flags: 0x%04x\n", req->coda_access.flags);
		
        path = look_name(&req->coda_access.VFid);
        send_to_child(req, numread, path, NULL);
        break;
		
    case CODA_OPEN:
        /* FIXME: I don't like this !!! */
        log("CODA_OPEN, flags: 0x%04x\n", req->coda_open.flags);
		
        fi = look_info(&req->coda_open.VFid);
        path = fi->path;
        log("path: %s\n", path);
		
        for(of = fi->ofs; of != NULL; of = of->next) 
            if(of->pid == req->ih.pid) break;
		
        if(of != NULL) {
            if((req->coda_open.flags & C_O_TRUNC) != 0) 
                truncate(of->tmpfile, 0);

            open_file(req, of);
        }
        else {
            char tmpname[64];
			
            strcpy(tmpname, "/tmp/.avfs_coda_XXXXXX");
            mktemp(tmpname);
			
            if(tmpname[0] == '\0') {
                log("Could not make temporary file\n");
                reply(req, ENFILE);
            }
            else {
                of = malloc(sizeof(struct openfile));
                if(of == NULL) {
                    reply(req, ENOMEM);
                }
                else {
                    of->use = 0;
                    of->wuse = 0;
                    of->pid = req->ih.pid;
                    of->tmpfile = strdup(tmpname);
                    of->next = fi->ofs;
                    fi->ofs = of;
					
                    log("tmpfile: %s\n", of->tmpfile);
                    send_to_child(req, numread, path, tmpname);
                }
            }
        }
        break;
		
    case CODA_CLOSE:
        log("CODA_CLOSE, flags: 0x%04x\n", req->coda_close.flags);
		
        fi = look_info(&req->coda_close.VFid);
        path = fi->path;
        log("path: %s\n", path);
		
        for(ofp = &fi->ofs; *ofp != NULL; ofp = &(*ofp)->next)
            if((*ofp)->pid == req->ih.pid) break;
		
        of = *ofp;
		
        if(of == NULL) {
            log("File not found\n");
            reply(req, ENOENT);
        }
        else {
            int dowrite = 0;

            log("use: %i\n", of->use);
            log("wuse: %i\n", of->wuse);
            if(of->wuse > 0 &&
               (req->coda_close.flags & (C_O_WRITE | C_O_TRUNC)) != 0) {
                of->wuse --;
                
                if(of->wuse == 0 && of->tmpfile != NULL) {
                    log("tmpfile: %s\n", of->tmpfile);
                    dowrite = 1;
                    send_to_child(req, numread, path, of->tmpfile);
                }
            }
            if(!dowrite) {
                close_file(of, ofp);
                reply(req, 0);
            }
        }
        break;
		
    case CODA_LOOKUP:
        /* It is not clear to me, whether lookups should be
           done as 'user' or as 'root' */
		
        filename = ibuf + req->coda_lookup.name;
		
        log("CODA_LOOKUP, name: '%s', flags: 0x%04x\n", 
            filename, req->coda_lookup.flags);
		
        send_with_path(req, numread, filename, &req->coda_lookup.VFid, NULL);
        break;

    case CODA_CREATE:
        filename = ibuf + req->coda_create.name;
		
        log("CODA_CREATE, name: '%s', mode: 0%o, rdev: 0x%04x\n", 
            filename, req->coda_create.mode, req->coda_create.attr.va_rdev);
		
        send_with_path(req, numread, filename, &req->coda_create.VFid, NULL);
        break;
		
    case CODA_READLINK:
        log("CODA_READLINK\n");
		
        path = look_name(&req->coda_readlink.VFid);
        send_to_child(req, numread, path, NULL);
        break;

    case CODA_SETATTR:
        log("CODA_SETATTR\n");

        path = look_name(&req->coda_setattr.VFid);
        send_to_child(req, numread, path, NULL);
        break;

    case CODA_REMOVE:
        filename = ibuf + req->coda_remove.name;
		
        log("CODA_REMOVE, name: '%s'\n", filename);
	       
        send_with_path(req, numread, filename, &req->coda_remove.VFid, NULL);
        break;

    case CODA_RMDIR:
        filename = ibuf + req->coda_rmdir.name;
        
        log("CODA_RMDIR, name: '%s'\n", filename);
	       
        send_with_path(req, numread, filename, &req->coda_rmdir.VFid, NULL);
        break;
        
    case CODA_MKDIR:
        filename = ibuf + req->coda_mkdir.name;
        
        log("CODA_MKDIR, name: '%s', mode: 0%o\n", filename, 
            req->coda_mkdir.attr.va_mode);
	       
        send_with_path(req, numread, filename, &req->coda_mkdir.VFid, NULL);
        break;

    case CODA_RENAME:
        filename = ibuf + req->coda_rename.srcname;
        filename2 = ibuf + req->coda_rename.destname;

        log("CODA_RENAME, name1: '%s', name2: '%s'\n", filename, filename2); 

        fi = look_info(&req->coda_rename.destFid);
        sprintf(pathbuf, "%s/%s", fi->path, filename2);
        
        send_with_path(req, numread, filename, &req->coda_rename.sourceFid,
                       pathbuf);
        break;

    case CODA_SYMLINK:
        filename = ibuf + req->coda_symlink.srcname;
        filename2 = ibuf + req->coda_symlink.tname;

        log("CODA_SYMLINK, src: '%s', tname: '%s'\n", filename, filename2); 
        
        send_with_path(req, numread, filename2, &req->coda_symlink.VFid,
                       filename);
        break;

    case CODA_LINK:
        fi = look_info(&req->coda_link.sourceFid);
        filename = fi->path;
        filename2 = ibuf + req->coda_link.tname;        
        
        log("CODA_LINK, src: '%s', tname: '%s'\n", filename, filename2);

        send_with_path(req, numread, filename2, &req->coda_link.destFid,
                       filename);
        break;

    case CODA_SIGNAL:
        log("CODA_SIGNAL\n");
        for(i = 0; i < MAXUSERS; i++) {
            for(opp = &currusers[i].ops; *opp != NULL; 
                opp = &(*opp)->next) 
                if((*opp)->req->ih.unique == req->ih.unique) 
                    break;
			
            if(*opp != NULL) break;
        }
        op = *opp;
		
        if(op == NULL) 
            log("Operation not found!!!!\n");
        else {
            /* FIXME: Inform the child that the operation
               is interrupted */
            *opp = op->next;
            free(op);
        }
        break;
		
    default:
        reply(req, EPERM);
		
        log("========================================\n");
        log("     N o t   I m p l e m e n t e d      \n");
        log("========================================\n");
    }	
}

static void process()
{
    fd_set rfds;
    int ret;
    int maxfd;
    int i;
	
    rootinfo.subdir = NULL;
    rootinfo.next = NULL; /* never used */
    rootinfo.name = NULL; /* never used */
    rootinfo.path = "/";
	
    while(1) {
        struct timeval timeout;

        check_servers();
		
        FD_ZERO(&rfds);
		
        FD_SET(codafd, &rfds);
        maxfd = codafd;
		
        for(i = 0; i < MAXUSERS; i++) {
            if(currusers[i].serverpid > 0) {
                int pipfd = currusers[i].pipin;
				
                FD_SET(pipfd, &rfds);
                if(pipfd > maxfd) maxfd = pipfd;
            }
        }

        timeout.tv_sec = 2;
        timeout.tv_usec = 0;
		
        ret = select(maxfd+1, &rfds, NULL, NULL, &timeout);
        if(ret == -1) {
            if(errno == EINTR)
                continue;
            log("Select failed: %s\n", strerror(errno));
            continue;
        }
        
        if(needflush && needflush + FLUSHTIME <= time(NULL)) {
            coda_flush();
            needflush = 0;
        }

        if(ret == 0)
            continue;

        log("Numfids: %i\n", numfids);
        

        if(FD_ISSET(codafd, &rfds))
            process_kernel_req();
		
        for(i = 0; i < MAXUSERS; i++) {
            if(currusers[i].serverpid > 0) {
                int pipfd = currusers[i].pipin;
				
                if(FD_ISSET(pipfd, &rfds))
                    process_answer(&currusers[i]);
            }
        }
    }
}


void run(int cfs, const char *dir, int dm)
{
    int i;
	
    codafd = cfs;
    codadir = dir;
    logfile = stderr;
    numfids = 0;
    checknum = 0;
    debugmode = dm;
	
    for(i = 0; i < MAXUSERS; i++)
        currusers[i].serverpid = -1;
	
    set_signal_handlers();
	
    process();
}