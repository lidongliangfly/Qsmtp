/** \file mmap.c
 \brief function to mmap a file
 */

#include <mmap.h>

#include <log.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <syslog.h>

/**
 * @brief map an already opened file into memory
 * @param fd file descriptor of opened file
 * @param len length of mapping will be stored here
 * @return pointer to mapped area
 * @retval NULL an error occured (errno is set) or file is empty (errno is 0)
 */
void *
mmap_fd(int fd, off_t *len)
{
	struct stat st;
	void *res;

	if (fstat(fd, &st) != 0)
		return NULL;

	*len = st.st_size;
	errno = 0;
	if (!st.st_size)
		return NULL;

	res = mmap(NULL, *len, PROT_READ, MAP_SHARED, fd, 0);

	return (res == MAP_FAILED) ? NULL : res;
}

/**
 * @brief map a file into memory
 * @param dirfd descriptor of directory
 * @param fname path to file to map relative to dirfd
 * @param len length of mapping will be stored here
 * @param fd file descriptor of opened file will be stored here
 * @return pointer to mapped area
 * @retval NULL an error occured (errno is set)
 *
 * The file is flock()'ed to allow atomic modification of this file.
 */
void *
mmap_name(int dirfd, const char *fname, off_t *len, int *fd)
{
	void *buf;

	*fd = openat(dirfd, fname, O_RDONLY | O_CLOEXEC);

	if (*fd < 0)
		return NULL;

	if (flock(*fd, LOCK_SH | LOCK_NB) != 0) {
		close(*fd);
		errno = ENOLCK;	/* not the right error code, but good enough */
		return NULL;
	}

	buf = mmap_fd(*fd, len);

	if (buf == NULL) {
		flock(*fd, LOCK_UN);
		close(*fd);
	}

	return buf;
}
