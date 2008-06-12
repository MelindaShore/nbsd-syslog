#ifndef SYSLOGD_H_
#define SYSLOGD_H_
/*
 * hold common data structures and prototypes 
 * for syslogd.c and tls_stuff.c
 * 
 */
 
#include <sys/cdefs.h>
#define MAXLINE         1024            /* maximum line length */
#define MAXSVLINE       120             /* maximum saved line length */
#define DEFUPRI         (LOG_USER|LOG_NOTICE)
#define DEFSPRI         (LOG_KERN|LOG_NOTICE)
#define TIMERINTVL      30              /* interval for checking flush, mark */
#define TTYMSGTIME      1               /* timeout passed to ttymsg */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <netinet/in.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <locale.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#ifndef _NO_NETBSD_USR_SRC_
#include <util.h>
#include "utmpentry.h"
#else
#include <libutil.h>
#include <utmp.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <limits.h>
#endif /* !_NO_NETBSD_USR_SRC_ */

#ifndef DISABLE_TLS
#include <netinet/tcp.h>
#include <sys/stdint.h>
#include <openssl/ssl.h>

/* simple message buffer container */
struct buf_msg {
        char *line;
        size_t len;
        unsigned short int refcount;
};

/* queue of messages */

struct buf_queue {
        struct buf_msg* msg;
        TAILQ_ENTRY(buf_queue) entries;
};
TAILQ_HEAD(buf_queue_head, buf_queue);
#endif /* !DISABLE_TLS */

#include "pathnames.h"

#include <sys/syslog.h>

#ifdef LIBWRAP
#include <tcpd.h>
#endif

#define FDMASK(fd)      (1 << (fd))

#define dprintf         if (Debug) printf

#define MAXUNAMES       20      /* maximum number of user names */

/*
 * Flags to logmsg().
 */

#define IGN_CONS        0x001   /* don't print on console */
#define SYNC_FILE       0x002   /* do fsync on file after printing */
#define ADDDATE         0x004   /* add a date to the message */
#define MARK            0x008   /* this message is a mark */
#define ISKERNEL        0x010   /* kernel generated message */

/*
 * This structure represents the files that will have log
 * copies printed.
 * We require f_file to be valid if f_type is F_FILE, F_CONSOLE, F_TTY,
 * or if f_type is F_PIPE and f_pid > 0.
 */

struct filed {
        struct  filed *f_next;          /* next in linked list */
        short   f_type;                 /* entry type, see below */
        short   f_file;                 /* file descriptor */
        time_t  f_time;                 /* time this was last written */
        char    *f_host;                /* host from which to record */
        u_char  f_pmask[LOG_NFACILITIES+1];     /* priority mask */
        u_char  f_pcmp[LOG_NFACILITIES+1];      /* compare priority */
#define PRI_LT  0x1
#define PRI_EQ  0x2
#define PRI_GT  0x4
        char    *f_program;             /* program this applies to */
        union {
                char    f_uname[MAXUNAMES][UT_NAMESIZE+1];
                struct {
                        char    f_hname[MAXHOSTNAMELEN];
                        struct  addrinfo *f_addr;
                } f_forw;               /* UDP forwarding address */
#ifndef DISABLE_TLS
                struct {
                        struct buf_queue_head qhead;    /* queue for undelivered msgs */
                        SSL     *ssl;                   /* SSL object  */
                        struct tls_conn_settings *tls_conn;  /* certificate info */ 
                } f_tls;                /* TLS forwarding address */
#endif /* !DISABLE_TLS */
                char    f_fname[MAXPATHLEN];
                struct {
                        char    f_pname[MAXPATHLEN];
                        pid_t   f_pid;
                } f_pipe;
        } f_un;
        char    f_prevline[MAXSVLINE];          /* last message logged */
        char    f_lasttime[16];                 /* time of last occurrence */
        char    f_prevhost[MAXHOSTNAMELEN];     /* host from which recd. */
        int     f_prevpri;                      /* pri of f_prevline */
        int     f_prevlen;                      /* length of f_prevline */
        int     f_prevcount;                    /* repetition cnt of prevline */
        int     f_repeatcount;                  /* number of "repeated" msgs */
        int     f_lasterror;                    /* last error on writev() */
        int     f_flags;                        /* file-specific flags */
#define FFLAG_SYNC      0x01
};


#ifndef DISABLE_TLS
/* TLS needs three sets of sockets:
 * - listening sockets: a fixed size array TLS_Listen_Set, just like finet for UDP.
 * - outgoing connections: managed as part of struct filed.
 * - incoming connections: variable sized, thus a linked list TLS_Incoming.
 */
/* every connection has its own input buffer with status
 * variables for message reading */
SLIST_HEAD(TLS_Incoming, TLS_Incoming_Conn);
 
struct TLS_Incoming_Conn {
        char inbuf[2*MAXLINE];           /* input buffer */
        SLIST_ENTRY(TLS_Incoming_Conn) entries;
        struct tls_conn_settings *tls_conn;
        SSL *ssl;
        int socket;
        uint_fast16_t cur_msg_len;       /* length of current msg */
        uint_fast16_t cur_msg_start;     /* beginning of current msg */
        uint_fast16_t read_pos;          /* ring buffer position to write to */
        uint_fast8_t errorcount;         /* to close faulty connections */
        bool closenow;                   /* close connection as soon as buffer processed */
};

#endif /* !DISABLE_TLS */

#endif /*SYSLOGD_H_*/