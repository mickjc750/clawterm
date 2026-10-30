#ifndef CLAWTERM_SERIAL_H
#define CLAWTERM_SERIAL_H

#include <stddef.h>

/* Open a serial port at 8N1, raw mode, no flow control, non-blocking.
 * Any integer baud rate is accepted (uses termios2/BOTHER).
 * Returns fd, or -1 with a message in err. */
int serial_open(const char *path, int baud, char *err, size_t errlen);

/* Change the baud rate of an open port. Returns 0 on success. */
int serial_set_baud(int fd, int baud);

/* Bytes still waiting in the kernel's transmit queue, or 0 if unknown. */
int serial_outq(int fd);

/* Returns 1 if the port looks like real hardware (filters the phantom
 * /dev/ttyS* entries the 8250 driver always creates). */
int serial_port_present(const char *path);

#endif
