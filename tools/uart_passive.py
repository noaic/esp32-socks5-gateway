"""POSIX UART reader that leaves modem lines and queued input untouched.

The kernel/adapter may still change lines on first open. Keeping one capture
open is the reliable way to avoid repeatedly disturbing a reset-wired adapter.
"""
import array
import fcntl
import os
import select
import termios
import time


class PassiveUart:
    def __init__(self, port, timeout=0.2):
        self.port = port
        self.timeout = timeout
        self.fd = None

    def __enter__(self):
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            attrs = termios.tcgetattr(self.fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB |
                          termios.CRTSCTS | termios.HUPCL)
            attrs[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
            attrs[3] = 0
            attrs[4] = attrs[5] = termios.B115200
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 0
            # No modem-line ioctl and no tcflush: retain pending fault logs.
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        except BaseException:
            os.close(self.fd)
            self.fd = None
            raise
        return self

    def __exit__(self, *_):
        os.close(self.fd)
        self.fd = None

    @property
    def in_waiting(self):
        count = array.array('i', [0])
        fcntl.ioctl(self.fd, termios.FIONREAD, count, True)
        return count[0]

    def read(self, size):
        if not select.select([self.fd], [], [], self.timeout)[0]:
            return b''
        try:
            return os.read(self.fd, size)
        except BlockingIOError:
            return b''

    def write(self, data):
        offset = 0
        deadline = time.monotonic() + 1.0
        while offset < len(data):
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([], [self.fd], [], left)[1]:
                raise TimeoutError('UART write timed out')
            try:
                offset += os.write(self.fd, data[offset:])
            except BlockingIOError:
                continue
        return offset
