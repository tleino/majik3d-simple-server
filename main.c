#include <err.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

#define MAX_USERS 128
#define READ_BLOCK 128

size_t
parseline(char *base, char *dst, size_t dstsz);
int tcpbind(const char *ip, int port);

struct server;

struct evsrc
{
	struct pollfd *pfd;
	int (*readcb)(struct evsrc *);
	struct server *server;
	int sz;
	int objid;
	double x;
	double y;
	double heading;
	char buf[READ_BLOCK];
};

struct server
{
	struct evsrc srcs[MAX_USERS + 1];
	struct pollfd pfds[MAX_USERS + 1];
	int nfds;
	int users;
	int next_objid;
	double sun_pitch;
};

struct msgv {
	size_t sz;
	const char *buf;
};

static int		 server_read(struct evsrc *);
static int		 client_read(struct evsrc *);
static void		 acceptclient(struct server *);
static struct evsrc	*server_add_evsrc(struct server *,
			    int, int(*)(struct evsrc *));
static void		 handle_msg(struct evsrc *, const char *);
static struct msgv	 msgv(const char *, ...);
static void		 send_msg(int, struct msgv);
static void		 broadcast_msg(struct server *, struct msgv);

static int
server_read(struct evsrc *src)
{
	acceptclient(src->server);
	return 0;
}

static void
send_msg(int fd, struct msgv msg)
{
	if (msg.buf == NULL || msg.sz == 0)
		return;

	if (send(fd, msg.buf, msg.sz, MSG_DONTWAIT | MSG_NOSIGNAL) == -1)
		warn("send");
}

static struct msgv
msgv(const char *fmt, ...)
{
	static char buf[1024];
	struct msgv m = { 0 };
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (n >= sizeof(buf)) {
		warn("too long message in msgv");
		return m;
	}
	m.sz = n;
	m.buf = buf;
	return m;
}

static void
broadcast_msg(struct server *srv, struct msgv msg)
{
	int i;

	for (i = 0; i < srv->nfds; i++)
		if (srv->srcs[i].objid != 0)
			send_msg(srv->pfds[i].fd, msg);
}

static void
send_all_objs(struct server *srv, struct evsrc *src)
{
	int i;
	struct evsrc *other;

	for (i = 0; i < srv->nfds; i++) {
		other = &srv->srcs[i];
		if (other->objid != 0)
			send_msg(src->pfd->fd,
			    msgv("55 %d %f %f %d %s\r\n",
			    other->objid, other->x, other->y,
			    (int) other->heading,
			    rand() % 2 == 0 ? "stickman.ac" : "snowman.ac"));
	}
}

static void
handle_msg(struct evsrc *src, const char *msg)
{
	int code;
	int startstop;
	char *endp;
	float x, y, heading;

	if (msg == NULL || msg[0] == '\0')
		return;

	code = (int)strtol(msg, &endp, 10);
	if (endp == msg || endp == NULL) {
		warn("parse error while parsing code");
		return;
	}
	switch (code) {
	case 57:
		if (src->objid != 0) {
			startstop = 0;
			x = 0;
			y = 0;
			heading = 0;
			if (sscanf(msg, "%*d %f %f %f %d",
			    &x, &y, &heading, &startstop) != 4) {
				warn("parse error while parsing 57");
			} else {
				if (startstop == 0) {
					src->x = x;
					src->y = y;
					src->heading = heading;
					broadcast_msg(src->server,
					    msgv("50 %d %f %f %d\r\n",
					    src->objid, src->x, src->y,
					    (int) src->heading));
				}
			}
		}
		break;
	case 51:
		if (src->objid == 0) {
			src->objid = src->server->next_objid++;
			src->x = 5000.0 + rand() % 200;
			src->y = 5000.0 + rand() % 200;
			src->heading = rand() % 360;

			send_msg(src->pfd->fd, msgv("52 %d\r\n", src->objid));
			send_all_objs(src->server, src);
			broadcast_msg(src->server,
			    msgv("55 %d %f %f %d %s\r\n",
			    src->objid, src->x, src->y, (int) src->heading,
			    rand() % 2 == 0 ? "stickman.ac" : "snowman.ac"));
			broadcast_msg(src->server, msgv("56 %f %f %f %f\r\n",
			    M_PI/2, src->server->sun_pitch, 1.2, 3.5));
		}
		break;
	}
}

static int
client_read(struct evsrc *src)
{
	int n;
	int len;
	char dst[READ_BLOCK];

	if (sizeof(src->buf) - 1 - src->sz <= 0) {
		warnx("discarded %d bytes; too long line", src->sz);
		src->sz = 0;
	}
	n = read(src->pfd->fd, &src->buf[src->sz],
	    sizeof(src->buf) - 1 - src->sz);
	if (n > 0) {
		src->sz += n;
		src->buf[src->sz] = '\0';
		while ((len = parseline(src->buf, dst, sizeof(dst)))
		    != -1) {
			handle_msg(src, dst);
			src->sz -= len;
		}
	} else if (n < 0 || n == 0) {
		src->pfd->events = 0;
		return -1;
	}
	return 0;
}

static void
acceptclient(struct server *srv)
{
	int fd;
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	struct evsrc *src;

	fd = accept(srv->pfds[0].fd, (struct sockaddr *) &addr, &len);
	if (fd < 0) {
		warn("accept");
		return;
	}

	src = server_add_evsrc(srv, fd, client_read);


#define START_MSG "220 51\r\n"
	if (send(fd, START_MSG, strlen(START_MSG),
	    MSG_DONTWAIT | MSG_NOSIGNAL) == -1)
		warn("send");
}

static void
wait_ev(struct server *srv)
{
	int i, nready;

#ifndef INFTIM
#define INFTIM -1
#endif

	nready = poll(srv->pfds, srv->nfds, 1000);
	if (nready == -1)
		err(1, "poll");
	if (nready == 0) {
		if (srv->sun_pitch > 0.03)
			srv->sun_pitch -= 0.03;
		else
			srv->sun_pitch = M_PI + 0.2;

		broadcast_msg(srv, msgv("56 %f %f %f %f\r\n",
		    M_PI/2, srv->sun_pitch, 1.2, 3.5));

		return;
	}
	i = 0;
	while (nready && i < srv->nfds) {
		if (srv->pfds[i].revents & (POLLIN)) {
			if (srv->srcs[i].readcb(&srv->srcs[i]) == -1) {
				memmove(&srv->srcs[i], &srv->srcs[i+1],
				    sizeof(struct evsrc) * srv->nfds - i - 1);
				memmove(&srv->pfds[i], &srv->pfds[i+1],
				    sizeof(struct pollfd) * srv->nfds - i - 1);
				srv->nfds--;
				break;
			}
			nready--;
		}
		i++;
	}
}

static struct evsrc *
server_add_evsrc(struct server *srv, int fd, int (*readcb)(struct evsrc *))
{
	struct evsrc *src;

	src = &srv->srcs[srv->nfds];
	src->pfd = &srv->pfds[srv->nfds];
	src->pfd->events = POLLIN;
	src->pfd->fd = fd;
	src->readcb = readcb;
	src->server = srv;
	src->sz = 0;
	src->buf[0] = '\0';
	srv->nfds++;

	return src;
}

int
main(int argc, char *argv[])
{
	static struct server srv;
	int fd;

	srand(time(0));

	fd = tcpbind("*", 4002);
	if (fd < 0)
		err(1, "tcpbind");

	srv.next_objid = 1000;
	srv.sun_pitch = M_PI + 0.2;
	server_add_evsrc(&srv, fd, server_read);

	for (;;)
		wait_ev(&srv);
}
