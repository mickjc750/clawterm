# clawterm

A small ncurses serial terminal for embedded work on Linux.

> **Authorship:** this project was written by Claude Opus 5.5, Anthropic's AI
> model, working from requirements and testing by
> [mickjc750](https://github.com/mickjc750).

* Picks serial ports from a list (USB serial, ACM, onboard UARTs), labelled with
  the device name from `/dev/serial/by-id`.
* Any baud rate, including 14400 and other non-standard rates. Always 8N1, no
  flow control.
* Logs every received line with a millisecond timestamp.
* Sends a file (such as a bootloader `.hex`) as text, and sends it again with
  one key. The file is re-read on every send, so a fresh build is picked up.
* Remembers the port, baud rate and file between runs.
* Reconnects by itself if a USB serial adapter is unplugged or the device
  re-enumerates.

## Build

Needs gcc and the ncurses development package (`libncurses-dev` on
Debian/Ubuntu, `ncurses-devel` on Fedora).

    make
    sudo make install      # optional, installs to /usr/local/bin

Your user needs access to the serial device, usually by being in the
`dialout` group (`sudo usermod -aG dialout $USER`, then log in again).

## Run

    clawterm                          # uses the last port, baud and file
    clawterm -p /dev/ttyUSB0 -b 14400 -f build/firmware.hex

Options: `-p port`, `-b baud`, `-f file`, `-l logfile`, `-n` (no log).

If no port has been chosen yet, the port list opens on start-up.

## Keys

| Key | Ctrl-A then | Action |
|-----|-------------|--------|
| F1  | h | Help |
| F2  | p | Choose serial port |
| F3  | b | Choose baud rate ("Other..." for any rate) |
| F4  | f | Choose the file to send |
| F5  | s | Send the file (press again to cancel a send in progress) |
| F6  | t | Show or hide timestamps on screen |
| F7  | c | Clear the screen |
| F8  | e | Choose what Enter sends: CR, LF or CRLF |
| F10 | q | Quit |
| PgUp / PgDn / End | | Scroll back through the last 10000 lines |
| | Ctrl-A | Send a literal Ctrl-A |

Everything else you type goes straight to the device. Arrow keys are sent as
ANSI escape sequences and Backspace as 0x08.

If your terminal emulator grabs some function keys (F10 and F11 often are),
use the Ctrl-A versions.

## Log file

By default a new log is created in the current directory, named
`clawterm-YYYYMMDD-HHMMSS.log`. Each received line looks like

    [2026-10-26 05:14:06.689] debug line 3	value=9

The timestamp is taken when the first byte of the line arrives. CR, LF and
CRLF all end a line. clawterm's own events (connect, disconnect, file sends)
are logged too, marked with `#`:

    [2026-10-26 05:14:35.292] # sending /home/me/fw/build/firmware.hex (4092 bytes)

On USB serial adapters clawterm asks the driver for low-latency mode (FTDI
otherwise buffers for up to 16 ms), so timestamps are usually accurate to a
few milliseconds.

## Settings

Saved in `~/.config/clawterm/config` as plain `key=value` lines: `port`, `baud`,
`file`, `eol` and `timestamps`.
