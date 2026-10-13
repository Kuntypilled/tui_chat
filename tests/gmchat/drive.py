#!/usr/bin/env python3
"""Drive a curses program in a pty and read the resulting screen.

usage: drive.py <cols> <rows> <steps> -- <program> [args...]
steps are separated by ';'
  wait:SECONDS       let the program run
  send:TEXT          write text (escape sequences like \\r \\x18 are interpreted)
  expect:TEXT        wait up to 10 s for TEXT to be on screen, else report a failure
  gone:TEXT          wait up to 10 s for TEXT to leave the screen
  snap:LABEL         print the current screen
  count:TEXT         print how many screen rows contain TEXT
  rightof:N:TEXT     pass if TEXT starts at screen column N or further right
  leftof:N:TEXT      pass if TEXT starts at screen column N or further left
Prints the exit state at the end. Exit status is 1 if any expect failed.
"""
import fcntl, os, pty, re, select, signal, struct, sys, termios, time


class Screen:
    def __init__(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.grid = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0
        self.top, self.bot = 0, rows - 1
        self.saved = (0, 0)
        self.last = " "
        self.pending = b""
        self.esc = b""

    def _scroll_up(self, n=1):
        for _ in range(n):
            del self.grid[self.top]
            self.grid.insert(self.bot, [" "] * self.cols)

    def _scroll_down(self, n=1):
        for _ in range(n):
            del self.grid[self.bot]
            self.grid.insert(self.top, [" "] * self.cols)

    def _put(self, ch):
        if self.c >= self.cols:
            self.c = 0
            self._newline()
        self.grid[self.r][self.c] = ch
        self.c += 1
        self.last = ch

    def _newline(self):
        if self.r == self.bot:
            self._scroll_up()
        else:
            self.r = min(self.r + 1, self.rows - 1)

    def feed(self, data):
        data = self.pending + data
        self.pending = b""
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            if b == 0x1B:
                if i + 1 >= n:
                    self.pending = data[i:]
                    return
                nxt = data[i + 1]
                if nxt == ord("["):
                    m = re.compile(rb"\x1b\[([0-9;?<>=]*)([ -/]*)([@-~])").match(data, i)
                    if not m:
                        self.pending = data[i:]
                        return
                    self._csi(m.group(1).decode(), m.group(3).decode())
                    i = m.end()
                    continue
                if nxt in b"()*+":
                    if i + 2 >= n:
                        self.pending = data[i:]
                        return
                    i += 3
                    continue
                if nxt == ord("]"):
                    end = data.find(b"\x07", i)
                    if end < 0:
                        self.pending = data[i:]
                        return
                    i = end + 1
                    continue
                if nxt == ord("7"):
                    self.saved = (self.r, self.c)
                elif nxt == ord("8"):
                    self.r, self.c = self.saved
                elif nxt == ord("M"):
                    if self.r == self.top:
                        self._scroll_down()
                    else:
                        self.r -= 1
                elif nxt == ord("D"):
                    self._newline()
                i += 2
                continue
            if b == 0x0D:
                self.c = 0
            elif b == 0x0A:
                self._newline()
            elif b == 0x08:
                self.c = max(0, self.c - 1)
            elif b == 0x09:
                self.c = min(self.cols - 1, (self.c // 8 + 1) * 8)
            elif b < 0x20 or b == 0x7F:
                pass
            else:
                # decode one UTF-8 character
                ln = 1 if b < 0x80 else 2 if b >> 5 == 6 else 3 if b >> 4 == 14 else 4
                if i + ln > n:
                    self.pending = data[i:]
                    return
                self._put(data[i:i + ln].decode("utf-8", "replace"))
                i += ln
                continue
            i += 1

    def _csi(self, params, final):
        private = params[:1] in ("?", "<", ">", "=")  # a bare "" must not count
        if private:
            return
        nums = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else []
        a = nums[0] if nums else 0
        one = a or 1
        if final in "Hf":
            self.r = min(max((nums[0] if nums and nums[0] else 1) - 1, 0), self.rows - 1)
            self.c = min(max((nums[1] if len(nums) > 1 and nums[1] else 1) - 1, 0), self.cols - 1)
        elif final == "A":
            self.r = max(self.top, self.r - one)
        elif final == "B":
            self.r = min(self.bot, self.r + one)
        elif final == "C":
            self.c = min(self.cols - 1, self.c + one)
        elif final == "D":
            self.c = max(0, self.c - one)
        elif final == "G":
            self.c = min(max(one - 1, 0), self.cols - 1)
        elif final == "d":
            self.r = min(max(one - 1, 0), self.rows - 1)
        elif final == "J":
            if a in (0, 2, 3):
                start = self.r if a == 0 else 0
                for rr in range(start, self.rows):
                    for cc in range(self.cols):
                        if a == 0 and rr == self.r and cc < self.c:
                            continue
                        self.grid[rr][cc] = " "
            elif a == 1:
                for rr in range(0, self.r + 1):
                    for cc in range(self.cols):
                        if rr == self.r and cc > self.c:
                            break
                        self.grid[rr][cc] = " "
        elif final == "K":
            rng = range(self.c, self.cols) if a == 0 else range(0, self.c + 1) if a == 1 else range(self.cols)
            for cc in rng:
                self.grid[self.r][cc] = " "
        elif final == "X":
            for cc in range(self.c, min(self.cols, self.c + one)):
                self.grid[self.r][cc] = " "
        elif final == "P":
            row = self.grid[self.r]
            del row[self.c:self.c + one]
            row.extend([" "] * (self.cols - len(row)))
        elif final == "@":
            row = self.grid[self.r]
            row[self.c:self.c] = [" "] * one
            del row[self.cols:]
        elif final == "L":
            for _ in range(one):
                del self.grid[self.bot]
                self.grid.insert(self.r, [" "] * self.cols)
        elif final == "M":
            for _ in range(one):
                del self.grid[self.r]
                self.grid.insert(self.bot, [" "] * self.cols)
        elif final == "S":
            self._scroll_up(one)
        elif final == "T":
            self._scroll_down(one)
        elif final == "r":
            t = (nums[0] if nums and nums[0] else 1) - 1
            b = (nums[1] if len(nums) > 1 and nums[1] else self.rows) - 1
            self.top, self.bot = t, b
            self.r = self.c = 0
        elif final == "b":
            for _ in range(one):
                self._put(self.last)

    def lines(self):
        return ["".join(row).rstrip() for row in self.grid]

    def text(self):
        return "\n".join(self.lines())


def main():
    cols, rows, steps = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    argv = sys.argv[sys.argv.index("--") + 1:]

    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-256color", LANG="en_US.UTF-8", COLUMNS=str(cols), LINES=str(rows))
        os.execvpe(argv[0], argv, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    screen = Screen(cols, rows)
    failures = 0
    alive = [True]

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end and alive[0]:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    alive[0] = False
                    return
                if not data:
                    alive[0] = False
                    return
                screen.feed(data)

    def wait_for(text, present, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            pump(0.1)
            if (text in screen.text()) == present:
                return True
            if not alive[0]:
                break
        return (text in screen.text()) == present

    for step in steps.split(";"):
        step = step.strip()
        if not step:
            continue
        kind, _, arg = step.partition(":")
        if kind == "wait":
            pump(float(arg))
        elif kind == "send":
            os.write(fd, arg.encode().decode("unicode_escape").encode("utf-8"))
            pump(0.3)
        elif kind == "expect":
            ok = wait_for(arg, True)
            print(("PASS" if ok else "FAIL") + " expect: " + arg)
            failures += 0 if ok else 1
        elif kind == "gone":
            ok = wait_for(arg, False)
            print(("PASS" if ok else "FAIL") + " gone: " + arg)
            failures += 0 if ok else 1
        elif kind in ("rightof", "leftof"):
            limit_s, _, target = arg.partition(":")
            wait_for(target, True)
            col = -1
            for line in screen.lines():
                if target in line:
                    col = line.index(target)
                    break
            ok = (col >= int(limit_s)) if kind == "rightof" else (0 <= col <= int(limit_s))
            print(("PASS" if ok else "FAIL") + " %s: %s starts at column %d, wanted %s %s" % (
                kind, target, col, "at least" if kind == "rightof" else "at most", limit_s))
            failures += 0 if ok else 1
        elif kind == "run":
            os.system(arg)
        elif kind == "count":
            pump(0.3)
            n = sum(1 for l in screen.lines() if arg in l)
            print("COUNT %d rows contain: %s" % (n, arg))
        elif kind == "snap":
            pump(0.3)
            print("---- screen: " + arg)
            for l in screen.lines():
                print("|" + l)
            print("----")

    pump(0.3)
    try:
        done, status = os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        done, status = pid, 0
    if done == 0:
        os.kill(pid, signal.SIGTERM)
        os.waitpid(pid, 0)
        print("PROGRAM: still running at end of steps, stopped by driver")
    else:
        print("PROGRAM: exited with status", os.waitstatus_to_exitcode(status))
    sys.exit(1 if failures else 0)


main()
