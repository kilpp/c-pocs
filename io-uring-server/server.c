/*
 * io_uring echo server PoC
 *
 * A single-threaded TCP echo server using Linux io_uring (liburing).
 *
 * Key difference from the epoll POC: instead of waiting for fd *readiness*
 * and then issuing syscalls, we submit async I/O *operations* to a
 * Submission Queue (SQ) and harvest results from a Completion Queue (CQ).
 * The kernel drives the I/O; we just react to completions.
 *
 * Flow per connection:
 *   ACCEPT → RECV → SEND → RECV → SEND → ... → close
 *
 * Each in-flight operation carries a `struct conn *` as user_data so the
 * completion handler knows which fd and operation type just finished.
 *
 * Build: see Makefile  (requires liburing-dev / liburing-devel)
 * Usage: ./server [port]   (default port 8080)
 *        Then: nc localhost 8080
 *
 * Requires: Linux >= 5.1, liburing >= 0.7
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <liburing.h>

#define DEFAULT_PORT  8080
#define QUEUE_DEPTH   256   /* max in-flight operations */
#define BUF_SIZE      1024

/* Tag for each in-flight SQE so we know what completed. */
enum op_type { OP_ACCEPT, OP_RECV, OP_SEND };

/*
 * One allocation per in-flight operation.
 * For ACCEPT the same struct is reused after each connection is accepted.
 * For RECV/SEND the struct follows the client until it disconnects.
 */
struct conn {
    int               fd;
    enum op_type      op;
    char              buf[BUF_SIZE];
    /* ACCEPT only: storage for the incoming client address */
    struct sockaddr_in peer_addr;
    socklen_t          peer_len;
};

static struct io_uring ring;

/* ------------------------------------------------------------------
 * SQE builders — each grabs one slot from the submission queue,
 * fills it, and tags it with `c` so the CQE handler can find us.
 * ------------------------------------------------------------------ */

static void add_accept(int listen_fd, struct conn *c)
{
    c->op       = OP_ACCEPT;
    c->fd       = listen_fd;
    c->peer_len = sizeof(c->peer_addr);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_accept(sqe, listen_fd,
                         (struct sockaddr *)&c->peer_addr, &c->peer_len, 0);
    io_uring_sqe_set_data(sqe, c);
}

static void add_recv(struct conn *c)
{
    c->op = OP_RECV;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_recv(sqe, c->fd, c->buf, BUF_SIZE, 0);
    io_uring_sqe_set_data(sqe, c);
}

static void add_send(struct conn *c, int len)
{
    c->op = OP_SEND;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    io_uring_prep_send(sqe, c->fd, c->buf, len, 0);
    io_uring_sqe_set_data(sqe, c);
}

/* ------------------------------------------------------------------ */

static int make_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind"); exit(1);
    }
    if (listen(fd, SOMAXCONN) == -1) {
        perror("listen"); exit(1);
    }
    return fd;
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;

    int listen_fd = make_listen_socket(port);

    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
        perror("io_uring_queue_init");
        return 1;
    }

    /*
     * Prime the pump: submit one ACCEPT so the kernel starts watching
     * for incoming connections immediately.
     */
    struct conn *accept_conn = calloc(1, sizeof(*accept_conn));
    if (!accept_conn) { perror("calloc"); return 1; }
    add_accept(listen_fd, accept_conn);
    io_uring_submit(&ring);

    printf("io_uring echo server listening on port %d\n", port);

    for (;;) {
        /*
         * Submit any pending SQEs and block until at least one CQE
         * is available. This is the *only* syscall in the hot path.
         */
        io_uring_submit_and_wait(&ring, 1);

        struct io_uring_cqe *cqe;
        unsigned head, count = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            count++;
            struct conn *c = io_uring_cqe_get_data(cqe);
            int res = cqe->res;  /* bytes transferred, or -errno on error */

            switch (c->op) {

            case OP_ACCEPT:
                if (res < 0) {
                    fprintf(stderr, "accept: %s\n", strerror(-res));
                    break;
                }
                printf("client fd=%d connected\n", res);

                /* Kick off a RECV for the newly accepted client. */
                struct conn *client = calloc(1, sizeof(*client));
                if (!client) { perror("calloc"); exit(1); }
                client->fd = res;
                add_recv(client);

                /* Re-arm ACCEPT so we're ready for the next connection. */
                add_accept(listen_fd, c);
                break;

            case OP_RECV:
                if (res <= 0) {
                    if (res < 0)
                        fprintf(stderr, "recv fd=%d: %s\n", c->fd, strerror(-res));
                    else
                        printf("client fd=%d disconnected\n", c->fd);
                    close(c->fd);
                    free(c);
                    break;
                }
                /* We have `res` bytes in c->buf — send them back. */
                add_send(c, res);
                break;

            case OP_SEND:
                if (res < 0) {
                    fprintf(stderr, "send fd=%d: %s\n", c->fd, strerror(-res));
                    close(c->fd);
                    free(c);
                    break;
                }
                /*
                 * Echo done. Wait for the next chunk from this client.
                 * We reuse the same conn struct — no alloc/free churn.
                 */
                add_recv(c);
                break;
            }
        }

        /* Advance the CQ head in one shot (cheaper than per-CQE seen()). */
        io_uring_cq_advance(&ring, count);
    }

    io_uring_queue_exit(&ring);
    close(listen_fd);
    return 0;
}
