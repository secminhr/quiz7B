/* System parameters */
const int PORT = 9000;
#define BACKLOG 1024
#define N_THREADS 24 * sysconf(_SC_NPROCESSORS_ONLN)
#define MAXMSG 1024

/* File parameters */
const char *DOCUMENT_ROOT;
#define MAXPATH 1024

#include <pthread.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

typedef int status_t;
enum {
    STATUS_OK = 200,
    STATUS_BAD_REQUEST = 400,
    STATUS_FORBIDDEN = 403,
    STATUS_NOT_FOUND = 404,
    STATUS_REQUEST_TIMEOUT = 408,
    STATUS_REQUEST_TOO_LARGE = 413,
    STATUS_SERVER_ERROR = 500,
};

typedef enum { GET, HEAD } http_method_t;

typedef enum {
    APPLICATION,
    AUDIO,
    IMAGE,
    MESSAGE,
    MULTIPART,
    TEXT,
    VIDEO
} content_type_t;

typedef struct {
    http_method_t method;
    char path[MAXPATH];
    content_type_t type;
    int protocol_version;
} http_request_t;

#include <string.h>

/* A collection of useful functions for parsing HTTP messages.
 * See function parse_request for high-level control flow and work upward.
 */

/* TRY_CATCH and TRY_CATCH_S are private macros that "throw" appropriate
 * status codes whenever a parsing method encounters an error. By wrapping
 * every parsing method call in a TRY_CATCH, errors may be piped up to the
 * original parse_request call. The second TRY_CATCH_S macro is for specially
 * translating the error outputs of the string.h function strsep into the
 * BAD_REQUEST (400) status code.
 */
#define TRY_CATCH(STATEMENT)      \
    do {                          \
        status_t s = (STATEMENT); \
        if (s != STATUS_OK)       \
            return s;             \
    } while (0)

#define TRY_CATCH_S(STATEMENT)         \
    do {                               \
        if (!(STATEMENT))              \
            return STATUS_BAD_REQUEST; \
    } while (0)

static const char *type_to_str(const content_type_t type)
{
    switch (type) {
    case APPLICATION:
        return "application";
    case AUDIO:
        return "audio";
    case IMAGE:
        return "image";
    case MESSAGE:
        return "message";
    case MULTIPART:
        return "multipart";
    case TEXT:
        return "text";
    case VIDEO:
        return "video";
    default:
        return NULL;
    }
}

static const char *status_to_str(status_t status)
{
    switch (status) {
    case STATUS_OK:
        return "OK";
    case STATUS_BAD_REQUEST:
        return "Bad Request";
    case STATUS_FORBIDDEN:
        return "Forbidden";
    case STATUS_NOT_FOUND:
        return "Not Found";
    case STATUS_REQUEST_TIMEOUT:
        return "Request Timeout";
    case STATUS_REQUEST_TOO_LARGE:
        return "Request Entity Too Large";
    case STATUS_SERVER_ERROR:
    default:
        return "Internal Server Error";
    }
}

/* Private utility method that acts like strsep(s," \t"), but also advances
 * s so that it skips any additional whitespace.
 */
static char *strsep_whitespace(char **s)
{
    char *ret = strsep(s, " \t");
    while (*s && (**s == ' ' || **s == '\t'))
        (*s)++; /* extra whitespace */
    return ret;
}

/* Same as strsep_whitespace, but for newlines. */
static char *strsep_newline(char **s)
{
    char *ret;
    char *r = strchr(*s, '\r');
    char *n = strchr(*s, '\n');

    if (!r || n < r)
        ret = strsep(s, "\n");
    else {
        ret = strsep(s, "\r");
        (*s)++; /* advance past the trailing \n */
    }
    return ret;
}

static status_t parse_method(char *token, http_request_t *request)
{
    if (strcmp(token, "GET") == 0)
        request->method = GET;
    else if (strcmp(token, "HEAD") == 0)
        request->method = HEAD;
    else
        return STATUS_BAD_REQUEST;
    return STATUS_OK;
}

static status_t parse_path(char *token, http_request_t *request)
{
    if (strcmp(token, "/") == 0 || strcmp(token, "/index.html") == 0) {
        snprintf(request->path, MAXPATH, "%s/index.html", DOCUMENT_ROOT);
        request->type = TEXT;
    } else /* FIXME: handle images files and other resources */
        return STATUS_NOT_FOUND;
    return STATUS_OK;
}

static status_t parse_protocol_version(char *token, http_request_t *request)
{
    if (!strcmp(token, "HTTP/1.0"))
        request->protocol_version = 0;
    else if (!strcmp(token, "HTTP/1.1"))
        request->protocol_version = 1;
    else
        return STATUS_BAD_REQUEST;
    return STATUS_OK;
}

static status_t parse_initial_line(char *line, http_request_t *request)
{
    char *token;
    TRY_CATCH_S(token = strsep_whitespace(&line));
    TRY_CATCH(parse_method(token, request));
    TRY_CATCH_S(token = strsep_whitespace(&line));
    TRY_CATCH(parse_path(token, request));
    TRY_CATCH_S(token = strsep_whitespace(&line));
    TRY_CATCH(parse_protocol_version(token, request));

    return STATUS_OK;
}

/* FIXME: Currently ignores any request headers */
static status_t parse_header(char *line, http_request_t *request)
{
    (void) line, (void) request;
    return STATUS_OK;
}

static status_t parse_request(char *msg, http_request_t *request)
{
    char *line;
    TRY_CATCH_S(line = strsep_newline(&msg));
    TRY_CATCH(parse_initial_line(line, request));
    while ((line = strsep_newline(&msg)) != NULL && *line != '\0')
        TRY_CATCH(parse_header(line, request));

    return STATUS_OK;
}

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Some wrapper functions for listening socket initalization,
 * where we may simply exit if we cannot set up the socket.
 */

static int socket_(int domain, int type, int protocol)
{
    int sockfd;
    if ((sockfd = socket(domain, type, protocol)) < 0) {
        fprintf(stderr, "Socket error!\n");
        exit(1);
    }
    return sockfd;
}

static int bind_(int socket,
                 const struct sockaddr *address,
                 socklen_t address_len)
{
    int ret;
    if ((ret = bind(socket, address, address_len)) < 0) {
        perror("bind_");
        exit(1);
    }
    return ret;
}

static int listen_(int socket, int backlog)
{
    int ret;
    if ((ret = listen(socket, backlog)) < 0) {
        fprintf(stderr, "Listen error!\n");
        exit(1);
    }
    return ret;
}

/* Initialize listening socket */
static int listening_socket()
{
    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(PORT);
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listenfd = socket_(AF_INET, SOCK_STREAM, 0);
    bind_(listenfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr));
    listen_(listenfd, BACKLOG);
    return listenfd;
}

#include <sys/epoll.h>

static void *worker_routine(void *arg)
{
    pthread_detach(pthread_self());

    int connfd, file, len, recv_bytes;
    char msg[MAXMSG], buf[1024];
    status_t status;
    http_request_t *request = malloc(sizeof(http_request_t));
    int epollfd = *((int *)arg);
    struct epoll_event event[1] = {0};
    struct stat st;

    while (1) {
    loopstart:
        memset(&event[0], 0, sizeof(event[0]));
        epoll_wait(epollfd, event, 1, -1);
        memset(msg, 0, MAXMSG);
        recv_bytes = 0;
        connfd = event[0].data.fd;
        /* Loop until full HTTP msg is received */
        while (strstr(strndup(msg, recv_bytes), "\r\n\r\n") == NULL &&
               strstr(strndup(msg, recv_bytes), "\n\n") == NULL &&
               recv_bytes < MAXMSG) {
            if ((len = recv(connfd, msg + recv_bytes, MAXMSG - recv_bytes,
                            0)) <= 0) {
                if (errno == EAGAIN) {
                    abort();
                }
                /* If client has closed, then close and move on */
                if (len == 0) {
                    close(connfd);
                    goto loopstart;
                }
                /* If timeout or error, skip parsing and send appropriate
                 * error message
                 */
                if (errno == EWOULDBLOCK) {
                    abort();
                } else {
                    status = STATUS_SERVER_ERROR;
                    printf("printing recv error: %d\n", errno);
                    switch (errno) {
                    case EAGAIN: printf("EAGAIN or EWOULDBLOCK\n"); break;
                    case EBADF: printf("EBADF\n"); break;
                    case ECONNREFUSED: printf("ECONNREFUSED\n"); break;
                    case EFAULT: printf("EFAULT\n"); break;
                    case EINTR: printf("EINTR\n"); break;
                    case EINVAL: printf("EINVAL\n"); break;
                    case ENOMEM: printf("ENOMEM\n"); break;
                    case ENOTCONN: printf("ENOTCONN\n"); break;
                    case ENOTSOCK: printf("ENOTSOCK\n"); break;
                    default: printf("errno: %d", errno);
                    }
                    perror("recv");
                }
                goto send;
            }
            recv_bytes += len;
        }
        struct epoll_event ev = {
            .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
            .data.fd = connfd
        };
        epoll_ctl(epollfd, EPOLL_CTL_MOD, connfd, &ev);
        /* Parse (complete) message */
        status = parse_request(msg, request);

    send:
        /* Send initial line */
        len = sprintf(msg, "HTTP/1.%d %d %s\r\n", request->protocol_version,
                      status, status_to_str(status));
        send(connfd, msg, len, 0);

        /* Send header lines */
        time_t now;
        time(&now);
        len = strftime(buf, 1024, "Date: %a, %d %b %Y %H:%M:%S GMT\r\n",
                       gmtime(&now));
        send(connfd, buf, len, 0);
        if (status == STATUS_OK && request->method == GET) {
            stat(request->path, &st);
            len = sprintf(msg, "Content-Length: %d\r\n", (int) st.st_size);
            send(connfd, msg, len, 0);
            len = sprintf(msg, "Content-Type: %s\r\n",
                          type_to_str(request->type));
            send(connfd, msg, len, 0);
        }
        send(connfd, "\r\n", 2, 0);

        /* If request was well-formed GET, then send file */
        if (status == STATUS_OK && request->method == GET) {
            if ((file = open(request->path, O_RDONLY)) < 0)
                perror("open");
            while ((len = read(file, msg, MAXMSG)) > 0)
                if (send(connfd, msg, len, 0) < 0)
                    perror("sending file");
            close(file);
        }

        /* If HTTP/1.0 or recv error, close connection. */
        if (request->protocol_version == 0 || status != STATUS_OK)
            close(connfd);
        else {/* Otherwise, keep connection alive and re-enqueue */
            //enqueue(q, connfd);
        }
    }
    return NULL;
}

struct greeter_args {
    int listfd;
    int epollfd;
};

void *greeter_routine(void *arg)
{
    struct greeter_args *ga = (struct greeter_args *) arg;
    int listfd = ga->listfd;
    int epollfd = ga->epollfd;

    struct sockaddr_in clientaddr;
    /* Accept connections, set their timeouts, and enqueue them */
    while (1) {
        socklen_t clientlen = sizeof(clientaddr);
        int connfd =
            accept(listfd, (struct sockaddr *) &clientaddr, &clientlen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        int flag = fcntl(connfd, F_GETFL, 0);
        flag |= O_NONBLOCK;
        fcntl(connfd, F_SETFL, flag);
        
        struct epoll_event event = {
            .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
            .data.fd = connfd
        };
        epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &event);
    }
}

int main()
{
    pthread_t workers[N_THREADS / 2], greeters[N_THREADS / 2];
    //pthread_t workers[1], greeters[N_THREADS / 2];
    /* Get current working directory */
    char cwd[1024];
    const char *RESOURCES = "/resources";
    if (getcwd(cwd, sizeof(cwd) - sizeof(RESOURCES)/*reserve space for resources*/) == NULL)
        perror("getcwd");
    /* Assign document root */
    DOCUMENT_ROOT = strcat(cwd, RESOURCES);

    /* Initialize listening socket */
    int listfd = listening_socket();

    int epollfd = epoll_create(1);

    /* Package arguments for greeter threads */
    struct greeter_args ga = {.listfd = listfd, .epollfd = epollfd};

    /* Spawn greeter threads. */
    for (int i = 0; i < N_THREADS / 2; i++)
        pthread_create(&greeters[i], NULL, greeter_routine, (void *) (&ga));

    /* Spawn worker threads. These will immediately block until signaled by
     * main server thread pushes connections onto the queue and signals.
     */
    for (int i = 0; i < N_THREADS / 2; i++)
    //for (int i = 0; i < 1; i++)
        pthread_create(&workers[i], NULL, worker_routine, (void *) (&epollfd));
    pthread_exit(NULL);
    return 0;
}
