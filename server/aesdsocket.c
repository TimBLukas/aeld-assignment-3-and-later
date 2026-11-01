#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <stdbool.h>

#define PORT        "9000"
#define DATA_FILE   "/var/tmp/aesdsocketdata"
#define BACKLOG     10
#define CHUNK_SIZE  1024

static volatile sig_atomic_t g_exit_requested = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_exit_requested = 1;
}

/* write() until everything is written (handles short writes / EINTR) */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* send() until everything is sent */
static int send_all(int sock, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(sock, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Stream the whole data file to the client in small chunks, so memory
 * use does not depend on the file size. */
static int send_file(int sock, int file_fd)
{
    char buf[CHUNK_SIZE];

    if (lseek(file_fd, 0, SEEK_SET) == -1)
        return -1;

    for (;;) {
        ssize_t n = read(file_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        if (send_all(sock, buf, (size_t)n) == -1)
            return -1;
    }
    return 0;
}

/* Append data to a growing heap buffer */
static int buf_append(char **buf, size_t *len, size_t *cap,
                      const char *data, size_t data_len)
{
    if (*len + data_len > *cap) {
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

/* Receive packets (newline terminated) from one client until it disconnects */
static int handle_client(int client_fd, int file_fd)
{
    char chunk[CHUNK_SIZE];
    char *packet = NULL;
    size_t len = 0, cap = 0;
    int ret = 0;

    while (!g_exit_requested) {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n == 0)
            break;                      /* client closed connection */
        if (n < 0) {
            if (errno == EINTR)
                continue;               /* loop condition checks the signal flag */
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            ret = -1;
            break;
        }

        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (chunk[i] != '\n')
                continue;

            /* complete packet = buffered part + chunk[start..i] */
            if (buf_append(&packet, &len, &cap, chunk + start, i + 1 - start) == -1) {
                syslog(LOG_ERR, "malloc failed, discarding packet");
                len = 0;
                start = i + 1;
                continue;
            }
            start = i + 1;

            if (write_all(file_fd, packet, len) == -1) {
                syslog(LOG_ERR, "write to %s failed: %s", DATA_FILE, strerror(errno));
                ret = -1;
                goto out;
            }
            len = 0;

            if (send_file(client_fd, file_fd) == -1) {
                syslog(LOG_ERR, "sending file failed: %s", strerror(errno));
                ret = -1;
                goto out;
            }
        }

        /* keep the incomplete tail for the next recv() */
        if (start < (size_t)n) {
            if (buf_append(&packet, &len, &cap, chunk + start, (size_t)n - start) == -1) {
                syslog(LOG_ERR, "malloc failed, discarding partial packet");
                len = 0;
            }
        }
    }

out:
    free(packet);
    return ret;
}

static int daemonize(void) 
{
    pid_t pId = fork();
    if ( pId < 0 )
        return -1;

    if ( pId > 0 ) 
        exit(0);

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

    while ( (cli_opt = getopt(argc, argv, "d")) != -1 ) {
        if ( cli_opt == 'd' ) {
            daemon_mode = 1;
        } else {
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
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;     /* bind to 0.0.0.0, not loopback */

    int rc = getaddrinfo(NULL, PORT, &hints, &result);
    if (rc != 0) {
        syslog(LOG_ERR, "getaddrinfo: %s", gai_strerror(rc));
        closelog();
        return -1;
    }

    server_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (server_fd == -1) {
        syslog(LOG_ERR, "socket: %s", strerror(errno));
        freeaddrinfo(result);
        closelog();
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, result->ai_addr, result->ai_addrlen) == -1) {
        syslog(LOG_ERR, "bind: %s", strerror(errno));
        freeaddrinfo(result);
        close(server_fd);
        closelog();
        return -1;
    }
    freeaddrinfo(result);

    if (listen(server_fd, BACKLOG) == -1) {
        syslog(LOG_ERR, "listen: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    /* If daemon mode, fork process */
    /* AFTER BIND and LISTEN to make move to background safe */
    if ( daemon_mode && daemonize() == -1 ) {
        syslog(LOG_ERR, "failed to daemonize");
        close(server_fd);
        closelog();
        return -1;
    }



    /* O_APPEND: every write() lands at the end, regardless of lseek() used for reading */
    file_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (file_fd == -1) {
        syslog(LOG_ERR, "open %s: %s", DATA_FILE, strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }

    while (!g_exit_requested) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int client_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd == -1) {
            if (errno == EINTR)
                continue;
            syslog(LOG_ERR, "accept: %s", strerror(errno));
            ret = -1;
            break;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        syslog(LOG_INFO, "Accepted connection from %s", ip);

        if (handle_client(client_fd, file_fd) == -1)
            ret = -1;

        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", ip);
    }

    if (g_exit_requested)
        syslog(LOG_INFO, "Caught signal, exiting");

    close(file_fd);
    close(server_fd);
    unlink(DATA_FILE);
    closelog();

    return ret;
}
