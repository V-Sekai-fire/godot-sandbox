#pragma once
#include "native_libc.h"

/* Custom syscalls installed by the host */
#define SYSCALL_GET_REQUEST   500
#define SYSCALL_SEND_RESPONSE 501

static inline void fast_exit(int code)
{
	register int a0 __asm__("a0") = code;
	__asm__ volatile (".insn i SYSTEM, 0, x0, x0, 0x7ff" : : "r"(a0) : "memory");
}

static unsigned get_request(char *buf, unsigned maxlen)
{
	register char    *a0 __asm__("a0") = buf;
	register unsigned a1 __asm__("a1") = maxlen;
	register long    a7 __asm__("a7") = SYSCALL_GET_REQUEST;
	register long   ret __asm__("a0");
	__asm__ volatile ("ecall" : "=r"(ret) : "r"(a0), "r"(a1), "r"(a7) : "memory");
	return (unsigned)ret;
}

static void send_response(const char *buf, unsigned len)
{
	register const char *a0 __asm__("a0") = buf;
	register unsigned    a1 __asm__("a1") = len;
	register long        a7 __asm__("a7") = SYSCALL_SEND_RESPONSE;
	__asm__ volatile ("ecall" : : "r"(a0), "r"(a1), "r"(a7) : "memory");
}

static char *append_uint(char *p, unsigned n)
{
	char tmp[10];
	int  i = 0;
	if (n == 0) { *p++ = '0'; return p; }
	while (n > 0) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
	while (i > 0) *p++ = tmp[--i];
	return p;
}

static char *append_str(char *p, const char *s)
{
	unsigned len = strlen(s);
	memcpy(p, s, len);
	return p + len;
}

static char *append_mem(char *p, const char *s, unsigned len)
{
	memcpy(p, s, len);
	return p + len;
}

static void http_respond(int status, const char *status_text,
                         const char *content_type,
                         const char *body, unsigned body_len)
{
	char header[256];
	char *p = header;
	p = append_str(p, "HTTP/1.1 ");
	p = append_uint(p, (unsigned)status);
	*p++ = ' ';
	p = append_str(p, status_text);
	p = append_str(p, "\r\nContent-Type: ");
	p = append_str(p, content_type);
	p = append_str(p, "\r\nContent-Length: ");
	p = append_uint(p, body_len);
	p = append_str(p, "\r\nConnection: close\r\n\r\n");
	unsigned hlen = (unsigned)(p - header);

	char *resp = malloc(hlen + body_len);
	if (!resp) return;
	memcpy(resp,        header, hlen);
	memcpy(resp + hlen, body,   body_len);
	send_response(resp, hlen + body_len);
	free(resp);
}
