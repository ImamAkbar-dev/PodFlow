#include "io_utils.h"

#include <errno.h>
#include <unistd.h>

ssize_t readn(int fd, void *buf, size_t n)
{
    char *p = buf;          /* cursor: next free byte in the caller's buffer */
    size_t left = n;        /* bytes still missing                           */

    while (left > 0) {
        ssize_t r = read(fd, p, left);

        if (r < 0) {
            if (errno == EINTR)
                continue;   /* interrupted by a signal: just try again */
            return -1;      /* real error: errno already set by read() */
        }
        if (r == 0)
            break;          /* EOF: peer closed the connection */

        p += r;             /* advance cursor by what we actually got */
        left -= (size_t)r;
    }
    return (ssize_t)(n - left);   /* bytes actually read (== n on success) */
}

ssize_t writen(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;

    while (left > 0) {
        ssize_t w = write(fd, p, left);

        if (w < 0) {
            if (errno == EINTR)
                continue;   /* interrupted: retry the same write */
            return -1;      /* EPIPE, ECONNRESET, ... */
        }
        if (w == 0) {       /* should not happen for left > 0; avoid infinite loop */
            errno = EIO;
            return -1;
        }

        p += w;             /* skip the part already sent */
        left -= (size_t)w;
    }
    return (ssize_t)n;
}