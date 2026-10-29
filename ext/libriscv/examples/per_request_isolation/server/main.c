/**
 * per_request_isolation: epoll HTTP server using libriscv fast-fork.
 *
 * One "master" RISC-V VM is created and initialized at startup.
 * For each incoming HTTP connection a lightweight Copy-on-Write fork
 * is spun up, the request is dispatched to the guest's handle()
 * function, and the fork is destroyed before the response is sent.
 *
 * The guest never gets a raw file descriptor — all I/O goes through
 * custom syscall handlers, so the sandboxed code cannot reach the
 * host network stack directly.
 *
 * Build requirements:  Linux, epoll(7), libriscv C API
 * Guest requirements:  RISC-V RV64, native heap syscalls at base 470
 */

#include <libriscv.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ── configuration ──────────────────────────────────────────────── */

#define LISTEN_PORT       8080
#define BACKLOG           128
#define MAX_EVENTS        64
#define RECV_BUFSIZE      8192   /* max bytes read from a connection  */
#define SEND_BUFSIZE      65536  /* max response size from the guest  */

/* Native heap syscalls match the base expected by native_libc.h */
#define NATIVE_SYSCALL_BASE  470

/* Custom syscalls installed below */
#define SYSCALL_GET_REQUEST   500
#define SYSCALL_SEND_RESPONSE 501

/* ── per-connection context ─────────────────────────────────────── */

typedef struct {
	int      fd;
	char     req_buf[RECV_BUFSIZE];
	unsigned req_len;
	char    *resp_buf;     /* malloc'd response from the guest */
	unsigned resp_len;
} Connection;

/* Thread-local (single-threaded here) active connection pointer.
   The syscall handlers need it, and the C API doesn't have a
   per-vmcall context slot, so we use a global for this example. */
static Connection *g_active_conn = NULL;

/* ── libriscv callbacks ─────────────────────────────────────────── */

static void error_cb(void *opaque, int type, const char *msg, long data)
{
	(void)opaque; (void)type; (void)data;
	fprintf(stderr, "[libriscv] error: %s\n", msg);
}

static void stdout_cb(void *opaque, const char *msg, unsigned len)
{
	(void)opaque;
	printf("[guest] %.*s", (int)len, msg);
}

/* ── custom syscall: SYSCALL_GET_REQUEST ────────────────────────── */
/* Guest calls: unsigned get_request(char *buf, unsigned maxlen)
   Returns number of bytes written to buf. */
static void syscall_get_request(RISCVMachine *m)
{
	RISCVRegisters *regs = libriscv_get_registers(m);
	uint64_t  guest_buf = LIBRISCV_ARG_REGISTER(regs, 0);
	unsigned  maxlen    = (unsigned)LIBRISCV_ARG_REGISTER(regs, 1);

	if (!g_active_conn || g_active_conn->req_len == 0) {
		libriscv_set_result_register(m, 0);
		return;
	}

	unsigned copy_len = g_active_conn->req_len;
	if (copy_len > maxlen)
		copy_len = maxlen;

	libriscv_copy_to_guest(m, guest_buf, g_active_conn->req_buf, copy_len);
	libriscv_set_result_register(m, (int64_t)copy_len);
}

/* ── custom syscall: SYSCALL_SEND_RESPONSE ──────────────────────── */
/* Guest calls: void send_response(const char *buf, unsigned len) */
static void syscall_send_response(RISCVMachine *m)
{
	RISCVRegisters *regs = libriscv_get_registers(m);
	uint64_t guest_buf = LIBRISCV_ARG_REGISTER(regs, 0);
	unsigned len       = (unsigned)LIBRISCV_ARG_REGISTER(regs, 1);

	if (!g_active_conn || len == 0 || len > SEND_BUFSIZE)
		return;

	/* Free any previous response (shouldn't happen in normal flow). */
	free(g_active_conn->resp_buf);
	g_active_conn->resp_buf = malloc(len);
	if (!g_active_conn->resp_buf) {
		g_active_conn->resp_len = 0;
		return;
	}

	if (libriscv_copy_from_guest(m, g_active_conn->resp_buf, guest_buf, len) != 0) {
		free(g_active_conn->resp_buf);
		g_active_conn->resp_buf = NULL;
		g_active_conn->resp_len = 0;
		return;
	}
	g_active_conn->resp_len = len;
}

/* ── epoll helpers ──────────────────────────────────────────────── */

static int make_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return -1;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listen_socket(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr = {
		.sin_family      = AF_INET,
		.sin_port        = htons((uint16_t)port),
		.sin_addr.s_addr = INADDR_ANY,
	};
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
	    listen(fd, BACKLOG) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ── timing ─────────────────────────────────────────────────────── */

static struct timespec ts_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t;
}

static int64_t ts_nanoseconds(struct timespec a, struct timespec b)
{
	return (b.tv_sec - a.tv_sec) * (int64_t)1000000000
	     + (b.tv_nsec - a.tv_nsec);
}

/* ── per-request VM dispatch ─────────────────────────────────────── */

static uint64_t g_handle_addr = 0; /* address of guest handle() */

static void dispatch_request(RISCVMachine *master, Connection *conn)
{
	/* Install options reusing stdout/error from above. */
	RISCVOptions fork_opts;
	libriscv_set_defaults(&fork_opts);
	fork_opts.error  = error_cb;
	fork_opts.stdout = stdout_cb;

	struct timespec t0 = ts_now();

	RISCVMachine *fork = libriscv_fast_fork(master, &fork_opts);
	if (!fork) {
		fprintf(stderr, "Failed to create fork\n");
		return;
	}

	/* Expose the active connection to the syscall handlers. */
	g_active_conn = conn;

	/* Jump to handle() in the fork and run until it calls fast_exit. */
	if (libriscv_setup_vmcall(fork, g_handle_addr) == 0)
		libriscv_run(fork, 100000000ULL /* 100 M insn limit */);

	g_active_conn = NULL;

	struct timespec t1 = ts_now();

	libriscv_delete(fork);

	struct timespec t2 = ts_now();

	printf("[server] fork+run+destroy: %" PRId64 " µs  (run=%" PRId64 " µs)\n",
	       ts_nanoseconds(t0, t2) / 1000,
	       ts_nanoseconds(t0, t1) / 1000);
}

/* ── main event loop ────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <guest-elf>\n", argv[0]);
		return 1;
	}

	/* ── Load ELF ─────────────────────────────────────────────── */
	char  *elf_data = NULL;
	int    elf_size = libriscv_load_binary_file(argv[1], &elf_data);
	if (elf_size <= 0) {
		fprintf(stderr, "Failed to load %s\n", argv[1]);
		return 1;
	}

	/* ── Install custom syscalls (global, before any machine) ─── */
	libriscv_set_syscall_handler(SYSCALL_GET_REQUEST,   syscall_get_request);
	libriscv_set_syscall_handler(SYSCALL_SEND_RESPONSE, syscall_send_response);

	/* ── Create and warm up the master VM ─────────────────────── */
	RISCVOptions master_opts;
	libriscv_set_defaults(&master_opts);
	master_opts.error              = error_cb;
	master_opts.stdout             = stdout_cb;
	master_opts.strict_sandbox     = 1;
	master_opts.native_syscall_base = NATIVE_SYSCALL_BASE;
	master_opts.arena_size         = 8ULL << 20; /* 8 MiB heap */

	/* Minimal argv so the Linux CRT starts up correctly. */
	const char *guest_argv[] = { "handler", NULL };
	master_opts.argc = 1;
	master_opts.argv = guest_argv;

	printf("[server] Initializing master VM from %s ...\n", argv[1]);

	RISCVMachine *master = libriscv_new(elf_data, (unsigned)elf_size, &master_opts);
	if (!master) {
		fprintf(stderr, "Failed to create master VM\n");
		free(elf_data);
		return 1;
	}

	/* Run main() — it calls fast_exit(0) immediately. */
	if (libriscv_run(master, 50000000ULL) != 0) {
		fprintf(stderr, "Master VM failed during initialization\n");
		libriscv_delete(master);
		free(elf_data);
		return 1;
	}

	/* Resolve handle() address once; all forks share the symbol table. */
	g_handle_addr = libriscv_address_of(master, "handle");
	if (g_handle_addr == 0) {
		fprintf(stderr, "Symbol 'handle' not found in guest ELF\n");
		libriscv_delete(master);
		free(elf_data);
		return 1;
	}
	printf("[server] guest handle() at 0x%" PRIx64 "\n", g_handle_addr);
	printf("[server] master heap: 0x%" PRIx64 "  stack: 0x%" PRIx64 "\n",
	       libriscv_heap_address(master),
	       libriscv_stack_initial(master));

	/* ── Set up epoll + listening socket ──────────────────────── */
	int listen_fd = create_listen_socket(LISTEN_PORT);
	if (listen_fd < 0) {
		perror("socket/bind/listen");
		libriscv_delete(master);
		free(elf_data);
		return 1;
	}

	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (epoll_fd < 0) {
		perror("epoll_create1");
		close(listen_fd);
		libriscv_delete(master);
		free(elf_data);
		return 1;
	}

	struct epoll_event ev = { .events = EPOLLIN, .data.fd = listen_fd };
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

	printf("[server] Listening on http://127.0.0.1:%d\n", LISTEN_PORT);
	printf("[server] Press Ctrl-C to stop.\n");

	struct epoll_event events[MAX_EVENTS];

	for (;;) {
		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}

		for (int i = 0; i < n; i++) {
			int ev_fd = events[i].data.fd;

			if (ev_fd == listen_fd) {
				/* Accept all pending connections. */
				for (;;) {
					struct sockaddr_in peer;
					socklen_t plen = sizeof(peer);
					int conn_fd = accept4(listen_fd,
					    (struct sockaddr *)&peer, &plen,
					    SOCK_NONBLOCK | SOCK_CLOEXEC);
					if (conn_fd < 0)
						break; /* EAGAIN */

					Connection *conn = calloc(1, sizeof(*conn));
					if (!conn) {
						close(conn_fd);
						continue;
					}
					conn->fd = conn_fd;

					struct epoll_event ce = {
						.events   = EPOLLIN | EPOLLET,
						.data.ptr = conn,
					};
					epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ce);
				}
			} else {
				/* Data ready on a connection fd. */
				Connection *conn = events[i].data.ptr;

				ssize_t r = recv(conn->fd, conn->req_buf,
				    sizeof(conn->req_buf) - 1, 0);
				if (r > 0) {
					conn->req_len = (unsigned)r;

					/* Process in an isolated VM fork. */
					dispatch_request(master, conn);

					/* Send response if the guest produced one. */
					if (conn->resp_buf && conn->resp_len > 0) {
						send(conn->fd, conn->resp_buf,
						     conn->resp_len, MSG_NOSIGNAL);
						free(conn->resp_buf);
						conn->resp_buf = NULL;
						conn->resp_len = 0;
					}
				}

				epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
				close(conn->fd);
				free(conn);
			}
		}
	}

	close(epoll_fd);
	close(listen_fd);
	libriscv_delete(master);
	free(elf_data);
	return 0;
}
