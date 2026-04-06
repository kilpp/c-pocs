/*
 * epoll event loop PoC
 *
 * A single-threaded TCP echo server that handles multiple clients
 * concurrently using epoll(7) with edge-triggered (EPOLLET) mode.
 *
 * Edge-triggered: epoll notifies only when new data arrives (transition).
 * We must drain the socket fully (loop until EAGAIN) each time.
 *
 * Usage: ./server [port]   (default port 8080)
 *        Then: nc localhost 8080
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#define DEFAULT_PORT  8080
#define MAX_EVENTS    64
#define BUF_SIZE      1024

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, SOMAXCONN) == -1) {
        perror("listen"); close(fd); return -1;
    }
    return fd;
}

/* Drain a client fd: read until EAGAIN, echo everything back. */
static void handle_client(int fd)
{
    char buf[BUF_SIZE];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        /* Echo back. A real server would buffer partial writes. */
        ssize_t sent = 0;
        while (sent < n) {
            ssize_t w = write(fd, buf + sent, n - sent);
            if (w == -1) {
                if (errno == EAGAIN) continue; /* out-buf full, retry */
                perror("write");
                close(fd);
                return;
            }
            sent += w;
        }
    }

    if (n == 0) {
        /* Client closed the connection. */
        printf("client fd=%d disconnected\n", fd);
        close(fd);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("read");
        close(fd);
    }
    /* EAGAIN/EWOULDBLOCK → we've drained the buffer, wait for next event */
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    int listen_fd = make_listen_socket(port);
    if (listen_fd == -1) return 1;
    if (set_nonblocking(listen_fd) == -1) { perror("fcntl"); return 1; }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) { perror("epoll_create1"); return 1; }

    /* Watch the listen socket for incoming connections. */
    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLET,
        .data.fd = listen_fd,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("epoll_ctl"); return 1;
    }

    printf("epoll echo server listening on port %d\n", port);

    struct epoll_event events[MAX_EVENTS];

    for (;;) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1 /* block forever */);
        if (n == -1) {
            if (errno == EINTR) continue; /* interrupted by signal, retry */
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                /* Accept all pending connections (ET: drain fully). */
                for (;;) {
                    struct sockaddr_in caddr;
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept(listen_fd,
                                     (struct sockaddr *)&caddr, &clen);
                    if (cfd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break; /* no more pending */
                        perror("accept");
                        break;
                    }

                    set_nonblocking(cfd);

                    struct epoll_event cev = {
                        .events  = EPOLLIN | EPOLLET | EPOLLRDHUP,
                        .data.fd = cfd,
                    };
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) == -1) {
                        perror("epoll_ctl add client");
                        close(cfd);
                        continue;
                    }
                    printf("client fd=%d connected\n", cfd);
                }
            } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                /* Peer closed or error — clean up. */
                printf("client fd=%d hangup/error\n", fd);
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
            } else if (events[i].events & EPOLLIN) {
                handle_client(fd);
                if (fcntl(fd, F_GETFD) == -1) {
                    /* handle_client closed fd on error */
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                }
            }
        }
    }

    close(listen_fd);
    close(epfd);
    return 0;
}
