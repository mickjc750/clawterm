/* Serial port setup using termios2 so that non-standard rates such as
 * 14400 work. Kept in its own file because <asm/termbits.h> clashes with
 * glibc's <termios.h>. */

#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <linux/serial.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "serial.h"

int ioctl(int fd, unsigned long request, ...);

static int apply_termios(int fd, int baud)
{
	struct termios2 t;

	if (ioctl(fd, TCGETS2, &t) < 0)
		return -1;

	t.c_cflag &= ~(CBAUD | (CBAUD << IBSHIFT) | CSIZE | PARENB | CSTOPB | CRTSCTS);
	t.c_cflag |= BOTHER | CS8 | CREAD | CLOCAL;
	t.c_ispeed = baud;
	t.c_ospeed = baud;
	t.c_iflag = 0;
	t.c_oflag = 0;
	t.c_lflag = 0;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;

	return ioctl(fd, TCSETS2, &t);
}

int serial_open(const char *path, int baud, char *err, size_t errlen)
{
	struct serial_struct ss;
	int fd;

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}

	if (ioctl(fd, TIOCEXCL) < 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}

	if (apply_termios(fd, baud) < 0) {
		snprintf(err, errlen, "%s: cannot set %d baud: %s", path, baud, strerror(errno));
		close(fd);
		return -1;
	}

	/* Ask USB serial drivers (FTDI in particular) to drop their latency
	 * timer to 1 ms, which keeps receive timestamps accurate. Optional. */
	if (ioctl(fd, TIOCGSERIAL, &ss) == 0) {
		ss.flags |= ASYNC_LOW_LATENCY;
		ioctl(fd, TIOCSSERIAL, &ss);
	}

	ioctl(fd, TCFLSH, TCIOFLUSH);
	return fd;
}

int serial_set_baud(int fd, int baud)
{
	return apply_termios(fd, baud) < 0 ? -1 : 0;
}

int serial_outq(int fd)
{
	int n = 0;

	if (ioctl(fd, TIOCOUTQ, &n) < 0)
		return 0;
	return n;
}

int serial_port_present(const char *path)
{
	struct serial_struct ss;
	int fd, present = 1;

	if (strncmp(path, "/dev/ttyS", 9) != 0)
		return 1;

	fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return errno == EACCES || errno == EBUSY;
	if (ioctl(fd, TIOCGSERIAL, &ss) == 0 && ss.type == PORT_UNKNOWN)
		present = 0;
	close(fd);
	return present;
}
