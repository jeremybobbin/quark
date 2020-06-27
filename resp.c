/* See LICENSE file for copyright and license details. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "resp.h"
#include "util.h"

static int
compareent(const struct dirent **d1, const struct dirent **d2)
{
	int v;

	v = ((*d2)->d_type == DT_DIR ? 1 : -1) -
	    ((*d1)->d_type == DT_DIR ? 1 : -1);
	if (v) {
		return v;
	}

	return strcmp((*d1)->d_name, (*d2)->d_name);
}

static char *
suffix(int t)
{
	switch (t) {
	case DT_FIFO: return "|";
	case DT_DIR:  return "/";
	case DT_LNK:  return "@";
	case DT_SOCK: return "=";
	}

	return "";
}

enum status
resp_exec(int fd, char *name, struct request *r, struct stat *st)
{
	enum status sta;
	int to[2], from[2];
	pid_t script;
	ssize_t bread, bwritten;
	static char buf[BUFSIZ], t[TIMESTAMP_LEN];

	/* check if script is executable */
	if (!(st->st_mode & S_IXOTH)) {
		return http_send_status(fd, S_FORBIDDEN);
	}

	/* open two pipes in case for POST method; this doesn't break operation if GET method is used */
	if (pipe(from) < 0) {
		return http_send_status(fd, S_INTERNAL_SERVER_ERROR);
	}

	if (pipe(to) < 0) {
			return http_send_status(fd, S_INTERNAL_SERVER_ERROR);
	}

	/* start script */
	if (!(script = fork())) {
		close(from[0]);
		close(to[1]);
		dup2(from[1], 1);
		dup2(to[0], 0);
		/* prepend "./" to name */
		memmove(name + (sizeof("./")-1), name, MIN(BUFSIZ-(sizeof("./")-1), strlen(name)+1));
		name[0] = '.'; name[1] = '/';
		execlp(name, name, (char*) NULL);
	}

	if (script < 0) {
		return http_send_status(fd, S_INTERNAL_SERVER_ERROR);
	}
	close(from[1]);
	close(to[0]);

	/* POST method should obtain its data */
	if (dprintf(to[1], "%s", r->body) < 0) {
		return http_send_status(fd, S_INTERNAL_SERVER_ERROR);
	}
	close(to[1]);

	/* send header as late as possible */
	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n",
	            S_OK, status_str[S_OK], timestamp(time(NULL), t)) < 0) {
		sta = S_REQUEST_TIMEOUT;
		goto cleanup;
	}

	while ((bread = read(from[0], buf, BUFSIZ)) > 0) {
		if (bread < 0) {
			return S_INTERNAL_SERVER_ERROR;
		}

		bwritten = write(fd, buf, bread);

		if (bwritten < 0) {
			return S_REQUEST_TIMEOUT;
		}
	}
	sta = S_OK;
cleanup:
	if (from[0]) {
		close(from[0]);
	}

	return sta;
}



static void
html_escape(char *src, char *dst, size_t dst_siz)
{
	const struct {
		char c;
		char *s;
	} escape[] = {
		{ '&',  "&amp;"  },
		{ '<',  "&lt;"   },
		{ '>',  "&gt;"   },
		{ '"',  "&quot;" },
		{ '\'', "&#x27;" },
	};
	size_t i, j, k, esclen;

	for (i = 0, j = 0; src[i] != '\0'; i++) {
		for (k = 0; k < LEN(escape); k++) {
			if (src[i] == escape[k].c) {
				break;
			}
		}
		if (k == LEN(escape)) {
			/* no escape char at src[i] */
			if (j == dst_siz - 1) {
				/* silent truncation */
				break;
			} else {
				dst[j++] = src[i];
			}
		} else {
			/* escape char at src[i] */
			esclen = strlen(escape[k].s);

			if (j >= dst_siz - esclen) {
				/* silent truncation */
				break;
			} else {
				memcpy(&dst[j], escape[k].s, esclen);
				j += esclen;
			}
		}
	}
	dst[j] = '\0';
}

enum status
resp_dir(int fd, char *name, struct request *r)
{
	struct dirent **e;
	size_t i;
	int dirlen, s;
	static char t[TIMESTAMP_LEN];
	char esc[PATH_MAX /* > NAME_MAX */ * 6]; /* strlen("&...;") <= 6 */

	/* read directory */
	if ((dirlen = scandir(name, &e, NULL, compareent)) < 0) {
		return http_send_status(fd, S_FORBIDDEN);
	}

	/* send header as late as possible */
	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
		    "Content-Type: text/html; charset=utf-8\r\n"
		    "\r\n",
	            S_OK, status_str[S_OK], timestamp(time(NULL), t)) < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}

	if (r->method == M_GET) {
		/* listing header */
		html_escape(name, esc, sizeof(esc));
		if (dprintf(fd,
		            "<!DOCTYPE html>\n<html>\n\t<head>"
		            "<title>Index of %s</title></head>\n"
		            "\t<body>\n\t\t<a href=\"..\">..</a>",
		            esc) < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}

		/* listing */
		for (i = 0; i < (size_t)dirlen; i++) {
			/* skip hidden files, "." and ".." */
			if (e[i]->d_name[0] == '.') {
				continue;
			}

			/* entry line */
			html_escape(e[i]->d_name, esc, sizeof(esc));
			if (dprintf(fd, "<br />\n\t\t<a href=\"%s%s\">%s%s</a>",
			            esc,
			            (e[i]->d_type == DT_DIR) ? "/" : "",
			            esc,
			            suffix(e[i]->d_type)) < 0) {
				s = S_REQUEST_TIMEOUT;
				goto cleanup;
			}
		}

		/* listing footer */
		if (dprintf(fd, "\n\t</body>\n</html>\n") < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}
	}
	s = S_OK;

cleanup:
	while (dirlen--) {
		free(e[dirlen]);
	}
	free(e);

	return s;
}

enum status
resp_file(int fd, char *name, struct request *r, struct stat *st, char *mime,
          off_t lower, off_t upper)
{
	FILE *fp;
	enum status s;
	ssize_t bread, bwritten;
	off_t remaining;
	int range;
	static char buf[BUFSIZ], *p, t1[TIMESTAMP_LEN], t2[TIMESTAMP_LEN];

	/* open file */
	if (!(fp = fopen(name, "r"))) {
		s = http_send_status(fd, S_FORBIDDEN);
		goto cleanup;
	}

	/* seek to lower bound */
	if (fseek(fp, lower, SEEK_SET)) {
		s = http_send_status(fd, S_INTERNAL_SERVER_ERROR);
		goto cleanup;
	}

	/* send header as late as possible */
	range = r->field[REQ_RANGE][0];
	s = range ? S_PARTIAL_CONTENT : S_OK;

	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
	            "Last-Modified: %s\r\n"
	            "Content-Type: %s\r\n"
	            "Content-Length: %zu\r\n"
		    "Accept-Ranges: bytes\r\n",
	            s, status_str[s], timestamp(time(NULL), t1),
	            timestamp(st->st_mtim.tv_sec, t2), mime,
	            upper - lower + 1) < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}
	if (range) {
		if (dprintf(fd, "Content-Range: bytes %zd-%zd/%zu\r\n",
		            lower, upper + (upper < 0), st->st_size) < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}
	}
	if (dprintf(fd, "\r\n") < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}

	if (r->method == M_GET) {
		/* write data until upper bound is hit */
		remaining = upper - lower + 1;

		while ((bread = fread(buf, 1, MIN(sizeof(buf),
		                      (size_t)remaining), fp))) {
			if (bread < 0) {
				s = S_INTERNAL_SERVER_ERROR;
				goto cleanup;
			}
			remaining -= bread;
			p = buf;
			while (bread > 0) {
				bwritten = write(fd, p, bread);
				if (bwritten <= 0) {
					s = S_REQUEST_TIMEOUT;
					goto cleanup;
				}
				bread -= bwritten;
				p += bwritten;
			}
		}
	}
cleanup:
	if (fp) {
		fclose(fp);
	}

	return s;
}
