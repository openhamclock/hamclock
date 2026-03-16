/* provide a proxy from localhost:8081 to multiple simultaneous web instances of hamclock.
 * hamclock preferences are presistent for each unique browser IP.
 *
 * Summary of program design:
 *   main process: listens for new browser connections, creates one shepherding pthread for each.
 *   shepherding pthread: checks the remote IP of the new browser connection. If a hamclock process is
 *     already running on behalf of that IP, anoher connection is made to the same hamclock. Otherwise a
 *     new hamclock instance is created for that IP. Thenceforth either way, copy all traffic between
 *     the browser and its hamclock instance as if they were directly connected. The thread ends when
 *     either socket sees EOF or error. When the last browser connection ends for a hamclock, it is sent
 *     SIGSTOP. If a new connection later arrives for the same IP it was serving, then it is sent SIGCONT
 *     so it may continue with the same context. A process that remains unused for longer than the time
 *     specified by -i may be killed for lack of use.
 * Additional notes:
 *   multiple simultaneous connections from the same IP connect to the same instance of hamclock. This
 *     is easy to do because hamclock itself supports multiple simultaneous connections to its -w API port.
 *   hamclock settings and supporting files are retained in a directory named with the IP of the browser
 *     that first caused it to be created. This directory may be specified with the -d option.
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>


// config and state info

#define VERSION         1.11                            // program version -- see history
#define DEF_PORT        8081                            // default public listen port
#define DEF_MAXHC       10                              // default max number of active hamclock processes
#define DEF_MAXAGE      (24*3600)                       // default process idle time before killing, seconds
static const char *DEF_HCNAME = "hamclock";             // default name of hamclock program

static int verbose;                                     // more chatty
static int max_hamclocks = DEF_MAXHC;                   // max number of active hamclock processes allowed
static int max_idle = DEF_MAXAGE;                       // max process idle time before killing, seconds
static const char *hc_name = DEF_HCNAME;                // name of hamclock program to execute
static char *hc_basedir;                                // base dir of tree of config files
static char **hc_argv;                                  // -- args for hamclock
static int hc_argc;                                     // n in hc_argv
static int api_port;                                    // first of sequence of RESTful ports, unless 0

// info about each hamclock process
typedef struct {
    char IP[INET_ADDRSTRLEN];                           // browser IP this hamclock serves
    int pid;                                            // process ID if active, even if stopped; 0 if unused
    int port;                                           // -w port
    int count;                                          // n clients using pid, iff pid > 0
    time_t sleep;                                       // when last client was closed
} HCProc;
static HCProc *hcprocs;                                 // malloced array of max_hamclocks HCProcs
static pthread_mutex_t hcprocs_lock = PTHREAD_MUTEX_INITIALIZER;        // access guard

// HCProc state testing macros
#define HCP_IDLEDT(hcp)       (time(NULL) - (hcp)->sleep)               // idle time, secs
#define HCP_ISIDLING(hcp)     (isRealHCProc(hcp) && (hcp)->count == 0)  // process exists but no users 
#define HCP_ISRUNNING(hcp)    (isRealHCProc(hcp) && (hcp)->count > 0)   // process is in active use
#define HCP_ISASSIGNED(hcp)   (HCP_ISRUNNING(hcp) || HCP_ISIDLING(hcp)) // process is real, even if unused
#define HCP_ISUNUSED(hcp)     (!isRealHCProc(hcp) || (hcp)->count == 0) // this record is unused
#define HCP_RESET(hcp)        ((hcp)->pid = 0)                          // reset this record as unused


#if defined(__GNUC__)
// tell g++ to check our variadic log function
static void log (int v, const char *my_format, ...) __attribute__ ((format (__printf__, 2, 3)));
#endif

/* return whether the given process really exists
 */
static bool isRealHCProc (HCProc *hcp)
{
    if (hcp->pid > 0) {
        if (kill (hcp->pid, 0) == 0)
            return (true);
        log (0, "pid %d: disappeared\n", hcp->pid);
        HCP_RESET (hcp);
    }
    return (false);
}

/* print a log entry prefix to the given stream
 */
static void logPrefix (FILE *fp)
{
        // print some sort of thread id??
        printf ("thread %03u ", (uint8_t) (((uint64_t)pthread_self() >> 14) & 0xff));

        // time to ms
        struct timeval tv;
        gettimeofday (&tv, NULL);
        struct tm *tmp = gmtime(&tv.tv_sec);
        fprintf (fp, "%02d:%02d:%02d.%03d Z: ", tmp->tm_hour, tmp->tm_min, tmp->tm_sec,
                        (int)(tv.tv_usec/1000));
}

/* print printf-style msg to stdout depending on verbose
 */
static void log (int v, const char *fmt, ...)
{
        if (v > verbose)
            return;

        // keep messages from different threads from overlapping
        static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock (&print_lock);

        // print prefix
        logPrefix (stdout);

        // print message
        va_list ap;
        va_start (ap, fmt);
        vprintf (fmt, ap);
        va_end (ap);

        pthread_mutex_unlock (&print_lock);
}

/* kill the given HCProc and reset hcp.
 * N.B. we assume hcprocs_lock is already acquired
 */
static void terminateHCProc (HCProc *hcp)
{
        if (kill (hcp->pid, SIGCONT) < 0 || kill (hcp->pid, SIGTERM) < 0)
            log (0, "pid %d: failed to terminate %s: %s\n", hcp->pid, hc_name, strerror(errno));
        else
            log (1, "pid %d: terminated %s\n", hcp->pid, hc_name);
        HCP_RESET (hcp);
}

/* report a fatal error, kill all hamclocks, print printf-style msg to stdout and exit(1)
 */
static void fatal (const char *fmt, ...)
{
        // create message
        char msg[1000];
        va_list ap;
        va_start (ap, fmt);
        vsnprintf (msg, sizeof(msg), fmt, ap);
        va_end (ap);

        // log
        log (0, "fatal: %s", msg);      // msg already includes \n

        // kill all hamclocks
        for (int i = 0; i < max_hamclocks; i++) {
            HCProc *hcp = &hcprocs[i];
            if (HCP_ISASSIGNED(hcp))
                terminateHCProc (hcp);
        }

        // outta here
        exit (1);
}

/* suspend the given HCProc and update state.
 * N.B. we assume hcprocs_lock is already acquired
 */
static void idleHCProc (HCProc *hcp)
{
        if (kill (hcp->pid, SIGSTOP) < 0) {
            log (0, "pid %d: failed to stop %s: %s\n", hcp->pid, hc_name, strerror(errno));
            HCP_RESET (hcp);
        } else {
            log (2, "pid %d: stopped %s\n", hcp->pid, hc_name);
            hcp->sleep = time(NULL);
            hcp->count = 0;
        }
}

/* increment usage count of the given hcp, resuming process if currently idling.
 * N.B. we assume hcprocs_lock is already acquired
 */
static void addUserHCProc (HCProc *hcp)
{
        if (HCP_ISIDLING(hcp)) {
            log (1, "pid %d: resuming %s on port %d for %s after %d s idle\n",
                        hcp->pid, hc_name, hcp->port, hcp->IP, (int)(time(NULL)-hcp->sleep));
            if (kill (hcp->pid, SIGCONT) < 0)
                log (0, "pid %d: failed to resume %s: %s\n", hcp->pid, hc_name, strerror(errno));
        }
        hcp->count += 1;
}

/* decrement usage count in the given instance, then idle process if no more.
 * N.B. we assume hcprocs_lock is already acquired
 */
static void rmUserHCProc (HCProc *hcp)
{
        if (hcp->count > 0)
            hcp->count -= 1;
        else
            fatal ("removing HCProc but count already <= 0: %d\n", hcp->count);

        if (hcp->count == 0) {
            log (1, "pid %d: idling unused %s that was serving %s\n", hcp->pid, hc_name, hcp->IP);
            idleHCProc (hcp);
        }
}

/* save info about a new hamclock process in the given fresh HCProc.
 * N.B. we assume hcprocs_lock is already acquired
 */
static void recordNewHCProc (HCProc *hcp, const char *ip, int pid, int port)
{
        log (2, "pid %d: recording new HPproc %s port %d\n", pid, ip, port);

        strcpy (hcp->IP, ip);
        hcp->pid = pid;
        hcp->port = port;
        hcp->count = 1;
}

/* search hcprocs[] for an existing hamclock servicing IP even if stopped, else NULL.
 * N.B. we assume hcprocs_lock is already acquired
 */
static HCProc *findHCProcForIp (const char *ip)
{
        HCProc *hcp_found = NULL;

        for (int i = 0; !hcp_found && i < max_hamclocks; i++) {
            HCProc *hcp = &hcprocs[i];
            if (HCP_ISASSIGNED(hcp) && strcmp (ip, hcp->IP) == 0)
                hcp_found = hcp;
        }

        // return result, if any
        return (hcp_found);
}


/* search hcprocs[] for an unused entry or reuse if find one idle longer than max_idle.
 * return NULL if full.
 * N.B. we assume hcprocs_lock is already acquired
 */
static HCProc *getNewHCProc ()
{
        HCProc *hcp_found = NULL;
        HCProc *hcp_idle = NULL;
        int max_idledt = max_idle;

        // search for unused entry or oldest one we can reuse
        for (int i = 0; !hcp_found && i < max_hamclocks; i++) {
            HCProc *hcp = &hcprocs[i];
            if (HCP_ISIDLING(hcp)) {
                int idle_dt = HCP_IDLEDT(hcp);
                if (idle_dt > max_idledt) {
                    max_idledt = idle_dt;
                    hcp_idle = hcp;
                }
            } else if (HCP_ISUNUSED(hcp))
                hcp_found = hcp;
        }

        // done if found one unused
        if (hcp_found)
            return (hcp_found);

        // else recycle idle if found
        if (hcp_idle) {
            terminateHCProc (hcp_idle);
            hcp_found = hcp_idle;
        }

        // return result, if any
        return (hcp_found);
}




/* find and return a fresh ephemeral port.
 * N.B. yes I know this is a race condition but OS probably won't recycle the same for a long while.
 */
static int findNewLocalPort()
{
        // create a TCP socket
        int socket = ::socket (AF_INET, SOCK_STREAM, 0);
        if (socket < 0)
            fatal ("ephemeral socket(): %s\n", strerror(errno));

        // bind to port 0 to allow OS to assign an ephemeral port number
        struct sockaddr_in sa;
        memset (&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl (INADDR_ANY);
        sa.sin_port = htons (0);
        if (::bind (socket, (struct sockaddr*)&sa, sizeof(sa)) < 0)
            fatal ("ephemeral bind(socket %d:0): %s\n", socket, strerror(errno));

        // get the result
        socklen_t sa_len = sizeof(sa);
        if (getsockname (socket, (struct sockaddr*)&sa, &sa_len) < 0)
            fatal ("ephemeral getsockname(socket %d): %s\n", socket, strerror(errno));
        int new_port = ntohs (sa.sin_port);

        // finished with socket
        close (socket);

        // return
        return (new_port);
}

/* open a socket to hamclock's live web port
 * return new socket else -1.
 */
static int connectToHamClock (int hclive_port)
{
        // create a TCP socket
        int socket = ::socket (AF_INET, SOCK_STREAM, 0);
        if (socket < 0)
            fatal ("%s socket(): %s\n", hc_name, strerror(errno));

        // connect to local hamclock -w hclive_port
        struct sockaddr_in hc_live;
        memset (&hc_live, 0, sizeof(hc_live));
        hc_live.sin_family      = AF_INET;
        hc_live.sin_port        = htons(hclive_port);
        hc_live.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (::connect (socket, (struct sockaddr *) &hc_live, sizeof(hc_live)) < 0) {
            log (1, "socket %d: connect(%d) failed: %s\n", socket, hclive_port, strerror(errno));
            close (socket);
            return (-1);
        }

        // return new connection socket
        log (1, "socket %d: new connection to -w socket %d\n", socket, hclive_port);
        return (socket);
}

/* read once from from_socket and copy all to_socket.
 * return whether all ok.
 */
static bool copySockets (int from_socket, int to_socket)
{
        // read
        char buf[4096];
        int nr = ::read (from_socket, buf, sizeof(buf));
        if (nr < 0) {
            log (1, "socket %d: read error: %s\n", from_socket, strerror(errno));
            return (false);
        }
        if (nr == 0) {
            log (1, "socket %d: EOF\n", from_socket);
            return (false);
        }

        log (3, "copying %d bytes from socket %d to %d\n", nr, from_socket, to_socket);
        log (3, "socket %d: copying %d bytes to socket %d\n", from_socket, nr, to_socket);

        // write all nr
        int n;
        for (int nw = 0; nw < nr; nw += n) {
            n = ::write (to_socket, buf+nw, nr-nw);
            if (n < 0) {
                log (1, "socket %d: write error: %s\n", to_socket, strerror(errno));
                return (false);
            }
            if (n == 0) {
                log (1, "pid %d: EOF from write socket ??\n", to_socket);
                return (false);
            }
        }

        // ok!
        return (true);
}

/* shuttle all data between the browser and hamclock sockets until either reports error or EOF.
 * N.B. this is the only time a thread unlocks hcprocs_lock
 */
static void shuttleSockets (int sock1, int sock2)
{
        pthread_mutex_unlock (&hcprocs_lock);

        int max_socket = sock2 > sock1 ? sock2 : sock1;
        for (;;) {
            fd_set rfdset;
            FD_ZERO (&rfdset);
            FD_SET (sock1, &rfdset);
            FD_SET (sock2, &rfdset);
            if (::select (max_socket+1, &rfdset, NULL, NULL, NULL) < 0) {
                log (0, "select(): %s\n", strerror(errno));
                break;
            }
            if (FD_ISSET (sock1, &rfdset) && !copySockets (sock1, sock2))
                break;
            if (FD_ISSET (sock2, &rfdset) && !copySockets (sock2, sock1))
                break;
        }

        pthread_mutex_lock (&hcprocs_lock);
}

/* politely close the given socket
 */
static void closeSocket (int s)
{
        log (2, "socket %d: closing\n", s);
        shutdown (s, SHUT_RDWR);
        close (s);
}

/* send a message to the given browser that we are too busy to handle another new hamclock
 * N.B. client_socket is closed on return.
 */
static void sendTooBusy (int client_socket)
{
        // stdout is just too nice to ignore, but that means we must close here.
        FILE *client_fp = fdopen (client_socket, "w");

        // send proper header
        fprintf (client_fp, "HTTP/1.0 200 OK\r\n");
        fprintf (client_fp, "Content-Type: text/html; charset=us-ascii\r\n");
        fprintf (client_fp, "Connection: close\r\n\r\n");

        // send page
        fprintf (client_fp, 
            R"_too_busy_(
                <!DOCTYPE html>
                <html>
                <head>
                    <title>
                        hcwebproxy too busy
                    </title>
                </head>
                <body background="#CCCCCC">
                <center>
                <h3>
                You have reached the hcwebproxy limit of %d HamClocks.
                <p>
                Consider increasing the maximum with <i>hcwebproxy -m</i>.
                </h3>
                </center>
                </body>
                </html>
            )_too_busy_", max_hamclocks);

        // flush before shutdown
        fflush (client_fp);

        // close properly and reclaim FILE
        closeSocket (client_socket);
        fclose (client_fp);
}

/* fill ip_str with the remote IP of the given socket.
 *
 */
static void getRemoteIP (int socket, char ip_str[INET_ADDRSTRLEN])
{
        struct sockaddr_in sa;
        socklen_t len = sizeof(sa);

        getpeername (socket, (struct sockaddr *)&sa, &len);
        struct in_addr ipAddr = sa.sin_addr;

        if (inet_ntop(AF_INET, &ipAddr, ip_str, INET_ADDRSTRLEN) == NULL) {
            log (0, "socket %d: inet_ntop error: %s\n", socket, strerror(errno));
            strcpy (ip_str, "Anon");
        }
}

/* pthread to shepherd a hamclock process in service of the given browser socket. reuse an existing process
 * if already running in behalf of the socket's remote IP, else start a new process. then either way
 * shuttle all traffic between the client and hamclock's -w socket until either gives error or EOF.
 * N.B. acquires hcprocs_lock except while connecting client and hamclock sockets.
 */
static void *HamClockThread (void *arg)
{
        // arg is malloced pointer to client_socket, free after extraction
        int client_socket = *(int *)arg;
        free (arg);

        // get browser's remote IP
        char ip_str[INET_ADDRSTRLEN];
        getRemoteIP (client_socket, ip_str);

        log (1, "socket %d: new pthread for browser from %s\n", client_socket, ip_str);

        // enforce single thread at a time access to hcprocs[] except while shuttling socket traffic.
        pthread_mutex_lock (&hcprocs_lock);

        // search for an existing hamclock process already handling this ip
        HCProc *hcp = findHCProcForIp (ip_str);

        // if none then we need a new hamclock instance
        if (!hcp) {

            // get fresh HCProc unless too many already
            hcp = getNewHCProc();
            if (!hcp) {
                log (1, "reached %d maximum active %s\n", max_hamclocks, hc_name);
                sendTooBusy (client_socket);
                pthread_mutex_unlock (&hcprocs_lock);
                return (NULL);
            }

            // find an unused local port for hamclock -w
            int hclive_port = findNewLocalPort();
            char hcport_str[10];
            snprintf (hcport_str, sizeof(hcport_str), "%d", hclive_port);

            // create a working dir name for hamclock -d using the client's IP to set context
            char ddir[INET_ADDRSTRLEN+100];
            snprintf (ddir, sizeof(ddir), "%s/%s", hc_basedir, ip_str);

            // assign the next RESTful API port if -a
            char apiport_str[20];
            if (api_port > 0) {
                log (1, "socket %d: %s using API port %d\n", client_socket, ip_str, api_port);
                snprintf (apiport_str, sizeof(apiport_str), "%d", api_port++);
            } else
                strcpy (apiport_str, "-1");

            // no zombies
            signal (SIGCHLD, SIG_IGN);

            // fork to exec hamclock
            int pid_hc = fork();
            if (pid_hc < 0)
                fatal ("fork(%s): %s\n", hc_name, strerror(errno));
            if (pid_hc == 0) {

                // new process: build hamclock args
                char *av[8+hc_argc];
                av[0] = (char*)hc_name;
                av[1] = (char*)"-w";
                av[2] = hcport_str;
                av[3] = (char*)"-e";
                av[4] = apiport_str;
                av[5] = (char*)"-d";
                av[6] = ddir;
                for (int i = 0; i < hc_argc; i++)
                    av[7+i] = hc_argv[i];
                av[7+hc_argc] = NULL;

                // close all fd except 0 1 2
                for (int i = 3; i < 100; i++)
                    close (i);

                // normal wait(2)'ability, e.g. important if it uses popen(3)
                signal (SIGCHLD, SIG_DFL);

                log (1, "pid %d: new %s args:\n", getpid(), hc_name);
                for (char **arg = av; *arg; arg++)
                    log (1, "   %s\n", *arg);

                // go
                if (execvp (hc_name, av) < 0)
                    fatal ("execvp(%s): %s\n", hc_name, strerror(errno));
                fatal ("failed to run %s\n", hc_name);

                // end of child process
            }

            // parent process continues on

            // record new instance
            log (1, "pid %d: forked new %s on port %d for %s\n", pid_hc, hc_name, hclive_port, ip_str);
            recordNewHCProc (hcp, ip_str, pid_hc, hclive_port);

        } else {

            // increment count of browser connections using this hamclock, resuming if none currently
            addUserHCProc (hcp);
        }

        // wait just so long for hamclock live web socket connection to succeed
        log (1, "pid %d: waiting for %s serving %s\n", hcp->pid, hc_name, hcp->IP);
        int hclive_socket = -1;
        for (int i = 0; hclive_socket < 0 && i < 10; i++) {
            if (hclive_socket < 0)
                usleep (1000000);
            hclive_socket = connectToHamClock (hcp->port);
        }

        // if ok connect browser and hamclock's live web sockets until either closes
        if (hclive_socket > 0) {
            log (1, "socket %d: from browser %s connecting with %s pid %d socket %d\n",
                                                client_socket, hcp->IP, hc_name, hcp->pid, hclive_socket);

            // shuttle traffic until either EOF.
            // N.B. unlocks hcprocs_lock while running then locks again before returning.
            shuttleSockets (client_socket, hclive_socket);

            log (1, "socket %d: from browser %s finished with %s pid %d socket %d\n",
                                                client_socket, hcp->IP, hc_name, hcp->pid, hclive_socket);

            // record the connection is finished, sleep if this was its last.
            rmUserHCProc(hcp);

        } else {

            log (0, "pid %d: connection failed to %s\n", hcp->pid, hc_name);
            terminateHCProc (hcp);
        }

        // finished with sockets
        if (HCP_ISASSIGNED(hcp))
            log (1, "socket %d: closing from %s for %s pid %d on socket %d\n",
                            client_socket, hcp->IP, hc_name, hcp->pid, hclive_socket);
        else
            log (0, "socket %d: cleaning up rogue client socket and %s socket %d\n",
                            client_socket, hc_name, hclive_socket);
        closeSocket (hclive_socket);
        closeSocket (client_socket);

        // this thread is finished
        pthread_mutex_unlock (&hcprocs_lock);
        return (NULL);
}

/* start a new thread to handle the given browser socket connection.
 */
static void startBrowserThread (int client_socket)
{
        // store client_socket in a separate malloced location for each thread, thread will free
        int *thread_arg = (int *) malloc (sizeof(int));
        *thread_arg = client_socket;

        // start a detached thread
        pthread_t pid;
        pthread_attr_t pattr;
        (void) pthread_attr_init (&pattr);
        (void) pthread_attr_setdetachstate (&pattr, PTHREAD_CREATE_DETACHED);
        int e = pthread_create (&pid, &pattr, HamClockThread, thread_arg);
        if (e)
            fatal ("pthread_create(): %s\n", strerror(e));
}

/* create a server listening to the given port on localhost.
 */
static int createServerSocket (int port)
{
        struct sockaddr_in serv_socket;
        int socket;

        // create socket endpoint
        if ((socket = ::socket (AF_INET, SOCK_STREAM, 0)) < 0)
            fatal ("server socket(): %s\n", strerror(errno));

        // bind to given port for any IP address
        memset (&serv_socket, 0, sizeof(serv_socket));
        serv_socket.sin_family = AF_INET;
        serv_socket.sin_addr.s_addr = htonl (INADDR_ANY);
        serv_socket.sin_port = htons ((unsigned short)port);

        // allow immediate restarting
        int reuse = 1;
        if (::setsockopt(socket,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) < 0)
            fatal ("setsockopt(socket %d, SO_REUSEADDR): %s\n", socket, strerror(errno));

        if (::bind(socket,(struct sockaddr*)&serv_socket,sizeof(serv_socket)) < 0)
            fatal ("server bind(socket %d:%d): %s\n", socket, port, strerror(errno));

        // willing to accept connections with a backlog of 5 pending
        if (::listen (socket, 5) < 0)
            fatal ("listen(socket %d): %\ns", socket, strerror(errno));

        log (1, "socket %d: public server now running on port %d\n", socket, port);

        // ok
        return (socket);
}

/* create the public server and listen forever for browser connections on the given port.
 * start a new pthread to handle each connection.
 */
static void listenForHamClockBrowsers (int port)
{
        // create public server
        int server = createServerSocket(port);

        // handle browser connections forever
        for (;;) {

            // listen for a new connection
            struct sockaddr_in cli_socket;
            socklen_t cli_len = sizeof(cli_socket);
            log (2, "accepting new connections...\n");
            int client_socket = ::accept (server, (struct sockaddr *)&cli_socket, &cli_len);
            if (client_socket < 0)
                fatal ("accept(%d): %s\n", server, strerror(errno));
            log (2, "socket %d: new browser connection\n", client_socket);

            // start new thread for this browser connection
            startBrowserThread (client_socket);
        }
}

/* look through PATH and exit if we can not find the name_candidate program
 */
static void findHamClock (const char *name_candidate)
{
        // first check straight way
        if (access (name_candidate, X_OK) == 0) {
            log (1, "found %s\n", name_candidate);
            return;     // simple as that!
        }

        // get copy of PATH so we can check each component. N.B. be sure to free
        const char *PATH = getenv ("PATH");
        if (!PATH)
            fatal ("No PATH?!\n");
        char *path_copy = strdup (PATH);

        // search each dir for name_candidate
        bool found_hc = false;
        for (char *next_path = path_copy; !found_hc && next_path; ) {

            char *colon = strchr (next_path, ':');
            if (colon)
                *colon = '\0';
            DIR *dirp = opendir (next_path);

            if (dirp) {
                for (struct dirent *dp = readdir (dirp); !found_hc && dp; dp = readdir (dirp)) {
                    if (strcmp (dp->d_name, name_candidate) == 0) {
                        found_hc = true;
                        log (1, "found %s in %s\n", name_candidate, next_path);
                    }
                }
                (void)closedir (dirp);
            } else {
                log (1, "%s: %s\n", next_path, strerror(errno));
            }

            next_path = colon ? colon+1 : NULL;
        }

        // finished with copy
        free (path_copy);

        // bale unless found
        if (!found_hc)
            fatal ("%s: not found in PATH\n", name_candidate);
}

/* called on most termination signals. try to clean up.
 */
static void onSig (int signo)
{
        // ack and gone
        fatal ("Goodbye from signal %d\n", signo);
}

/* connect onSig to several interesting signals
 */
static void connectOnSig()
{
        struct sigaction sa;
        memset (&sa, 0, sizeof(sa));
        sa.sa_handler = onSig;
        sigaction (SIGKILL, &sa, NULL);
        sigaction (SIGINT, &sa, NULL);
        sigaction (SIGTERM, &sa, NULL);
        sigaction (SIGHUP, &sa, NULL);
}


/* print usage and optional message to stderr then exit(1)
 */
static void usage (const char *me, const char *msg)
{
        if (msg)
            fprintf (stderr, "%s\n", msg);

        fprintf (stderr, "Purpose: proxy mutiple instances of hamclock from one public address\n");
        fprintf (stderr, "Version %g Usage: %s [options]\n", VERSION, me);
        fprintf (stderr, "Options:\n");
        fprintf (stderr, " --   : all subsequent args are passed directly to hamclock\n");
        fprintf (stderr,
                " -a p : assign hamclock RESTful API ports sequentially starting with p; default is off\n");
        fprintf (stderr,
                " -d d : create all temporary HamClock working directories within d; default %s\n", hc_basedir);
        fprintf (stderr,
                " -h n : name of HamClock program to run; default %s\n", DEF_HCNAME);
        fprintf (stderr,
                " -i h : set hamclock idle time to be eligible for reuse to h hours; default %g\n",
                DEF_MAXAGE/3600.0);
        fprintf (stderr,
                " -m n : set maximum number of managed hamclock processes to n; default %d\n", DEF_MAXHC);
        fprintf (stderr,
                " -p p : set public port on which to listen for browsers to p; default %d\n", DEF_PORT);
        fprintf (stderr, " -v   : verbose, cummulative\n");

        exit (1);
}

/* set default hc_basedir to a malloced string
 */
static void setBaseDir(void)
{
        char buf[2000];
        char *cwd = getcwd(buf, sizeof(buf));
        if (!cwd)
            fatal ("getcwd(): %s\n", strerror(errno));
        strcat (cwd, "/hcwebproxy-hamclocks");
        hc_basedir = strdup(cwd);
}

/* start here
 */
int main (int ac, char *av[])
{
        // our basename
        char *slash = strrchr (av[0], '/');
        char *me = slash ? slash+1 : av[0];

        // set default hc_basedir before first call to usage() then free() if -d
        setBaseDir();

        // crack args
        int web_port = DEF_PORT;
        while (--ac && **++av == '-') {
            char *s = *av;
            while (*++s) {
                switch (*s) {
                case '-':
                    if (ac < 2)
                        usage (me, "-- requires at least one HamClock arg\n");
                    hc_argv = ++av;
                    hc_argc = --ac;
                    ac = 1;   // we skip all remainging args
                    break;
                case 'a':       
                    if (ac < 2)
                        usage (me, "-a requires initial API port number\n");
                    api_port = atoi(*++av);
                    if (api_port < 1000 || api_port > 65535)
                        usage (me, "-a port must be between 1000 and 65535\n");
                    ac -= 1;
                    break;
                case 'd':       
                    if (ac < 2)
                        usage (me, "-d requires directory\n");
                    free (hc_basedir);
                    hc_basedir = *++av;
                    ac -= 1;
                    break;
                case 'h':       
                    if (ac < 2)
                        usage (me, "-h requires name\n");
                    hc_name = *++av;
                    ac -= 1;
                    break;
                case 'i':       
                    if (ac < 2)
                        usage (me, "-i requires hours\n");
                    max_idle = atof(*++av)*3600;
                    ac -= 1;
                    break;
                case 'm':       
                    if (ac < 2)
                        usage (me, "-m requires max number of hamclocks\n");
                    max_hamclocks = atoi(*++av);
                    ac -= 1;
                    break;
                case 'p':       
                    if (ac < 2)
                        usage (me, "-p requires web server port\n");
                    web_port = atoi (*++av);
                    if (web_port > 65535)
                        usage (me, "-p port must be < 65535\n");
                    ac -= 1;
                    break;
                case 'v':       
                    verbose++;
                    break;
                }
            }
        }
        if (ac > 0)
            usage (me, NULL);

        // prepare hcprocs[]
        hcprocs = (HCProc *) calloc (max_hamclocks, sizeof(HCProc));

        // make sure we can find hc_name in PATH
        findHamClock (hc_name);

        // handle all write errors inline
        signal (SIGPIPE, SIG_IGN);

        // immediate logging
        setbuf (stdout, NULL);

        // connect signal handlers
        connectOnSig();

        // listen forever for new connections
        listenForHamClockBrowsers (web_port);

        // lint
        return (0);
}
