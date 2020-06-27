/* See LICENSE file for copyright and license details. */
#ifndef RESP_H
#define RESP_H

#include <sys/stat.h>
#include <sys/types.h>

#include "http.h"

enum status resp_exec(int, char *, struct request *, struct stat *);
enum status resp_dir(int, char *, struct request *);
enum status resp_file(int, char *, struct request *, struct stat *, char *,
                      off_t, off_t);

#endif /* RESP_H */
