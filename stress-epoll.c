/*
 * Copyright (C) 2013-2015 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#define _GNU_SOURCE

#if defined(__linux__)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef AF_INET6
#include <netinet/in.h>
#endif
#ifdef AF_UNIX
#include <sys/un.h>
#endif
#include <fcntl.h>
#include <netdb.h>
#if defined(_POSIX_PRIORITY_SCHEDULING)
#include <sched.h>
#endif

#include "stress-ng.h"

#define MAX_EPOLL_EVENTS 	(1024)
#define MAX_SERVERS		(4)

static int opt_epoll_domain = AF_UNIX;
static int opt_epoll_port = DEFAULT_EPOLL_PORT;
static int max_servers = 1;
static timer_t epoll_timerid;

typedef void (epoll_func_t)(
	const int child,
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name,
	const pid_t ppid);

typedef struct {
	const char *name;
	const int  domain;
	const int  max_servers;
} domain_t;

static const domain_t domains[] = {
	{ "ipv4",	AF_INET,	4 },
	{ "ipv6",	AF_INET6,	4 },
	{ "unix",	AF_UNIX,	1 },
	{ NULL,		-1,		0 }
};

/*
 *  stress_set_epoll_port()
 *	set the default port base
 */
void stress_set_epoll_port(const char *optarg)
{
	opt_epoll_port = get_uint64(optarg);
	check_range("epoll-port", opt_epoll_port,
		MIN_EPOLL_PORT, MAX_EPOLL_PORT - (STRESS_PROCS_MAX * MAX_SERVERS));
}

/*
 *  stress_set_epoll_domain()
 *	set the socket domain option
 */
int stress_set_epoll_domain(const char *name)
{
	int i;

	for (i = 0; domains[i].name; i++) {
		if (!strcmp(name, domains[i].name)) {
			opt_epoll_domain = domains[i].domain;
			max_servers = domains[i].max_servers;
			return 0;
		}
	}
	fprintf(stderr, "socket domain must be one of:");
	for (i = 0; domains[i].name; i++)
		fprintf(stderr, " %s", domains[i].name);
	fprintf(stderr, "\n");
	return -1;
}

/*
 * epoll_timer_handler()
 *	catch timer signal and cancel if no more runs flagged
 */
static void epoll_timer_handler(int sig)
{
	(void)sig;

	/* Cancel timer if we detect no more runs */
	if (!opt_do_run) {
		struct itimerspec timer;

		timer.it_value.tv_sec = 0;
		timer.it_value.tv_nsec = 0;
		timer.it_interval.tv_sec = timer.it_value.tv_sec;
		timer.it_interval.tv_nsec = timer.it_value.tv_nsec;

		timer_settime(epoll_timerid, 0, &timer, NULL);
	}
}


/*
 *  handle_socket_sigalrm()
 *	catch SIGALRM
 */
static void handle_socket_sigalrm(int dummy)
{
	(void)dummy;
	opt_do_run = false;
}


/*
 *  epoll_spawn()
 *	spawn a process
 */
static int epoll_spawn(
	epoll_func_t func,
	const int child,
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name,
	const pid_t ppid)
{
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		func(child, counter, instance, max_ops, name, ppid);
		exit(EXIT_SUCCESS);
	}
	return pid;
}

/*
 *  epoll_set_fd_nonblock()
 *	set non-blocking mode on fd
 */
static int epoll_set_fd_nonblock(const int fd)
{
	int flags;

	if ((flags = fcntl(fd, F_GETFL, 0)) < 0)
		return -1;
  	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

/*
 *  epoll_recv_data()
 *	receive data on fd
 */
static void epoll_recv_data(const int fd) {

	for (;;) {
		char buf[8192];
		ssize_t n;

		n = recv(fd, buf, sizeof(buf), 0);
		if (n == -1) {
			if (errno != EAGAIN)
  				(void)close(fd);
			break;
		} else if (n == 0) {
  			(void)close(fd);
			break;
		}
	}
}

/*
 *  epoll_ctl_add()
 *	add fd to epoll list
 */
static int epoll_ctl_add(const int efd, const int fd)
{
	struct epoll_event event;

	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &event) < 0)
		return -1;

	return 0;
}

/*
 *  epoll_notification()
 *	handle accept notification on sfd, add
 *	fd's to epoll event list
 */
static int epoll_notification(
	const char *name,
	const int efd,
	const int sfd)
{
	for (;;) {
		struct sockaddr saddr;
		socklen_t slen = sizeof(saddr);
		int fd;

		if ((fd = accept(sfd, &saddr, &slen)) < 0) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				/* all incoming connections handled so finish */
				return 0;
			}
			if ((errno == EMFILE) || (errno == ENFILE)) {
				/* out of file descriptors! */
				return 0;
			}
			pr_failed_err(name, "accept");
			return -1;
		}
		/*
		 *  Add non-blocking fd to epoll event list
		 */
		if (epoll_set_fd_nonblock(fd) < 0) {
			pr_failed_err(name, "setting socket to non-blocking");
			(void)close(fd);
			return -1;
		}
		if (epoll_ctl_add(efd, fd) < 0) {
			pr_failed_err(name, "epoll ctl add");
			(void)close(fd);
			return -1;
		}
	}
	return 0;
}

/*
 *  epoll_client()
 *	rapidly try to connect to server(s) and
 *	send a relatively short message
 */
static int epoll_client(
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name,
	const pid_t ppid)
{
	int port_counter = 0;
	uint64_t connect_timeouts = 0;
	struct sigaction new_action;
	struct sigevent sev;
	struct itimerspec timer;
#ifdef AF_UNIX
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path),
		"/tmp/stress-ng-%d-%" PRIu32,
		ppid, instance);
#endif
	new_action.sa_flags = 0;
	new_action.sa_handler = epoll_timer_handler;
	sigemptyset(&new_action.sa_mask);
	if (sigaction(SIGRTMIN, &new_action, NULL) < 0) {
		pr_failed_err(name, "sigaction");
		return -1;
	}

	do {
		char buf[4096];
		int fd;
		int retries = 0;
		int ret = -1;
		int port = opt_epoll_port + port_counter + (max_servers * instance);

		/* Cycle through the servers */
		port_counter = (port_counter + 1) % max_servers;
retry:
		if (!opt_do_run)
			break;

		if ((fd = socket(opt_epoll_domain, SOCK_STREAM, 0)) < 0) {
			pr_failed_dbg(name, "socket");
			return -1;
		}

		sev.sigev_notify = SIGEV_SIGNAL;
		sev.sigev_signo = SIGRTMIN;
		sev.sigev_value.sival_ptr = &epoll_timerid;
		if (timer_create(CLOCK_REALTIME, &sev, &epoll_timerid) < 0) {
			pr_failed_err(name, "timer_create");
			close(fd);
			return -1;
		}

		/*
		 * Allow 1 second for connection to occur,
		 * connect can block if the connection table
		 * fills up because we're waiting for TIME-OUTs
		 * to occur on previously closed connections
		 */
		timer.it_value.tv_sec = 0;
		timer.it_value.tv_nsec = 250000000;
		timer.it_interval.tv_sec = timer.it_value.tv_sec;
		timer.it_interval.tv_nsec = timer.it_value.tv_nsec;
		if (timer_settime(epoll_timerid, 0, &timer, NULL) < 0) {
			pr_failed_err(name, "timer_settime");
			close(fd);
			return -1;
		}
		errno = 0;
		switch (opt_epoll_domain) {
		case AF_INET: {
				struct sockaddr_in addr;

				memset(&addr, 0, sizeof(addr));
				addr.sin_family = opt_epoll_domain;
				addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
				addr.sin_port = htons(port);
				ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
			}
			break;
#ifdef AF_INET6
		case AF_INET6: {
				struct sockaddr_in6 addr;

				memset(&addr, 0, sizeof(addr));
				addr.sin6_family = opt_epoll_domain;
				addr.sin6_addr = in6addr_loopback;
				addr.sin6_port = htons(port);
				ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
			}
			break;
#endif
#ifdef AF_UNIX
		case AF_UNIX:
			ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
			break;
#endif

		default:
			pr_failed_err(name, "unknown domain");
			close(fd);
			return -1;
		}

		/* No longer need timer */
		if (timer_delete(epoll_timerid) < 0) {
			pr_failed_err(name, "timer_delete");
			close(fd);
			return -1;
		}

		if (ret < 0) {
			switch (errno) {
			case EINTR:
				connect_timeouts++;
				break;
			case ECONNREFUSED:	/* No servers yet running */
			case ENOENT:		/* unix domain not yet created */
				break;
			default:
				pr_dbg(stderr, "%s: connect failed: %d (%s)\n",
					name, errno, strerror(errno));
				break;
			}
			(void)close(fd);
			usleep(1000);		/* Twiddle fingers for a moment */

			retries++;
			if (retries > 100) {
				/* Sigh, give up.. */
				pr_failed_dbg(name, "connect");
				return -1;
			}
			goto retry;
		}

		retries = 0;

		memset(buf, 'A' + (*counter % 26), sizeof(buf));
		if (send(fd, buf, sizeof(buf), 0) < 0) {
			(void)close(fd);
			pr_failed_dbg(name, "send");
			break;
		}
		(void)close(fd);
#if defined(_POSIX_PRIORITY_SCHEDULING)
		sched_yield();
#endif
		(*counter)++;
	} while (opt_do_run && (!max_ops || *counter < max_ops));

#ifdef AF_UNIX
	if (opt_epoll_domain == AF_UNIX) {
		(void)unlink(addr.sun_path);
	}
#endif
	if (connect_timeouts)
		pr_dbg(stderr, "%s: %" PRIu64 " x 0.25 second connect timeouts, "
			"connection table full (instance %" PRIu32 ")\n",
			name, connect_timeouts, instance);
	return EXIT_SUCCESS;
}

/*
 *  epoll_server()
 *	wait on connections and read data
 */
static void epoll_server(
	const int child,
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name,
	const pid_t ppid)
{
	int efd = -1, sfd = -1, ret = -1, rc = EXIT_SUCCESS;
	int so_reuseaddr = 1;
	int port = opt_epoll_port + child + (max_servers * instance);
	struct sigaction new_action;
	struct epoll_event *events = NULL;
#ifdef AF_UNIX
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path),
		"/tmp/stress-ng-%d-%" PRIu32,
		ppid, instance);
#endif

	new_action.sa_handler = handle_socket_sigalrm;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	if (sigaction(SIGALRM, &new_action, NULL) < 0) {
		pr_failed_err(name, "sigaction");
		rc = EXIT_FAILURE;
		goto die;
	}
	if ((sfd = socket(opt_epoll_domain, SOCK_STREAM, 0)) < 0) {
		pr_failed_err(name, "socket");
		rc = EXIT_FAILURE;
		goto die;
	}
	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr)) < 0) {
		pr_failed_err(name, "setsockopt");
		rc = EXIT_FAILURE;
		goto die_close;
	}

	switch (opt_epoll_domain) {
	case AF_INET: {
			struct sockaddr_in addr;

			memset(&addr, 0, sizeof(addr));
			addr.sin_family = opt_epoll_domain;
			addr.sin_addr.s_addr = htonl(INADDR_ANY);
			addr.sin_port = htons(port);
			ret = bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
		}
		break;
#ifdef AF_INET6
	case AF_INET6: {
			struct sockaddr_in6 addr;

			memset(&addr, 0, sizeof(addr));
			addr.sin6_family = opt_epoll_domain;
			addr.sin6_addr = in6addr_any;
			addr.sin6_port = htons(port);
			ret = bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
		}
		break;
#endif
#ifdef AF_UNIX
	case AF_UNIX:
		ret = bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
		break;
#endif
	default:
		pr_failed_err(name, "unknown domain");
		(void)kill(getppid(), SIGALRM);
		exit(EXIT_FAILURE);
	}
	if (ret < 0) {
		pr_failed_err(name, "bind");
		rc = EXIT_FAILURE;
		goto die_close;
	}

	if (epoll_set_fd_nonblock(sfd) < 0) {
		pr_failed_err(name, "setting socket to non-blocking");
		rc = EXIT_FAILURE;
		goto die_close;
	}
	if (listen(sfd, SOMAXCONN) < 0) {
		pr_failed_err(name, "listen");
		rc = EXIT_FAILURE;
		goto die_close;
	}
	if ((efd = epoll_create1(0)) < 0) {
		pr_failed_err(name, "epoll_create1");
		rc = EXIT_FAILURE;
		goto die_close;
	}
	if (epoll_ctl_add(efd, sfd) < 0) {
		pr_failed_err(name, "epoll ctl add");
		rc = EXIT_FAILURE;
		goto die_close;
	}
	if ((events = calloc(MAX_EPOLL_EVENTS, sizeof(struct epoll_event))) == NULL) {
		pr_failed_err(name, "epoll ctl add");
		rc = EXIT_FAILURE;
		goto die_close;
	}

	do {
		int n, i;

		memset(events, 0, MAX_EPOLL_EVENTS * sizeof(struct epoll_event));
		errno = 0;

		/*
		 * Wait for 100ms for an event, allowing us to
		 * to break out if opt_do_run has been changed
		 */
		n = epoll_wait(efd, events, MAX_EPOLL_EVENTS, 100);
		if (n < 0) {
			if (errno != EINTR) {
				pr_failed_err(name, "epoll_wait");
				rc = EXIT_FAILURE;
				goto die_close;
			}
			break;
		}

		for (i = 0; i < n; i++) {
			if ((events[i].events & EPOLLERR) ||
			    (events[i].events & EPOLLHUP) ||
			    (!(events[i].events & EPOLLIN))) {
				/*
				 *  Error has occurred or fd is not
				 *  for reading anymore.. so reap fd
				 */
				(void)close(events[i].data.fd);
			} else if (sfd == events[i].data.fd) {
				/*
				 *  The listening socket has notification(s)
				 *  pending, so handle incomming connections
				 */
				if (epoll_notification(name, efd, sfd) < 0)
					break;
			} else {
				/*
				 *  The fd has data available, so read it
				 */
				epoll_recv_data(events[i].data.fd);
			}
		}
	} while (opt_do_run && (!max_ops || *counter < max_ops));

die_close:
	if (efd != -1)
		(void)close(efd);
	if (sfd != -1)
		(void)close(sfd);
die:
#ifdef AF_UNIX
	if (opt_epoll_domain == AF_UNIX) {
		(void)unlink(addr.sun_path);
	}
#endif
	free(events);

	exit(rc);
}

/*
 *  stress_epoll
 *	stress by heavy socket I/O
 */
int stress_epoll(
	uint64_t *const counter,
	const uint32_t instance,
	const uint64_t max_ops,
	const char *name)
{
	pid_t pids[MAX_SERVERS], ppid = getppid();
	int i, rc = EXIT_SUCCESS;

	if (max_servers == 1) {
		pr_dbg(stderr, "%s: process [%d] using socket port %d\n",
			name, getpid(),
			opt_epoll_port + instance);
	} else {
		pr_dbg(stderr, "%s: process [%d] using socket ports %d..%d\n",
			name, getpid(),
			opt_epoll_port + (max_servers * instance),
			opt_epoll_port + (max_servers * (instance + 1)) - 1);
	}

	/*
	 *  Spawn off servers to handle multi port connections.
	 *  The (src address, src port, dst address, dst port) tuple
	 *  is kept in the connection table for a default of 60 seconds
	 *  which means for many fast short connections we can
	 *  fill this table up and new connections get blocked until
	 *  this table empties. One strategy is to reduce TIME_WAIT (not
	 *  good) so the easiest way forward is to just increase the
	 *  number of ports being listened to to increase the tuple
	 *  range and hence allow more connections.  See
	 *  http://vincent.bernat.im/en/blog/2014-tcp-time-wait-state-linux.html
	 *  Typically, we are limited to ~500 connections per second
	 *  on a default Linux configuration.
	 */
	memset(pids, 0, sizeof(pids));
	for (i = 0; i < max_servers; i++) {
		pids[i] = epoll_spawn(epoll_server, i, counter, instance, max_ops, name, ppid);
		if (pids[i] < 0) {
			pr_failed_dbg(name, "fork");
			goto reap;
		}
	}

	epoll_client(counter, instance, max_ops, name, ppid);
reap:
	for (i = 0; i < max_servers; i++) {
		int status;

		if (pids[i] > 0) {
			(void)kill(pids[i], SIGKILL);
			if (waitpid(pids[i], &status, 0) < 0) {
				pr_failed_dbg(name, "waitpid");
			}
		}
	}

	return rc;
}

#endif
