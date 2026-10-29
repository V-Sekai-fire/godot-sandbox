/**
 * RISC-V guest: HTTP request handler.
 *
 * Each fork of this program handles exactly one HTTP request.
 * The host passes the raw request via a custom syscall, and this
 * code writes the HTTP response back through another syscall.
 *
 * All string/memory operations go through native_libc.h syscalls
 * (ecall instructions, no glibc code pages touched in the hot path).
 * stdio.h is intentionally excluded — snprintf would pull in glibc's
 * printf machinery, causing expensive CoW page faults in every fork.
 */

#include "api.h"

/* ── route handlers ── */

static void route_root(void)
{
	static const char body[] =
		"<html><body>"
		"<h1>Hello from a sandboxed RISC-V VM!</h1>"
		"<p>Every HTTP request is handled inside an isolated "
		"Copy-on-Write fork of the master VM.</p>"
		"<ul>"
		"<li><a href=\"/info\">/info</a></li>"
		"<li><a href=\"/echo\">/echo</a></li>"
		"</ul>"
		"</body></html>";
	http_respond(200, "OK", "text/html", body, sizeof(body) - 1);
}

static void route_info(void)
{
	static const char body[] =
		"<html><body>"
		"<h1>VM Info</h1>"
		"<p>Architecture: RISC-V RV64GC (Linux ABI)</p>"
		"<p>Isolation: fast Copy-on-Write fork per request</p>"
		"<p>Heap: host-accelerated native arena (syscall base 470)</p>"
		"</body></html>";
	http_respond(200, "OK", "text/html", body, sizeof(body) - 1);
}

static void route_echo(const char *path)
{
	static const char pre[]  = "<html><body><h1>Echo</h1><p>Path: ";
	static const char post[] = "</p></body></html>";
	unsigned path_len = strlen(path);
	unsigned body_len = (sizeof(pre) - 1) + path_len + (sizeof(post) - 1);
	char *body = malloc(body_len);
	if (!body) return;
	char *p = body;
	p = append_mem(p, pre,  sizeof(pre) - 1);
	p = append_mem(p, path, path_len);
	p = append_mem(p, post, sizeof(post) - 1);
	http_respond(200, "OK", "text/html", body, body_len);
	free(body);
}

static void route_not_found(void)
{
	static const char body[] =
		"<html><body><h1>404 Not Found</h1></body></html>";
	http_respond(404, "Not Found", "text/html", body, sizeof(body) - 1);
}

/* ── router ── */

static void parse_path(const char *req, unsigned len, char *out, unsigned out_max)
{
	const char *p   = req;
	const char *end = req + len;

	while (p < end && *p != ' ') p++;
	if (p >= end) { out[0] = '/'; out[1] = '\0'; return; }
	p++;

	const char *path_start = p;
	while (p < end && *p != ' ' && *p != '\r' && *p != '\n') p++;

	unsigned plen = (unsigned)(p - path_start);
	if (plen >= out_max) plen = out_max - 1;
	memcpy(out, path_start, plen);
	out[plen] = '\0';
}

static void dispatch(const char *path)
{
	if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
		route_root();
	else if (strcmp(path, "/info") == 0)
		route_info();
	else if (strcmp(path, "/echo") == 0)
		route_echo(path);
	else
		route_not_found();
}

/* ── entry points ── */

/**
 * main() is called once during master VM initialization.
 * It finishes immediately — the master is never used to serve requests,
 * only as a template for fast-forking.
 */
int main(void)
{
	fast_exit(0);
}

/**
 * handle() is called by the host via vmcall in each forked VM.
 * Each fork handles exactly one HTTP request then exits.
 */
void handle(void)
{
	char req[4096];
	unsigned req_len = get_request(req, sizeof(req) - 1);
	req[req_len] = '\0';

	char path[256];
	parse_path(req, req_len, path, sizeof(path));
	dispatch(path);

	fast_exit(0);
}
