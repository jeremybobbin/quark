/* See LICENSE file for copyright and license details. */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <regex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "resp.h"
#include "util.h"

const char *req_field_str[] = {
	[REQ_HOST]    = "Host",
	[REQ_RANGE]   = "Range",
	[REQ_MOD]     = "If-Modified-Since",
};

const char *req_method_str[] = {
	[M_GET]  = "GET",
	[M_HEAD] = "HEAD",
};

const char *status_str[] = {
	[S_OK]                    = "OK",
	[S_PARTIAL_CONTENT]       = "Partial Content",
	[S_MOVED_PERMANENTLY]     = "Moved Permanently",
	[S_NOT_MODIFIED]          = "Not Modified",
	[S_BAD_REQUEST]           = "Bad Request",
	[S_FORBIDDEN]             = "Forbidden",
	[S_NOT_FOUND]             = "Not Found",
	[S_METHOD_NOT_ALLOWED]    = "Method Not Allowed",
	[S_REQUEST_TIMEOUT]       = "Request Time-out",
	[S_RANGE_NOT_SATISFIABLE] = "Range Not Satisfiable",
	[S_REQUEST_TOO_LARGE]     = "Request Header Fields Too Large",
	[S_INTERNAL_SERVER_ERROR] = "Internal Server Error",
	[S_VERSION_NOT_SUPPORTED] = "HTTP Version not supported",
};

enum status
http_send_status(int fd, enum status s)
{
	static char t[TIMESTAMP_LEN];

	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
	            "%s"
	            "Content-Type: text/html; charset=utf-8\r\n"
	            "\r\n"
	            "<!DOCTYPE html>\n<html>\n\t<head>\n"
	            "\t\t<title>%d %s</title>\n\t</head>\n\t<body>\n"
	            "\t\t<h1>%d %s</h1>\n\t</body>\n</html>\n",
	            s, status_str[s], timestamp(time(NULL), t),
	            (s == S_METHOD_NOT_ALLOWED) ? "Allow: HEAD, GET\r\n" : "",
	            s, status_str[s], s, status_str[s]) < 0) {
		return S_REQUEST_TIMEOUT;
	}

	return s;
}

static void
decode(char src[PATH_MAX], char dest[PATH_MAX])
{
	size_t i;
	uint8_t n;
	char *s;

	for (s = src, i = 0; *s; s++, i++) {
		if (*s == '%' && (sscanf(s + 1, "%2hhx", &n) == 1)) {
			dest[i] = n;
			s += 2;
		} else {
			dest[i] = *s;
		}
	}
	dest[i] = '\0';
}

int
http_get_request(int fd, struct request *r)
{
	struct in6_addr res;
	size_t hlen, i, mlen;
	ssize_t off;
	char h[HEADER_MAX], *p, *q;

	/* empty all fields */
	memset(r, 0, sizeof(*r));

	/*
	 * receive header
	 */
	for (hlen = 0; ;) {
		if ((off = read(fd, h + hlen, sizeof(h) - hlen)) < 0) {
			return http_send_status(fd, S_REQUEST_TIMEOUT);
		} else if (off == 0) {
			break;
		}
		hlen += off;
		if (hlen >= 4 && !memcmp(h + hlen - 4, "\r\n\r\n", 4)) {
			break;
		}
		if (hlen == sizeof(h)) {
			return http_send_status(fd, S_REQUEST_TOO_LARGE);
		}
	}

	/* remove terminating empty line */
	if (hlen < 2) {
		return http_send_status(fd, S_BAD_REQUEST);
	}
	hlen -= 2;

	/* null-terminate the header */
	h[hlen] = '\0';

	/*
	 * parse request line
	 */

	/* METHOD */
	for (i = 0; i < NUM_REQ_METHODS; i++) {
		mlen = strlen(req_method_str[i]);
		if (!strncmp(req_method_str[i], h, mlen)) {
			r->method = i;
			break;
		}
	}
	if (i == NUM_REQ_METHODS) {
		return http_send_status(fd, S_METHOD_NOT_ALLOWED);
	}

	/* a single space must follow the method */
	if (h[mlen] != ' ') {
		return http_send_status(fd, S_BAD_REQUEST);
	}

	/* basis for next step */
	p = h + mlen + 1;

	/* TARGET */
	if (!(q = strchr(p, ' '))) {
		return http_send_status(fd, S_BAD_REQUEST);
	}
	*q = '\0';
	if (q - p + 1 > PATH_MAX) {
		return http_send_status(fd, S_REQUEST_TOO_LARGE);
	}
	memcpy(r->target, p, q - p + 1);
	decode(r->target, r->target);

	/* basis for next step */
	p = q + 1;

	/* HTTP-VERSION */
	if (strncmp(p, "HTTP/", sizeof("HTTP/") - 1)) {
		return http_send_status(fd, S_BAD_REQUEST);
	}
	p += sizeof("HTTP/") - 1;
	if (strncmp(p, "1.0", sizeof("1.0") - 1) &&
	    strncmp(p, "1.1", sizeof("1.1") - 1)) {
		return http_send_status(fd, S_VERSION_NOT_SUPPORTED);
	}
	p += sizeof("1.*") - 1;

	/* check terminator */
	if (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
		return http_send_status(fd, S_BAD_REQUEST);
	}

	/* basis for next step */
	p += sizeof("\r\n") - 1;

	/*
	 * parse request-fields
	 */

	/* match field type */
	for (; *p != '\0';) {
		for (i = 0; i < NUM_REQ_FIELDS; i++) {
			if (!strncasecmp(p, req_field_str[i],
			                 strlen(req_field_str[i]))) {
				break;
			}
		}
		if (i == NUM_REQ_FIELDS) {
			/* unmatched field, skip this line */
			if (!(q = strstr(p, "\r\n"))) {
				return http_send_status(fd, S_BAD_REQUEST);
			}
			p = q + (sizeof("\r\n") - 1);
			continue;
		}

		p += strlen(req_field_str[i]);

		/* a single colon must follow the field name */
		if (*p != ':') {
			return http_send_status(fd, S_BAD_REQUEST);
		}

		/* skip whitespace */
		for (++p; *p == ' ' || *p == '\t'; p++)
			;

		/* extract field content */
		if (!(q = strstr(p, "\r\n"))) {
			return http_send_status(fd, S_BAD_REQUEST);
		}
		*q = '\0';
		if (q - p + 1 > FIELD_MAX) {
			return http_send_status(fd, S_REQUEST_TOO_LARGE);
		}
		memcpy(r->field[i], p, q - p + 1);

		/* go to next line */
		p = q + (sizeof("\r\n") - 1);
	}

	/*
	 * clean up host
	 */

	p = strrchr(r->field[REQ_HOST], ':');
	q = strrchr(r->field[REQ_HOST], ']');

	/* strip port suffix but don't interfere with IPv6 bracket notation
	 * as per RFC 2732 */
	if (p && (!q || p > q)) {
		/* port suffix must not be empty */
		if (*(p + 1) == '\0') {
			return http_send_status(fd, S_BAD_REQUEST);
		}
		*p = '\0';
	}

	/* strip the brackets from the IPv6 notation and validate the address */
	if (q) {
		/* brackets must be on the outside */
		if (r->field[REQ_HOST][0] != '[' || *(q + 1) != '\0') {
			return http_send_status(fd, S_BAD_REQUEST);
		}

		/* remove the right bracket */
		*q = '\0';
		p = r->field[REQ_HOST] + 1;

		/* validate the contained IPv6 address */
		if (inet_pton(AF_INET6, p, &res) != 1) {
			return http_send_status(fd, S_BAD_REQUEST);
		}

		/* copy it into the host field */
		memmove(r->field[REQ_HOST], p, q - p + 1);
	}

	return 0;
}

static void
encode(char src[PATH_MAX], char dest[PATH_MAX])
{
	size_t i;
	char *s;

	for (s = src, i = 0; *s && i < (PATH_MAX - 4); s++) {
		if (iscntrl(*s) || (unsigned char)*s > 127) {
			i += snprintf(dest + i, PATH_MAX - i, "%%%02X",
			              (unsigned char)*s);
		} else {
			dest[i] = *s;
			i++;
		}
	}
	dest[i] = '\0';
}

static int
normabspath(char *path)
{
	size_t len;
	int last = 0;
	char *p, *q;

	/* require and skip first slash */
	if (path[0] != '/') {
		return 1;
	}
	p = path + 1;

	/* get length of path */
	len = strlen(p);

	for (; !last; ) {
		/* bound path component within (p,q) */
		if (!(q = strchr(p, '/'))) {
			q = strchr(p, '\0');
			last = 1;
		}

		if (p == q || (q - p == 1 && p[0] == '.')) {
			/* "/" or "./" */
			goto squash;
		} else if (q - p == 2 && p[0] == '.' && p[1] == '.') {
			/* "../" */
			if (p != path + 1) {
				/* place p right after the previous / */
				for (p -= 2; p > path && *p != '/'; p--);
				p++;
			}
			goto squash;
		} else {
			/* move on */
			p = q + 1;
			continue;
		}
squash:
		/* squash (p,q) into void */
		if (last) {
			*p = '\0';
			len = p - path;
		} else {
			memmove(p, q + 1, len - ((q + 1) - path) + 2);
			len -= (q + 1) - p;
		}
	}

	return 0;
}

#undef RELPATH
#define RELPATH(x) ((!*(x) || !strcmp(x, "/")) ? "." : ((x) + 1))

enum status
http_send_response(int fd, struct request *r)
{
	struct in6_addr res;
	struct stat st;
	struct tm tm;
	size_t len, i;
	off_t lower, upper;
	int hasport, ipv6host;
	static char realtarget[PATH_MAX], tmptarget[PATH_MAX], t[TIMESTAMP_LEN];
	char *p, *q, *mime;
	const char *vhostmatch, *targethost, *err;

	/* make a working copy of the target */
	memcpy(realtarget, r->target, sizeof(realtarget));

	/* match vhost */
	vhostmatch = NULL;
	if (s.vhost) {
		for (i = 0; i < s.vhost_len; i++) {
			/* switch to vhost directory if there is a match */
			if (!regexec(&s.vhost[i].re, r->field[REQ_HOST], 0,
			             NULL, 0)) {
				if (chdir(s.vhost[i].dir) < 0) {
					return http_send_status(fd, (errno == EACCES) ?
					                        S_FORBIDDEN : S_NOT_FOUND);
				}
				vhostmatch = s.vhost[i].chost;
				break;
			}
		}
		if (i == s.vhost_len) {
			return http_send_status(fd, S_NOT_FOUND);
		}

		/* if we have a vhost prefix, prepend it to the target */
		if (s.vhost[i].prefix) {
			if (esnprintf(tmptarget, sizeof(tmptarget), "%s%s",
			              s.vhost[i].prefix, realtarget)) {
				return http_send_status(fd, S_REQUEST_TOO_LARGE);
			}
			memcpy(realtarget, tmptarget, sizeof(realtarget));
		}
	}

	/* apply target prefix mapping */
	for (i = 0; i < s.map_len; i++) {
		len = strlen(s.map[i].from);
		if (!strncmp(realtarget, s.map[i].from, len)) {
			/* match canonical host if vhosts are enabled and
			 * the mapping specifies a canonical host */
			if (s.vhost && s.map[i].chost &&
			    strcmp(s.map[i].chost, vhostmatch)) {
				continue;
			}

			/* swap out target prefix */
			if (esnprintf(tmptarget, sizeof(tmptarget), "%s%s",
			              s.map[i].to, realtarget + len)) {
				return http_send_status(fd, S_REQUEST_TOO_LARGE);
			}
			memcpy(realtarget, tmptarget, sizeof(realtarget));
			break;
		}
	}

	/* normalize target */
	if (normabspath(realtarget)) {
		return http_send_status(fd, S_BAD_REQUEST);
	}

	/* reject hidden target */
	if (realtarget[0] == '.' || strstr(realtarget, "/.")) {
		return http_send_status(fd, S_FORBIDDEN);
	}

	/* stat the target */
	if (stat(RELPATH(realtarget), &st) < 0) {
		return http_send_status(fd, (errno == EACCES) ?
		                        S_FORBIDDEN : S_NOT_FOUND);
	}

	if (S_ISDIR(st.st_mode)) {
		/* add / to target if not present */
		len = strlen(realtarget);
		if (len >= PATH_MAX - 2) {
			return http_send_status(fd, S_REQUEST_TOO_LARGE);
		}
		if (len && realtarget[len - 1] != '/') {
			realtarget[len] = '/';
			realtarget[len + 1] = '\0';
		}
	}

	/* redirect if targets differ, host is non-canonical or we prefixed */
	if (strcmp(r->target, realtarget) || (s.vhost && vhostmatch &&
	    strcmp(r->field[REQ_HOST], vhostmatch))) {
		/* encode realtarget */
		encode(realtarget, tmptarget);

		/* send redirection header */
		if (s.vhost) {
			/* absolute redirection URL */
			targethost = r->field[REQ_HOST][0] ? vhostmatch ?
			             vhostmatch : r->field[REQ_HOST] : s.host ?
			             s.host : "localhost";

			/* do we need to add a port to the Location? */
			hasport = s.port && strcmp(s.port, "80");

			/* RFC 2732 specifies to use brackets for IPv6-addresses
			 * in URLs, so we need to check if our host is one and
			 * honor that later when we fill the "Location"-field */
			if ((ipv6host = inet_pton(AF_INET6, targethost,
			                          &res)) < 0) {
				return http_send_status(fd,
				                        S_INTERNAL_SERVER_ERROR);
			}

			if (dprintf(fd,
			            "HTTP/1.1 %d %s\r\n"
			            "Date: %s\r\n"
			            "Connection: close\r\n"
			            "Location: //%s%s%s%s%s%s\r\n"
			            "\r\n",
			            S_MOVED_PERMANENTLY,
			            status_str[S_MOVED_PERMANENTLY],
				    timestamp(time(NULL), t),
			            ipv6host ? "[" : "",
				    targethost,
			            ipv6host ? "]" : "", hasport ? ":" : "",
			            hasport ? s.port : "", tmptarget) < 0) {
				return S_REQUEST_TIMEOUT;
			}
		} else {
			/* relative redirection URL */
			if (dprintf(fd,
			            "HTTP/1.1 %d %s\r\n"
			            "Date: %s\r\n"
			            "Connection: close\r\n"
			            "Location: %s\r\n"
			            "\r\n",
			            S_MOVED_PERMANENTLY,
			            status_str[S_MOVED_PERMANENTLY],
				    timestamp(time(NULL), t),
				    tmptarget) < 0) {
				return S_REQUEST_TIMEOUT;
			}
		}

		return S_MOVED_PERMANENTLY;
	}

	if (S_ISDIR(st.st_mode)) {
		/* append docindex to target */
		if (esnprintf(realtarget, sizeof(realtarget), "%s%s",
		              r->target, s.docindex)) {
			return http_send_status(fd, S_REQUEST_TOO_LARGE);
		}

		/* stat the docindex, which must be a regular file */
		if (stat(RELPATH(realtarget), &st) < 0 || !S_ISREG(st.st_mode)) {
			if (s.listdirs) {
				/* remove index suffix and serve dir */
				realtarget[strlen(realtarget) -
				           strlen(s.docindex)] = '\0';
				return resp_dir(fd, RELPATH(realtarget), r);
			} else {
				/* reject */
				if (!S_ISREG(st.st_mode) || errno == EACCES) {
					return http_send_status(fd, S_FORBIDDEN);
				} else {
					return http_send_status(fd, S_NOT_FOUND);
				}
			}
		}
	}

	/* modified since */
	if (r->field[REQ_MOD][0]) {
		/* parse field */
		if (!strptime(r->field[REQ_MOD], "%a, %d %b %Y %T GMT", &tm)) {
			return http_send_status(fd, S_BAD_REQUEST);
		}

		/* compare with last modification date of the file */
		if (difftime(st.st_mtim.tv_sec, timegm(&tm)) <= 0) {
			if (dprintf(fd,
			            "HTTP/1.1 %d %s\r\n"
			            "Date: %s\r\n"
			            "Connection: close\r\n"
				    "\r\n",
			            S_NOT_MODIFIED, status_str[S_NOT_MODIFIED],
			            timestamp(time(NULL), t)) < 0) {
				return S_REQUEST_TIMEOUT;
			}
			return S_NOT_MODIFIED;
		}
	}

	/* range */
	lower = 0;
	upper = st.st_size - 1;
	if (r->field[REQ_RANGE][0]) {
		/* parse field */
		p = r->field[REQ_RANGE];
		err = NULL;

		if (strncmp(p, "bytes=", sizeof("bytes=") - 1)) {
			return http_send_status(fd, S_BAD_REQUEST);
		}
		p += sizeof("bytes=") - 1;

		if (!(q = strchr(p, '-'))) {
			return http_send_status(fd, S_BAD_REQUEST);
		}
		*(q++) = '\0';
		if (p[0]) {
			lower = strtonum(p, 0, LLONG_MAX, &err);
		}
		if (!err && q[0]) {
			upper = strtonum(q, 0, LLONG_MAX, &err);
		}
		if (err) {
			return http_send_status(fd, S_BAD_REQUEST);
		}

		/* check range */
		if (lower < 0 || upper < 0 || lower > upper) {
			if (dprintf(fd,
			            "HTTP/1.1 %d %s\r\n"
			            "Date: %s\r\n"
			            "Content-Range: bytes */%zu\r\n"
			            "Connection: close\r\n"
			            "\r\n",
			            S_RANGE_NOT_SATISFIABLE,
			            status_str[S_RANGE_NOT_SATISFIABLE],
			            timestamp(time(NULL), t),
			            st.st_size) < 0) {
				return S_REQUEST_TIMEOUT;
			}
			return S_RANGE_NOT_SATISFIABLE;
		}

		/* adjust upper limit */
		if (upper >= st.st_size)
			upper = st.st_size-1;
	}

	/* mime */
	mime = "application/octet-stream";
	if ((p = strrchr(realtarget, '.'))) {
		for (i = 0; i < sizeof(mimes) / sizeof(*mimes); i++) {
			if (!strcmp(mimes[i].ext, p + 1)) {
				mime = mimes[i].type;
				break;
			}
		}
	}

	return resp_file(fd, RELPATH(realtarget), r, &st, mime, lower, upper);
}
