/* See LICENSE file for copyright and license details. */
#ifndef SOCK_H
#define SOCK_H

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

int sock_get_ips(const char *, const char *);
void sock_rem_uds(const char *);
int sock_get_uds(const char *);
int sock_set_timeout(int, int);
int sock_get_inaddr_str(struct sockaddr_storage *, char *, size_t);

#endif /* SOCK_H */
