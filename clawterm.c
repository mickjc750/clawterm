/* clawterm - a small ncurses serial terminal for embedded work.
 *
 * Fixed 8N1, any baud rate, timestamped receive log, and one-key
 * (re)transmission of a text file such as a bootloader .hex image. */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "serial.h"

#define VERSION    "1.0"
#define MAX_LINES  10000   /* scrollback lines kept in memory */
#define MAX_LINE   4096    /* longer received lines are split */
#define RETRY_MS   1000    /* reconnect interval */
#define CTRL_A     0x01

enum { EOL_CR, EOL_LF, EOL_CRLF, EOL_COUNT };
static const char *eol_names[] = { "CR", "LF", "CRLF" };
static const char *eol_seqs[]  = { "\r", "\n", "\r\n" };

static const int baud_list[] = {
	300, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600,
	115200, 230400, 460800, 500000, 921600, 1000000, 2000000,
};
#define BAUD_COUNT ((int)(sizeof(baud_list) / sizeof(baud_list[0])))

enum { C_STATUS = 1, C_HELP, C_TS, C_SYS, C_MENU, C_MENUSEL };

struct line {
	struct timespec ts;
	char *s;
	int len, cap;
	int sys;            /* clawterm message rather than received data */
};

static struct {
	char port[PATH_MAX];
	int baud;
	char file[PATH_MAX];
	int eol;
	int show_ts;
} cfg = { "", 115200, "", EOL_CR, 0 };

static char cfg_path[PATH_MAX];
static char log_path[PATH_MAX];
static FILE *logf;

static int fd = -1;
static struct timespec last_retry;
static char last_err[256];

static struct line ring[MAX_LINES];
static int rstart, rcount;
static int line_open;       /* last ring entry is still receiving */
static int pending_cr;
static int scroll_off;      /* logical lines scrolled back from the bottom */
static int dirty = 1;

static unsigned char outq[4096];   /* typed keys */
static size_t outq_len;
static unsigned char *txbuf;       /* file being sent */
static size_t txlen, txoff;
static struct timespec tx_start;

static long rx_bytes, tx_bytes;
static int ctrl_a_pending;
static int quit;
static volatile sig_atomic_t got_winch;

/* ------------------------------------------------------------------ */
/* time helpers                                                         */

static void now(struct timespec *ts)
{
	clock_gettime(CLOCK_REALTIME, ts);
}

static long ms_since(const struct timespec *t)
{
	struct timespec n;

	clock_gettime(CLOCK_REALTIME, &n);
	return (n.tv_sec - t->tv_sec) * 1000 + (n.tv_nsec - t->tv_nsec) / 1000000;
}

static void fmt_ts(const struct timespec *ts, char *buf, size_t len, int date)
{
	struct tm tm;
	char tmp[32];

	localtime_r(&ts->tv_sec, &tm);
	strftime(tmp, sizeof(tmp), date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &tm);
	snprintf(buf, len, "%s.%03ld", tmp, ts->tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* config                                                               */

static void config_load(void)
{
	char buf[PATH_MAX + 32];
	FILE *f = fopen(cfg_path, "r");

	if (!f)
		return;
	while (fgets(buf, sizeof(buf), f)) {
		char *eq = strchr(buf, '='), *v;

		if (!eq)
			continue;
		*eq = 0;
		v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(buf, "port"))
			snprintf(cfg.port, sizeof(cfg.port), "%s", v);
		else if (!strcmp(buf, "baud") && atoi(v) > 0)
			cfg.baud = atoi(v);
		else if (!strcmp(buf, "file"))
			snprintf(cfg.file, sizeof(cfg.file), "%s", v);
		else if (!strcmp(buf, "eol") && atoi(v) >= 0 && atoi(v) < EOL_COUNT)
			cfg.eol = atoi(v);
		else if (!strcmp(buf, "timestamps"))
			cfg.show_ts = atoi(v) != 0;
	}
	fclose(f);
}

static void config_save(void)
{
	char dir[PATH_MAX];
	char *slash;
	FILE *f;

	snprintf(dir, sizeof(dir), "%s", cfg_path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		slash = strrchr(dir, '/');
		if (slash) {      /* ~/.config may not exist yet either */
			*slash = 0;
			mkdir(dir, 0755);
			*slash = '/';
		}
		mkdir(dir, 0755);
	}
	f = fopen(cfg_path, "w");
	if (!f)
		return;
	fprintf(f, "port=%s\nbaud=%d\nfile=%s\neol=%d\ntimestamps=%d\n",
		cfg.port, cfg.baud, cfg.file, cfg.eol, cfg.show_ts);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* scrollback and log                                                   */

static struct line *ring_at(int i)
{
	return &ring[(rstart + i) % MAX_LINES];
}

static struct line *ring_push(void)
{
	struct line *l;

	if (rcount == MAX_LINES) {
		free(ring[rstart].s);
		rstart = (rstart + 1) % MAX_LINES;
		rcount--;
	}
	l = ring_at(rcount++);
	memset(l, 0, sizeof(*l));
	now(&l->ts);
	if (scroll_off > 0 && scroll_off < rcount - 1)
		scroll_off++;
	return l;
}

static void line_add(struct line *l, const char *s, int n)
{
	if (l->len + n + 1 > l->cap) {
		l->cap = (l->len + n + 1) * 2;
		l->s = realloc(l->s, l->cap);
	}
	memcpy(l->s + l->len, s, n);
	l->len += n;
	l->s[l->len] = 0;
}

static void ring_clear(void)
{
	int i;

	for (i = 0; i < rcount; i++)
		free(ring_at(i)->s);
	rstart = rcount = 0;
	line_open = 0;
	scroll_off = 0;
	dirty = 1;
}

static void log_line(const struct line *l)
{
	char ts[40];

	if (!logf)
		return;
	fmt_ts(&l->ts, ts, sizeof(ts), 1);
	fprintf(logf, "[%s] %s%.*s\n", ts, l->sys ? "# " : "", l->len, l->s ? l->s : "");
	fflush(logf);
}

/* A clawterm message: shown in yellow, logged with a '#' marker. It is
 * placed before any half-received line so that line stays intact. */
static void sys_msg(const char *fmt, ...)
{
	char buf[512];
	struct line *l, tmp;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	l = ring_push();
	l->sys = 1;
	line_add(l, buf, strlen(buf));
	log_line(l);
	if (line_open && rcount >= 2) {
		tmp = *ring_at(rcount - 2);
		*ring_at(rcount - 2) = *l;
		*l = tmp;
	}
	dirty = 1;
}

static void rx_end_line(void)
{
	if (!line_open)
		return;
	log_line(ring_at(rcount - 1));
	line_open = 0;
}

static void rx_byte(unsigned char c)
{
	struct line *l;
	char ch;

	if (pending_cr) {
		pending_cr = 0;
		if (c == '\n')
			return;   /* CRLF already ended the line */
	}
	if (c == '\r') {
		/* CR alone also ends a line, for devices that send CR only */
		if (!line_open)
			ring_push();
		line_open = 1;
		rx_end_line();
		pending_cr = 1;
		return;
	}
	if (c == '\n') {
		if (!line_open)
			ring_push();
		line_open = 1;
		rx_end_line();
		return;
	}

	if (!line_open) {
		ring_push();
		line_open = 1;
	}
	l = ring_at(rcount - 1);
	if (c == '\b' || c == 0x7f) {
		if (l->len > 0)
			l->s[--l->len] = 0;
		return;
	}
	ch = (char)c;
	line_add(l, &ch, 1);
	if (l->len >= MAX_LINE)
		rx_end_line();
}

/* ------------------------------------------------------------------ */
/* serial port                                                          */

static void port_close(const char *why)
{
	if (fd < 0)
		return;
	close(fd);
	fd = -1;
	if (txbuf) {
		free(txbuf);
		txbuf = NULL;
		sys_msg("file send aborted");
	}
	outq_len = 0;
	sys_msg("%s %s", cfg.port, why);
	now(&last_retry);
}

static void port_open(int verbose)
{
	char err[256];

	if (fd >= 0 || !cfg.port[0])
		return;
	now(&last_retry);
	fd = serial_open(cfg.port, cfg.baud, err, sizeof(err));
	if (fd < 0) {
		/* only report a new error, retries happen quietly */
		if (verbose || strcmp(err, last_err))
			sys_msg("%s (retrying)", err);
		snprintf(last_err, sizeof(last_err), "%s", err);
		return;
	}
	last_err[0] = 0;
	sys_msg("connected to %s at %d 8N1", cfg.port, cfg.baud);
}

static void port_switch(const char *path)
{
	if (fd >= 0)
		port_close("closed");
	snprintf(cfg.port, sizeof(cfg.port), "%s", path);
	config_save();
	last_err[0] = 0;
	port_open(1);
}

static void set_baud(int baud)
{
	cfg.baud = baud;
	config_save();
	if (fd >= 0) {
		if (serial_set_baud(fd, baud) == 0)
			sys_msg("baud rate set to %d", baud);
		else
			sys_msg("cannot set %d baud: %s", baud, strerror(errno));
	}
	dirty = 1;
}

static size_t tx_done_bytes(void);

static void queue_out(const char *s, size_t n)
{
	if (fd < 0)
		return;
	if (outq_len + n > sizeof(outq))
		n = sizeof(outq) - outq_len;
	memcpy(outq + outq_len, s, n);
	outq_len += n;
}

static void send_file(void)
{
	FILE *f;
	long size;

	if (txbuf) {
		free(txbuf);
		txbuf = NULL;
		if (fd >= 0)
			tcflush(fd, TCOFLUSH);
		sys_msg("file send cancelled at %zu of %zu bytes", tx_done_bytes(), txlen);
		return;
	}
	if (!cfg.file[0]) {
		sys_msg("no file chosen yet, press F4");
		return;
	}
	if (fd < 0) {
		sys_msg("not connected");
		return;
	}
	/* read it fresh every time so a rebuilt file is picked up */
	f = fopen(cfg.file, "rb");
	if (!f) {
		sys_msg("%s: %s", cfg.file, strerror(errno));
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		sys_msg("%s is empty", cfg.file);
		return;
	}
	txbuf = malloc(size);
	txlen = fread(txbuf, 1, size, f);
	fclose(f);
	txoff = 0;
	now(&tx_start);
	sys_msg("sending %s (%zu bytes)", cfg.file, txlen);
}

static void service_write(void)
{
	ssize_t n;

	if (fd < 0)
		return;
	if (outq_len) {
		n = write(fd, outq, outq_len);
		if (n > 0) {
			memmove(outq, outq + n, outq_len - n);
			outq_len -= n;
			tx_bytes += n;
		}
		if (outq_len)
			return;
	}
	if (txbuf && txoff < txlen) {
		n = write(fd, txbuf + txoff, txlen - txoff);
		if (n > 0) {
			txoff += n;
			tx_bytes += n;
			dirty = 1;
		} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
			port_close("write error");
			return;
		}
	}
}

/* A send is only finished once the UART has emptied the kernel queue. */
static size_t tx_done_bytes(void)
{
	size_t q = fd >= 0 ? (size_t)serial_outq(fd) : 0;

	return q > txoff ? 0 : txoff - q;
}

static void check_tx_done(void)
{
	if (!txbuf || txoff < txlen || tx_done_bytes() < txlen)
		return;
	free(txbuf);
	txbuf = NULL;
	sys_msg("sent %zu bytes in %.2f s", txlen, ms_since(&tx_start) / 1000.0);
}

static void service_read(void)
{
	unsigned char buf[4096];
	ssize_t n, i;

	n = read(fd, buf, sizeof(buf));
	if (n > 0) {
		for (i = 0; i < n; i++)
			rx_byte(buf[i]);
		rx_bytes += n;
		dirty = 1;
	} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
		port_close("disconnected");
	}
}

/* Wait up to timeout_ms for keyboard input while servicing the port.
 * Returns 1 if a key is ready. Everything modal calls this so data keeps
 * flowing to the log while menus are open. */
static int pump(int timeout_ms)
{
	struct pollfd p[2];
	int np = 1;

	p[0].fd = STDIN_FILENO;
	p[0].events = POLLIN;
	if (fd >= 0) {
		p[1].fd = fd;
		p[1].events = POLLIN | ((outq_len || (txbuf && txoff < txlen)) ? POLLOUT : 0);
		np = 2;
	}
	if (poll(p, np, timeout_ms) < 0)
		return 0;
	if (np == 2) {
		if (p[1].revents & (POLLERR | POLLHUP | POLLNVAL))
			port_close("disconnected");
		else {
			if (p[1].revents & POLLIN)
				service_read();
			if (fd >= 0 && (p[1].revents & POLLOUT))
				service_write();
		}
	}
	if (txbuf) {
		check_tx_done();
		dirty = 1;
	}
	if (fd < 0 && cfg.port[0] && ms_since(&last_retry) >= RETRY_MS)
		port_open(0);
	return (p[0].revents & POLLIN) != 0 || got_winch;
}

/* ------------------------------------------------------------------ */
/* drawing                                                              */

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static void draw_status(void)
{
	char buf[1024], right[128];
	int w = COLS;

	if (txbuf)
		snprintf(right, sizeof(right), " TX %3zu%% ", tx_done_bytes() * 100 / txlen);
	else
		snprintf(right, sizeof(right), " RX %ld  TX %ld ", rx_bytes, tx_bytes);

	snprintf(buf, sizeof(buf), " clawterm | %s @ %d 8N1 %s| Enter=%s | file: %s | log: %s",
		 cfg.port[0] ? cfg.port : "(no port)", cfg.baud,
		 fd >= 0 ? "" : "[offline] ", eol_names[cfg.eol],
		 cfg.file[0] ? base_name(cfg.file) : "(none)",
		 logf ? base_name(log_path) : "(off)");

	attron(COLOR_PAIR(C_STATUS) | A_BOLD);
	mvhline(0, 0, ' ', w);
	mvaddnstr(0, 0, buf, w);
	if ((int)strlen(right) < w)
		mvaddstr(0, w - strlen(right), right);
	attroff(COLOR_PAIR(C_STATUS) | A_BOLD);
}

static void draw_help(void)
{
	const char *help = " F2 Port  F3 Baud  F4 File  F5 Send/Cancel  F6 Time  F7 Clear"
			   "  F8 Enter  PgUp/PgDn  F10 Quit  (or Ctrl-A + p b f s t c e q h)";

	attron(COLOR_PAIR(C_HELP));
	mvhline(LINES - 1, 0, ' ', COLS);
	mvaddnstr(LINES - 1, 0, help, COLS);
	if (scroll_off > 0) {
		char buf[48];

		snprintf(buf, sizeof(buf), " SCROLLED -%d (End) ", scroll_off);
		mvaddstr(LINES - 1, COLS - (int)strlen(buf), buf);
	}
	attroff(COLOR_PAIR(C_HELP));
}

/* Lay out one line into display text; returns the length and sets
 * *prefix to the number of leading timestamp characters. */
static int line_text(const struct line *l, char *out, int cap, int *prefix)
{
	char ts[40];
	int n = 0, i;

	*prefix = 0;
	if (cfg.show_ts) {
		fmt_ts(&l->ts, ts, sizeof(ts), 0);
		n = snprintf(out, cap, "%s ", ts);
		*prefix = n;
	}
	for (i = 0; i < l->len && n < cap - 1; i++) {
		unsigned char c = l->s[i];

		if (c == '\t') {
			do
				out[n++] = ' ';
			while ((n - *prefix) % 8 && n < cap - 1);
		} else {
			out[n++] = (c >= 0x20 && c < 0x7f) ? c : '.';
		}
	}
	out[n] = 0;
	return n;
}

static void draw_rx(void)
{
	static char text[MAX_LINE * 2 + 64];
	int top = 1, rows = LINES - 2, w = COLS;
	int row = top + rows, idx, max_scroll;

	if (w < 1 || rows < 1)
		return;
	max_scroll = rcount > 0 ? rcount - 1 : 0;
	if (scroll_off > max_scroll)
		scroll_off = max_scroll;

	for (idx = rcount - 1 - scroll_off; idx >= 0 && row > top; idx--) {
		struct line *l = ring_at(idx);
		int prefix, len = line_text(l, text, sizeof(text), &prefix);
		int nrows = len ? (len + w - 1) / w : 1, r;

		for (r = nrows - 1; r >= 0 && row > top; r--) {
			int start = r * w, n = len - start;

			row--;
			move(row, 0);
			clrtoeol();
			if (n > w)
				n = w;
			if (n <= 0)
				continue;
			if (l->sys) {
				attron(COLOR_PAIR(C_SYS) | A_BOLD);
				addnstr(text + start, n);
				attroff(COLOR_PAIR(C_SYS) | A_BOLD);
			} else if (start < prefix) {
				int p = prefix - start < n ? prefix - start : n;

				attron(COLOR_PAIR(C_TS));
				addnstr(text + start, p);
				attroff(COLOR_PAIR(C_TS));
				addnstr(text + start + p, n - p);
			} else {
				addnstr(text + start, n);
			}
		}
	}
	while (row > top) {
		move(--row, 0);
		clrtoeol();
	}
}

static void draw_main(void)
{
	if (got_winch) {
		got_winch = 0;
		endwin();
		refresh();
		clear();
	}
	draw_rx();
	draw_status();
	draw_help();
	wnoutrefresh(stdscr);
	dirty = 0;
}

/* ------------------------------------------------------------------ */
/* modal dialogs                                                        */

static int read_key(void)
{
	int ch = getch();

	if (ch == KEY_RESIZE)
		got_winch = 1;
	return ch;
}

/* Generic list picker. Returns the chosen index or -1 on Esc. */
static int pick(const char *title, char **items, int n, int sel)
{
	int top = 0, ch, i, w = (int)strlen(title) + 4, h;
	WINDOW *win = NULL;

	for (i = 0; i < n; i++)
		if ((int)strlen(items[i]) + 4 > w)
			w = strlen(items[i]) + 4;
	if (sel < 0 || sel >= n)
		sel = 0;

	for (;;) {
		int ww = w > COLS - 2 ? COLS - 2 : w;
		int vis;

		h = n + 2 > LINES - 4 ? LINES - 4 : n + 2;
		vis = h - 2;
		if (vis < 1)
			vis = 1;
		if (sel < top)
			top = sel;
		if (sel >= top + vis)
			top = sel - vis + 1;

		draw_main();
		if (win)
			delwin(win);
		win = newwin(h, ww, (LINES - h) / 2, (COLS - ww) / 2);
		wbkgd(win, COLOR_PAIR(C_MENU));
		box(win, 0, 0);
		mvwaddnstr(win, 0, 2, title, ww - 4);
		for (i = 0; i < vis && top + i < n; i++) {
			if (top + i == sel)
				wattron(win, COLOR_PAIR(C_MENUSEL) | A_BOLD);
			mvwhline(win, i + 1, 1, ' ', ww - 2);
			mvwaddnstr(win, i + 1, 2, items[top + i], ww - 4);
			if (top + i == sel)
				wattroff(win, COLOR_PAIR(C_MENUSEL) | A_BOLD);
		}
		wnoutrefresh(win);
		doupdate();

		while (!pump(50))
			if (dirty) {
				draw_main();
				touchwin(win);
				wnoutrefresh(win);
				doupdate();
			}

		ch = read_key();
		switch (ch) {
		case KEY_UP:    if (sel > 0) sel--; break;
		case KEY_DOWN:  if (sel < n - 1) sel++; break;
		case KEY_PPAGE: sel = sel > vis ? sel - vis : 0; break;
		case KEY_NPAGE: sel = sel + vis < n ? sel + vis : n - 1; break;
		case KEY_HOME:  sel = 0; break;
		case KEY_END:   sel = n - 1; break;
		case '\n': case '\r': case KEY_ENTER:
			delwin(win);
			dirty = 1;
			return sel;
		case 27: case KEY_F(10): case 'q':
			delwin(win);
			dirty = 1;
			return -1;
		}
	}
}

/* One-line text prompt. Returns 1 if confirmed. */
static int prompt(const char *title, char *buf, size_t cap)
{
	size_t len = strlen(buf);
	WINDOW *win = NULL;
	int ch;

	for (;;) {
		int ww = COLS - 4 > 70 ? 70 : COLS - 4;
		size_t off = len > (size_t)ww - 5 ? len - (ww - 5) : 0;

		draw_main();
		if (win)
			delwin(win);
		win = newwin(3, ww, LINES / 2 - 1, (COLS - ww) / 2);
		wbkgd(win, COLOR_PAIR(C_MENU));
		box(win, 0, 0);
		mvwaddnstr(win, 0, 2, title, ww - 4);
		mvwaddnstr(win, 1, 2, buf + off, ww - 4);
		curs_set(1);
		wnoutrefresh(win);
		doupdate();

		while (!pump(50))
			if (dirty) {
				draw_main();
				touchwin(win);
				wnoutrefresh(win);
				doupdate();
			}

		ch = read_key();
		if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == 27) {
			delwin(win);
			curs_set(0);
			dirty = 1;
			return ch != 27;
		} else if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && len > 0) {
			buf[--len] = 0;
		} else if (ch >= 0x20 && ch < 0x7f && len + 1 < cap) {
			buf[len++] = ch;
			buf[len] = 0;
		}
	}
}

static void choose_port(void)
{
	static const char *patterns[] = {
		"/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyAMA*", "/dev/ttyS*", "/dev/rfcomm*",
	};
	char *items[256], *paths[256];
	char label[PATH_MAX + 256];
	int n = 0, sel = 0, i, j, k;

	for (i = 0; i < (int)(sizeof(patterns) / sizeof(patterns[0])); i++) {
		glob_t g;

		if (glob(patterns[i], 0, NULL, &g) != 0)
			continue;
		for (j = 0; j < (int)g.gl_pathc && n < 254; j++) {
			const char *p = g.gl_pathv[j];
			char desc[PATH_MAX] = "";
			glob_t ids;

			if (!serial_port_present(p))
				continue;
			/* use /dev/serial/by-id to say what is plugged in */
			if (glob("/dev/serial/by-id/*", 0, NULL, &ids) == 0) {
				for (k = 0; k < (int)ids.gl_pathc; k++) {
					char real[PATH_MAX];

					if (realpath(ids.gl_pathv[k], real) && !strcmp(real, p)) {
						snprintf(desc, sizeof(desc), "%s", base_name(ids.gl_pathv[k]));
						break;
					}
				}
				globfree(&ids);
			}
			snprintf(label, sizeof(label), "%-14s %s", p, desc);
			if (!strcmp(p, cfg.port))
				sel = n;
			paths[n] = strdup(p);
			items[n++] = strdup(label);
		}
		globfree(&g);
	}
	paths[n] = NULL;
	items[n++] = strdup("Enter a path...");

	i = pick(" Serial port ", items, n, sel);
	if (i >= 0 && paths[i]) {
		port_switch(paths[i]);
	} else if (i >= 0) {
		char buf[PATH_MAX];

		snprintf(buf, sizeof(buf), "%s", cfg.port[0] ? cfg.port : "/dev/");
		if (prompt(" Port path ", buf, sizeof(buf)) && buf[0])
			port_switch(buf);
	}
	for (j = 0; j < n; j++) {
		free(items[j]);
		free(paths[j]);
	}
}

static void choose_baud(void)
{
	char *items[BAUD_COUNT + 1];
	char buf[32];
	int i, sel = BAUD_COUNT;

	for (i = 0; i < BAUD_COUNT; i++) {
		snprintf(buf, sizeof(buf), "%d", baud_list[i]);
		items[i] = strdup(buf);
		if (baud_list[i] == cfg.baud)
			sel = i;
	}
	items[BAUD_COUNT] = strdup("Other...");

	i = pick(" Baud rate ", items, BAUD_COUNT + 1, sel);
	if (i >= 0 && i < BAUD_COUNT) {
		set_baud(baud_list[i]);
	} else if (i == BAUD_COUNT) {
		snprintf(buf, sizeof(buf), "%d", cfg.baud);
		if (prompt(" Baud rate ", buf, sizeof(buf)) && atoi(buf) > 0)
			set_baud(atoi(buf));
	}
	for (i = 0; i <= BAUD_COUNT; i++)
		free(items[i]);
}

struct entry {
	char *name;
	int dir;
};

static int entry_cmp(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->dir != y->dir)
		return y->dir - x->dir;
	return strcasecmp(x->name, y->name);
}

/* Simple file browser starting from the last file's directory. */
static void choose_file(void)
{
	char dir[PATH_MAX], path[PATH_MAX * 2], title[PATH_MAX + 16];

	if (cfg.file[0]) {
		char *s;

		snprintf(dir, sizeof(dir), "%s", cfg.file);
		s = strrchr(dir, '/');
		if (s && s != dir)
			*s = 0;
		else
			strcpy(dir, "/");
	} else if (!getcwd(dir, sizeof(dir))) {
		strcpy(dir, "/");
	}

	for (;;) {
		struct entry *ents = NULL;
		char **items;
		int n = 0, cap = 0, i, sel = 0;
		struct dirent *de;
		DIR *d = opendir(dir);

		if (!d) {
			sys_msg("%s: %s", dir, strerror(errno));
			strcpy(dir, "/");
			d = opendir(dir);
			if (!d)
				return;
		}
		while ((de = readdir(d))) {
			struct stat st;

			if (de->d_name[0] == '.')
				continue;
			snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", de->d_name);
			if (stat(path, &st) < 0)
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 64;
				ents = realloc(ents, cap * sizeof(*ents));
			}
			ents[n].name = strdup(de->d_name);
			ents[n].dir = S_ISDIR(st.st_mode);
			n++;
		}
		closedir(d);
		qsort(ents, n, sizeof(*ents), entry_cmp);

		items = malloc((n + 2) * sizeof(*items));
		items[0] = strdup("[ type a path ]");
		items[1] = strdup("../");
		for (i = 0; i < n; i++) {
			snprintf(path, sizeof(path), "%s%s", ents[i].name, ents[i].dir ? "/" : "");
			items[i + 2] = strdup(path);
			if (cfg.file[0] && !strcmp(base_name(cfg.file), ents[i].name))
				sel = i + 2;
		}
		if (!sel)
			sel = n ? 2 : 1;

		snprintf(title, sizeof(title), " %s ", dir);
		i = pick(title, items, n + 2, sel);

		if (i == 0) {
			snprintf(path, sizeof(path), "%s", cfg.file[0] ? cfg.file : dir);
			if (prompt(" File to send ", path, sizeof(path)) && path[0]) {
				snprintf(cfg.file, sizeof(cfg.file), "%s", path);
				config_save();
				i = -2;
			}
		} else if (i == 1) {
			char *s = strrchr(dir, '/');

			if (s && s != dir)
				*s = 0;
			else
				strcpy(dir, "/");
		} else if (i >= 2 && ents[i - 2].dir) {
			snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", ents[i - 2].name);
			snprintf(dir, sizeof(dir), "%s", path);
		} else if (i >= 2) {
			snprintf(path, sizeof(path), "%s/%s", strcmp(dir, "/") ? dir : "", ents[i - 2].name);
			snprintf(cfg.file, sizeof(cfg.file), "%s", path);
			config_save();
			i = -2;
		}

		for (int k = 0; k < n; k++)
			free(ents[k].name);
		for (int k = 0; k < n + 2; k++)
			free(items[k]);
		free(ents);
		free(items);

		if (i == -2) {
			sys_msg("file set to %s, press F5 to send", cfg.file);
			return;
		}
		if (i == -1)
			return;
	}
}

static void show_help(void)
{
	char *items[] = {
		"F2  / Ctrl-A p   choose serial port",
		"F3  / Ctrl-A b   choose baud rate",
		"F4  / Ctrl-A f   choose file to send",
		"F5  / Ctrl-A s   send file again (or cancel a send)",
		"F6  / Ctrl-A t   show/hide timestamps on screen",
		"F7  / Ctrl-A c   clear screen",
		"F8  / Ctrl-A e   Enter sends CR / LF / CRLF",
		"PgUp PgDn End    scroll back through received lines",
		"F10 / Ctrl-A q   quit",
		"Ctrl-A Ctrl-A    send a literal Ctrl-A",
		"",
		"Received lines are always logged with ms timestamps.",
	};

	pick(" clawterm " VERSION " help (Esc to close) ", items, sizeof(items) / sizeof(items[0]), 0);
}

/* ------------------------------------------------------------------ */
/* keyboard                                                             */

static void command(int ch)
{
	switch (ch) {
	case 'p': choose_port(); break;
	case 'b': choose_baud(); break;
	case 'f': choose_file(); break;
	case 's': send_file(); break;
	case 't':
		cfg.show_ts = !cfg.show_ts;
		config_save();
		break;
	case 'c': ring_clear(); break;
	case 'e':
		cfg.eol = (cfg.eol + 1) % EOL_COUNT;
		config_save();
		break;
	case 'h': show_help(); break;
	case 'q': quit = 1; break;
	case CTRL_A: queue_out("\x01", 1); break;
	}
	dirty = 1;
}

static void handle_key(int ch)
{
	int page = LINES - 3 > 1 ? LINES - 3 : 1;

	if (ctrl_a_pending) {
		ctrl_a_pending = 0;
		command(ch == CTRL_A ? CTRL_A : tolower(ch));
		return;
	}

	switch (ch) {
	case CTRL_A:     ctrl_a_pending = 1; return;
	case KEY_F(1):   command('h'); return;
	case KEY_F(2):   command('p'); return;
	case KEY_F(3):   command('b'); return;
	case KEY_F(4):   command('f'); return;
	case KEY_F(5):   command('s'); return;
	case KEY_F(6):   command('t'); return;
	case KEY_F(7):   command('c'); return;
	case KEY_F(8):   command('e'); return;
	case KEY_F(10):  command('q'); return;
	case KEY_PPAGE:  scroll_off += page; dirty = 1; return;
	case KEY_NPAGE:  scroll_off = scroll_off > page ? scroll_off - page : 0; dirty = 1; return;
	case KEY_END:    scroll_off = 0; dirty = 1; return;
	case KEY_RESIZE: got_winch = 1; dirty = 1; return;
	case '\n': case '\r': case KEY_ENTER:
		queue_out(eol_seqs[cfg.eol], strlen(eol_seqs[cfg.eol]));
		return;
	case KEY_BACKSPACE: queue_out("\b", 1); return;
	case KEY_DC:     queue_out("\x1b[3~", 4); return;
	case KEY_UP:     queue_out("\x1b[A", 3); return;
	case KEY_DOWN:   queue_out("\x1b[B", 3); return;
	case KEY_RIGHT:  queue_out("\x1b[C", 3); return;
	case KEY_LEFT:   queue_out("\x1b[D", 3); return;
	}
	if (ch >= 0 && ch < 256) {
		char c = ch;

		queue_out(&c, 1);
	}
}

/* ------------------------------------------------------------------ */

static void on_winch(int sig)
{
	(void)sig;
	got_winch = 1;
}

static void usage(void)
{
	printf("clawterm " VERSION " - serial terminal for embedded work (8N1)\n\n"
	       "usage: clawterm [-p port] [-b baud] [-f file] [-l logfile]\n\n"
	       "  -p port     serial device, e.g. /dev/ttyUSB0\n"
	       "  -b baud     any rate, e.g. 14400\n"
	       "  -f file     file to send with F5\n"
	       "  -l logfile  log path (default ./clawterm-YYYYMMDD-HHMMSS.log)\n"
	       "  -n          no log file\n\n"
	       "Port, baud and file are remembered in ~/.config/clawterm/config.\n");
}

int main(int argc, char **argv)
{
	const char *home = getenv("HOME");
	int opt, nolog = 0, ch;

	setlocale(LC_ALL, "");
	snprintf(cfg_path, sizeof(cfg_path), "%s/.config/clawterm/config", home ? home : ".");
	config_load();

	while ((opt = getopt(argc, argv, "p:b:f:l:nh")) != -1) {
		switch (opt) {
		case 'p': snprintf(cfg.port, sizeof(cfg.port), "%s", optarg); break;
		case 'b':
			if (atoi(optarg) <= 0) {
				fprintf(stderr, "bad baud rate: %s\n", optarg);
				return 1;
			}
			cfg.baud = atoi(optarg);
			break;
		case 'f':
			if (!realpath(optarg, cfg.file))
				snprintf(cfg.file, sizeof(cfg.file), "%s", optarg);
			break;
		case 'l': snprintf(log_path, sizeof(log_path), "%s", optarg); break;
		case 'n': nolog = 1; break;
		default:  usage(); return opt == 'h' ? 0 : 1;
		}
	}

	if (!nolog) {
		if (!log_path[0]) {
			struct timespec ts;
			struct tm tm;

			now(&ts);
			localtime_r(&ts.tv_sec, &tm);
			strftime(log_path, sizeof(log_path), "clawterm-%Y%m%d-%H%M%S.log", &tm);
		}
		logf = fopen(log_path, "a");
		if (!logf) {
			fprintf(stderr, "cannot open log %s: %s\n", log_path, strerror(errno));
			return 1;
		}
	}
	config_save();

	signal(SIGWINCH, on_winch);
	signal(SIGPIPE, SIG_IGN);
	set_escdelay(25);
	initscr();
	raw();
	noecho();
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);
	curs_set(0);
	if (has_colors()) {
		start_color();
		use_default_colors();
		init_pair(C_STATUS, COLOR_WHITE, COLOR_BLUE);
		init_pair(C_HELP, COLOR_BLACK, COLOR_CYAN);
		init_pair(C_TS, COLOR_GREEN, -1);
		init_pair(C_SYS, COLOR_YELLOW, -1);
		init_pair(C_MENU, COLOR_WHITE, COLOR_BLUE);
		init_pair(C_MENUSEL, COLOR_BLACK, COLOR_CYAN);
	}

	sys_msg("clawterm " VERSION " - F1 or Ctrl-A h for help");
	if (logf)
		sys_msg("logging to %s", log_path);
	if (cfg.port[0])
		port_open(1);
	draw_main();
	doupdate();
	if (!cfg.port[0])
		choose_port();

	while (!quit) {
		if (pump(50)) {
			while ((ch = read_key()) != ERR) {
				handle_key(ch);
				if (quit)
					break;
			}
		}
		if (dirty || got_winch) {
			draw_main();
			doupdate();
		}
	}

	endwin();
	rx_end_line();
	if (fd >= 0)
		close(fd);
	if (logf)
		fclose(logf);
	if (log_path[0] && !nolog)
		printf("log saved to %s\n", log_path);
	return 0;
}
