#ifndef IO_UTILS_H
#define IO_UTILS_H

#include <stddef.h>     /* size_t  */
#include <sys/types.h>  /* ssize_t */

/*
 * Read exactly n bytes from fd.
 * Returns: n          on success,
 *          0 <= r < n if EOF was reached after r bytes (peer closed),
 *          -1         on I/O error (errno is set).
 * Retries automatically when interrupted by a signal (EINTR).
 */
ssize_t readn(int fd, void *buf, size_t n);

/*
 * Write exactly n bytes to fd.
 * Returns: n   on success,
 *          -1  on error (errno is set).
 * Retries automatically on short writes and EINTR.
 */
ssize_t writen(int fd, const void *buf, size_t n);

#endif /* IO_UTILS_H */