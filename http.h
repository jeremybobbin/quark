/* See LICENSE file for copyright and license details. */
#ifndef HTTP_H
#define HTTP_H

#include <limits.h>

#define HEADER_MAX 4096
#define FIELD_MAX 200

enum req_field {
	REQ_HOST,
	REQ_RANGE,
	REQ_MOD,
	NUM_REQ_FIELDS,
};

extern const char *req_field_str[];

enum req_method {
	M_GET,
	M_HEAD,
	M_POST,
	NUM_REQ_METHODS,
};

extern const char *req_method_str[];

struct request {
	enum req_method method;
	char target[PATH_MAX];
	char field[NUM_REQ_FIELDS][FIELD_MAX];
	char body[PATH_MAX];
};

enum status {
	S_OK                    = 200,
	S_NO_CONTENT            = 204,
	S_PARTIAL_CONTENT       = 206,
	S_MOVED_PERMANENTLY     = 301,
	S_NOT_MODIFIED          = 304,
	S_BAD_REQUEST           = 400,
	S_FORBIDDEN             = 403,
	S_NOT_FOUND             = 404,
	S_METHOD_NOT_ALLOWED    = 405,
	S_REQUEST_TIMEOUT       = 408,
	S_RANGE_NOT_SATISFIABLE = 416,
	S_REQUEST_TOO_LARGE     = 431,
	S_INTERNAL_SERVER_ERROR = 500,
	S_VERSION_NOT_SUPPORTED = 505,
};

extern const char *status_str[];

enum status http_send_status(int, enum status);
int http_get_request(int, struct request *);
enum status http_send_response(int, struct request *);

#endif /* HTTP_H */
