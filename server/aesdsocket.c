#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>


#define PORT        "9000"
#define DATA_FILE   "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define CHUNK_SIZE  1024

static volatile sig_atomic_t g_exit_requested = 0;
static pthread_mutex_t g_file_mutex = PTHREAD_MUTEX_INITIALIZER;

static void signal_handler(int sig)
{
    (void)sig;
    g_exit_requested = 1;
}


struct client_thread {
    pthread_t                   tid;
    int                         client_fd;
    int                         file_fd;
    char                        ip[INET_ADDRSTRLEN];
    atomic_bool                 done;
    SLIST_ENTRY(client_thread)  entries;
};

SLIST_HEAD(thread_list, client_thread);

/* write() until everything is written (handles short writes / EINTR) */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) 
    {
        ssize_t n = write(fd, buf, len);
        if (n < 0) 
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* send() until everything is sent; a send timeout is retried unless we are shutting down */
static int send_all(int sock, const char *buf, size_t len)
{
    while (len > 0) 
    {
        ssize_t n = send(sock, buf, len, MSG_NOSIGNAL);
        if (n < 0) 
        {
            if (errno == EINTR)
                continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && !g_exit_requested)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Send the first `size` bytes of the data file. pread() takes an explicit offset,
so threads sharing one fd cannot disturb each other's read position. */
static int send_file(int sock, int file_fd, off_t size)
{
    char buf[CHUNK_SIZE];
    off_t off = 0;

    while (off < size)
    {
        size_t want = (size - off) < (off_t)sizeof(buf) ? (size_t)(size - off) : sizeof(buf);
        ssize_t n = pread(file_fd, buf, want, off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        if(send_all(sock, buf, (size_t)n) == -1)
            return -1;
        off += n;
    }
    return 0;
}

/* Append data to a growing heap buffer */
static int buf_append(char **buf, size_t *len, size_t *cap,
                      const char *data, size_t data_len)
{
    if (*len + data_len > *cap) 
    {
        size_t new_cap = *cap ? *cap : CHUNK_SIZE;
        while (new_cap < *len + data_len)
            new_cap *= 2;
        char *tmp = realloc(*buf, new_cap);
        if (tmp == NULL)
            return -1;
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return 0;
}

/* Append one packet under the mutex and report the file size right after it */
static int append_packet(int file_fd, const char *buf, size_t len, off_t *size_out)
{
    struct stat st;
    int rc = 0;

    pthread_mutex_lock(&g_file_mutex);
    if (write_all(file_fd, buf, len) == -1 || fstat(file_fd, &st) == -1)
        rc = -1;
    else
        *size_out = st.st_size;
    pthread_mutex_unlock(&g_file_mutex);
    return rc;
}

/* Receive packets (newline terminated) from one client until it disconnects */
static int handle_client(int client_fd, int file_fd)
{
    char chunk[CHUNK_SIZE];
    char *packet = NULL;
    size_t len = 0, cap = 0;
    int ret = 0;

    while (!g_exit_requested) 
    {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n == 0)
            break;                      /* client closed connection */
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;               /* received timeout: re-check the exit flag */
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            ret = -1;
            break;
        }

        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) 
        {
            if (chunk[i] != '\n')
                continue;
                
            /* complete packet= buffered part + chunk[start..i] */
            if (buf_append(&packet, &len, &cap, chunk + start, i + 1 - start) == -1)
            {
            	syslog(LOG_ERR, "malloc failed, discarding packet");
            	len = 0;
            	start = i + 1;
            	continue;
            }
            start = i + 1;
            
            off_t size = 0;
            if (append_packet(file_fd, packet, len, &size) == -1) 
            {
            	syslog(LOG_ERR, "write to %s failed: %s", DATA_FILE, strerror(errno));
            	ret = -1;
            	goto out;	  
            }
            len = 0;
                
            if (send_file(client_fd, file_fd, size) == -1)
            {
            	syslog(LOG_ERR, "sending file failed: %s", strerror(errno));
            	ret = -1;
            	goto out;
            }  
        }

        /* keep the incomplete tail for the next recv() */
        if (start < (size_t)n) 
        {
            if (buf_append(&packet, &len, &cap, chunk + start, (size_t)n - start) == -1) 
            {
                syslog(LOG_ERR, "malloc failed, discarding partial packet");
                len = 0;
            }
        }
    }

out:
    free(packet);
    return ret;
}

static void *client_thread_fn(void *arg)
{
    struct client_thread *t = arg;

    /* 1s timeout so a blocked recv()/send() notices a shutdown request */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(t->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(t->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    syslog(LOG_INFO, "Accpepted connection from %s", t->ip);
    handle_client(t->client_fd, t->file_fd);
    close(t->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", t->ip);

    atomic_store(&t->done, true);
    return NULL;
}

static void* timestamp_thread(void* arg)
{
    int file_fd = *(int *)arg;
    
    while (!g_exit_requested) 
    {
        for (int i = 0; i < 10 && !g_exit_requested; i++)
            sleep(1);		/* 1s steps to shutdown is not delayed by 10s */

        if (g_exit_requested)
            break;

        time_t now = time(NULL);
        struct tm tm_now;
        char line[96];
        localtime_r(&now, &tm_now);
        size_t n = strftime(line, sizeof(line), "timestamp:%a, %d %b %Y %T %z\n", &tm_now);
        if (n == 0)
            continue;

        pthread_mutex_lock(&g_file_mutex);
        if (write_all(file_fd, line, n) == -1)
            syslog(LOG_ERR, "timestampe write failed: %s", strerror(errno));
        pthread_mutex_unlock(&g_file_mutex);
    }

    return NULL;
}

/* Join finished up threads (or all of them) and free their list entries */
static void reap_threads(struct thread_list *list, bool all)
{
    struct client_thread *t = SLIST_FIRST(list);

    while (t != NULL) 
    {
        struct client_thread *next = SLIST_NEXT(t, entries);
        if ( all || atomic_load(&t->done))
        {
            pthread_join(t->tid, NULL);
            SLIST_REMOVE(list, t, client_thread, entries);
            free(t);
        }
        t = next;
    }
}

static int daemonize(void) 
{
    pid_t pId = fork();
    if ( pId < 0 )
        return -1;

    if ( pId > 0 ) 
        _exit(0);

    /* Child Process - Daemon */
    pid_t sId = setsid();
    if (sId == -1 )
        return -1;

    if (chdir("/") == -1)
        return -1;


    int devnull = open("/dev/null", O_RDWR);
    if (devnull == -1)
        return -1;

    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO)
        close(devnull);

    return 0;
}



int main(int argc, char* argv[])
{
    int ret = 0;
    int server_fd = -1;
    int file_fd = -1;

    int cli_opt;
    int daemon_mode = 0;

    while ( (cli_opt = getopt(argc, argv, "d")) != -1 ) 
    {
        if ( cli_opt == 'd' ) 
        {
            daemon_mode = 1;
        }
        else 
        {
            fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
            return -1;
        }
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    /* Signals: no SA_RESTART so accept()/recv() return EINTR */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) 
    {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    /* Block the signals here; threads created later inherit this mask, so only
     * the main thread, inside ppoll(), ever takes them */
    sigset_t block_mask, orig_mask;
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &block_mask, &orig_mask) != 0)
    {
        syslog(LOG_ERR, "pthread_sigmask failed");
        closelog();
        return -1;
    }

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;     /* bind to 0.0.0.0, not loopback */

    int rc = getaddrinfo(NULL, PORT, &hints, &result);
    if (rc != 0) 
    {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rc));
        closelog();
        return -1;
    }

    server_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (server_fd == -1) 
    {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        freeaddrinfo(result);
        closelog();
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, result->ai_addr, result->ai_addrlen) == -1) 
    {
        syslog(LOG_ERR, "bind: %s", strerror(errno));
        freeaddrinfo(result);
        close(server_fd);
        closelog();
        return -1;
    }
    freeaddrinfo(result);

    if (listen(server_fd, BACKLOG) == -1) 
    {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    /* If daemon mode, fork process */
    /* AFTER BIND and LISTEN to make move to background safe */
    if ( daemon_mode && daemonize() == -1 ) 
    {
        syslog(LOG_ERR, "failed to daemonize");
        close(server_fd);
        closelog();
        return -1;
    }


    /* O_APPEND: every write() lands at the end, regardless of lseek() used for reading */
    file_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (file_fd == -1) 
    {
        syslog(LOG_ERR, "open %s: %s", DATA_FILE, strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    pthread_t timer_tid;
    if (pthread_create(&timer_tid, NULL, timestamp_thread, &file_fd) != 0) 
    {
        syslog(LOG_ERR, "failed to start timestamp thread");
        close(file_fd);
        close(server_fd);
        unlink(DATA_FILE);
        closelog();
        return -1;
    }

    struct thread_list threads = SLIST_HEAD_INITIALIZER(threads);
    struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
    const struct timespec timeout = { .tv_sec = 1, .tv_nsec = 0 };

    while (!g_exit_requested) 
    {
        /* ppoll atomically unblocks SIGINT/SIGTERM while waiting: no lost-signal race */
        int pr = ppoll(&pfd, 1, &timeout, &orig_mask);
        reap_threads(&threads, false);

        if (pr == 0)
            continue;
        if (pr == -1) 
        {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "ppoll: %s", strerror(errno));
            ret = -1;
            break;
        }

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd == -1) 
        {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            ret = -1;
            break;
        }

        struct client_thread *t = calloc(1, sizeof(*t));
        if (t == NULL) 
        {
            syslog(LOG_ERR, "calloc failed, dropping connection");
            close(client_fd);
            continue;
        }
        t->client_fd = client_fd;
        t->file_fd = file_fd;
        inet_ntop(AF_INET, &peer.sin_addr, t->ip, sizeof(t->ip));

        if (pthread_create(&t->tid, NULL, client_thread_fn, t) != 0) 
        {
            syslog(LOG_ERR, "pthread_create failed");
            close(client_fd);
            free(t);
            continue;
        }
        SLIST_INSERT_HEAD(&threads, t, entries);
    }

    if (g_exit_requested)
        syslog(LOG_INFO, "Caught signal, exiting");
    g_exit_requested = 1;               /* also covers the error-exit path */

    reap_threads(&threads, true);       /* join every client thread */
    pthread_join(timer_tid, NULL);

    close(file_fd);
    close(server_fd);
    unlink(DATA_FILE);
    closelog();

    return ret;
}
