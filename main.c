/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <regex.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "sock.h"
#include "util.h"

static char *udsname;

static void
serve(int infd, struct sockaddr_storage *in_sa)
{
	struct request r;
	time_t t;
	enum status status;
	char inaddr[INET6_ADDRSTRLEN /* > INET_ADDRSTRLEN */];
	char tstmp[21];

	/* set connection timeout */
	if (sock_set_timeout(infd, 30)) {
		goto cleanup;
	}

	/* handle request */
	if (!(status = http_get_request(infd, &r))) {
		status = http_send_response(infd, &r);
	}

	/* write output to log */
	t = time(NULL);
	if (!strftime(tstmp, sizeof(tstmp), "%Y-%m-%dT%H:%M:%SZ",
	              gmtime(&t))) {
		warn("strftime: Exceeded buffer capacity");
		goto cleanup;
	}
	if (sock_get_inaddr_str(in_sa, inaddr, LEN(inaddr))) {
		goto cleanup;
	}
	printf("%s\t%s\t%d\t%s\t%s\n", tstmp, inaddr, status,
	       r.field[REQ_HOST], r.target);
cleanup:
	/* clean up and finish */
	shutdown(infd, SHUT_RD);
	shutdown(infd, SHUT_WR);
	close(infd);
}

static void
cleanup(void)
{
	if (udsname)
		 sock_rem_uds(udsname);
}

static void
sigcleanup(int sig)
{
	cleanup();
	kill(0, sig);
	_exit(1);
}

static void
handlesignals(void(*hdl)(int))
{
	struct sigaction sa = {
		.sa_handler = hdl,
	};

	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
}

static int
spacetok(const char *s, char **t, size_t tlen)
{
	const char *tok;
	size_t i, j, toki, spaces;

	/* fill token-array with NULL-pointers */
	for (i = 0; i < tlen; i++) {
		t[i] = NULL;
	}
	toki = 0;

	/* don't allow NULL string or leading spaces */
	if (!s || *s == ' ') {
		return 1;
	}
start:
	/* skip spaces */
	for (; *s == ' '; s++)
		;

	/* don't allow trailing spaces */
	if (*s == '\0') {
		goto err;
	}

	/* consume token */
	for (tok = s, spaces = 0; ; s++) {
		if (*s == '\\' && *(s + 1) == ' ') {
			spaces++;
			s++;
			continue;
		} else if (*s == ' ') {
			/* end of token */
			goto token;
		} else if (*s == '\0') {
			/* end of string */
			goto token;
		}
	}
token:
	if (toki >= tlen) {
		goto err;
	}
	if (!(t[toki] = malloc(s - tok - spaces + 1))) {
		die("malloc:");
	}
	for (i = 0, j = 0; j < s - tok - spaces + 1; i++, j++) {
		if (tok[i] == '\\' && tok[i + 1] == ' ') {
			i++;
		}
		t[toki][j] = tok[i];
	}
	t[toki][s - tok - spaces] = '\0';
	toki++;

	if (*s == ' ') {
		s++;
		goto start;
	}

	return 0;
err:
	for (i = 0; i < tlen; i++) {
		free(t[i]);
		t[i] = NULL;
	}

	return 1;
}


static void
usage(void)
{
	const char *opts = "[-n num] [-d dir] [-l] "
	                   "[-i file] [-v vhost] ... [-m map] ... [-c cgi] ...";

	die("usage: %s -p port [-h host] %s\n"
	    "       %s -U file [-p port] %s", argv0,
	    opts, argv0, opts);
}

int
main(int argc, char *argv[])
{
	struct rlimit rlim;
	struct sockaddr_storage in_sa;
	pid_t cpid, wpid, spid;
	size_t i;
	socklen_t in_sa_len;
	int insock, status = 0, infd;
	const char *err;
	char *tok[4];

	/* defaults */
	int maxnprocs = 512;
	char *servedir = ".";

	s.host = s.port = NULL;
	s.vhost = NULL;
	s.map = NULL;
	s.cgi = NULL;
	s.vhost_len = s.map_len = s.cgi_len = 0;
	s.docindex = "index.html";
	s.listdirs = 0;

	ARGBEGIN {
	case 'c':
		if (spacetok(EARGF(usage()), tok, 2) || !tok[0] || !tok[1]) {
			usage();
		}
		if (!(s.cgi = reallocarray(s.cgi, ++s.cgi_len,
		                             sizeof(struct cgi)))) {
			die("reallocarray:");
		}
		s.cgi[s.cgi_len - 1].regex  = tok[0];
		s.cgi[s.cgi_len - 1].dir  = tok[1];
		break;
	case 'd':
		servedir = EARGF(usage());
		break;
	case 'h':
		s.host = EARGF(usage());
		break;
	case 'i':
		s.docindex = EARGF(usage());
		if (strchr(s.docindex, '/')) {
			die("The document index must not contain '/'");
		}
		break;
	case 'l':
		s.listdirs = 1;
		break;
	case 'm':
		if (spacetok(EARGF(usage()), tok, 3) || !tok[0] || !tok[1]) {
			usage();
		}
		if (!(s.map = reallocarray(s.map, ++s.map_len,
		                           sizeof(struct map)))) {
			die("reallocarray:");
		}
		s.map[s.map_len - 1].from  = tok[0];
		s.map[s.map_len - 1].to    = tok[1];
		s.map[s.map_len - 1].chost = tok[2];
		break;
	case 'n':
		maxnprocs = strtonum(EARGF(usage()), 1, INT_MAX, &err);
		if (err) {
			die("strtonum '%s': %s", EARGF(usage()), err);
		}
		break;
	case 'p':
		s.port = EARGF(usage());
		break;
	case 'U':
		udsname = EARGF(usage());
		break;
	case 'v':
		if (spacetok(EARGF(usage()), tok, 4) || !tok[0] || !tok[1] ||
		    !tok[2]) {
			usage();
		}
		if (!(s.vhost = reallocarray(s.vhost, ++s.vhost_len,
		                             sizeof(struct vhost)))) {
			die("reallocarray:");
		}
		s.vhost[s.vhost_len - 1].chost  = tok[0];
		s.vhost[s.vhost_len - 1].regex  = tok[1];
		s.vhost[s.vhost_len - 1].dir    = tok[2];
		s.vhost[s.vhost_len - 1].prefix = tok[3];
		break;
	default:
		usage();
	} ARGEND

	if (argc) {
		usage();
	}

	/* can't have both host and UDS but must have one of port or UDS*/
	if ((s.host && udsname) || !(s.port || udsname)) {
		usage();
	}

	if (udsname && (!access(udsname, F_OK) || errno != ENOENT)) {
		die("UNIX-domain socket: %s", errno ?
		    strerror(errno) : "file exists");
	}

	/* compile and check the supplied vhost regexes */
	for (i = 0; i < s.vhost_len; i++) {
		if (regcomp(&s.vhost[i].re, s.vhost[i].regex,
		            REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
			die("regcomp '%s': invalid regex",
			    s.vhost[i].regex);
		}
	}

	/* compile and check the supplied cgi regexes */
	for (i = 0; i < s.cgi_len; i++) {
		if (regcomp(&s.cgi[i].re, s.cgi[i].regex,
		            REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
			die("regcomp '%s': invalid regex",
			    s.cgi[i].regex);
		}
	}

	/* raise the process limit */
	rlim.rlim_cur = rlim.rlim_max = maxnprocs;
	if (setrlimit(RLIMIT_NPROC, &rlim) < 0) {
		die("setrlimit RLIMIT_NPROC:");
	}

	/* Open a new process group */
	setpgid(0,0);

	handlesignals(sigcleanup);

	/* bind socket */
	insock = udsname ? sock_get_uds(udsname) :
	                   sock_get_ips(s.host, s.port);

	switch (cpid = fork()) {
	case -1:
		warn("fork:");
		break;
	case 0:
		/* restore default handlers */
		handlesignals(SIG_DFL);

		/* reap children automatically */
		if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
			die("signal: Failed to set SIG_IGN on SIGCHLD");
		}

		/* limit ourselves to reading the servedir and block further unveils */
		eunveil(servedir, "r");
		eunveil(NULL, NULL);

		if (chdir(servedir) < 0) {
			die("chdir '%s':", servedir);
		}

		if (udsname) {
			epledge("stdio rpath proc unix", NULL);
		} else {
			epledge("stdio rpath proc inet", NULL);
		}

		/* accept incoming connections */
		while (1) {
			in_sa_len = sizeof(in_sa);
			if ((infd = accept(insock, (struct sockaddr *)&in_sa,
			                   &in_sa_len)) < 0) {
				warn("accept:");
				continue;
			}

			/* fork and handle */
			switch ((spid = fork())) {
			case 0:
				serve(infd, &in_sa);
				exit(0);
				break;
			case -1:
				warn("fork:");
				/* fallthrough */
			default:
				/* close the connection in the parent */
				close(infd);
			}
		}
		exit(0);
	default:
		/* limit ourselves even further while we are waiting */
		if (udsname) {
			eunveil(udsname, "c");
			eunveil(NULL, NULL);
			epledge("stdio cpath", NULL);
		} else {
			eunveil("/", "");
			eunveil(NULL, NULL);
			epledge("stdio", NULL);
		}

		while ((wpid = wait(&status)) > 0)
			;
	}

	cleanup();
	return status;
}
