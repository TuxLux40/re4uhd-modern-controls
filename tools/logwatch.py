#!/usr/bin/env python3
"""Play-test watcher for RE4ModernControls.log.

Runs quietly in the background and analyses the mod's log itself, so nobody has to read it line by line. It only
ends (one notification) in two cases, each with the compiled report of the whole session:

  FREEZE  the game's log stopped while one game thread spins at 100 % (stack of that thread captured with gdb)
  EXIT    the game process went away (quit or crash)

Everything else is only collected for that report:
  ERROR    the mod logged an error (hook failed, NaN, ...)
  FARCAM   gameplay camera far from Leon (> 6000) or a fov > 95 flash
  AIMSHOT  the camera moves or zooms within 1 s of a shot while the aim button is held
  CAMJUMP  a big one-frame camera jump outside aim start/end and camera switches
  AIMOFF   while aiming, the gun stays more than 15 deg off the reticle for 2 s (the laser is bent to the reticle)

Everything (also the mod's own fix messages, counted as INFO) goes to the report file. Kinds passed with --mute are
marked as known in the report. --threshold N (default 0 = off) also ends the watch once a kind reaches N incidents.
Usage:
  python3 -I tools/logwatch.py [--mute FARCAM,AIMSHOT] [--threshold N] [--report PATH] [--replay LOG]
"""
import argparse
import os
import re
import subprocess
import sys
import time

LOG = "/mnt/ssd1tb/SteamLibrary/steamapps/common/Resident Evil 4/Bin32/RE4ModernControls.log"
GAME = "bio4.exe"

TS = re.compile(r"^\[(\d+):(\d+):(\d+)\.(\d+)\] (.*)")
CAMLINE = re.compile(r"cam mode (\d+) eye\((-?\d+),(-?\d+),(-?\d+)\).* fov ([\d.]+) \| leon pos\((-?\d+),(-?\d+),(-?\d+)\)"
                     r".*aimkeys (\S+) free (\d)")
JUMP = re.compile(r"diag camjump: turn ([+-][\d.]+) deg, dist (\d+) -> (\d+), height (-?\d+) -> (-?\d+), "
                  r"fov ([\d.]+) -> ([\d.]+) \| stick ([\d.]+) rno \S+ aimkeys (\S+) free (\d)")
ERROR = re.compile(r"unexpected code|MinHook failed|had NaN|\berror\b|exception|crash", re.I)
INFO = re.compile(r"ours put in|ours shown|walk copy dropped|wrapped in one step|aim start with the shown eye|"
                  r"last sane one used")
LASER = re.compile(r"diag laser 0\.5s: gun up to ([\d.]+) deg off")
TRANSITION = re.compile(r"cam: aim (start|end)|back from game camera|diag cam reset|diag cam kept|glide ends")


def secs(m):
    return int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000.0


def game_pid():
    out = subprocess.run(["ps", "-eo", "pid=,comm="], capture_output=True, text=True).stdout
    for line in out.splitlines():
        pid, _, comm = line.strip().partition(" ")
        if comm.strip() == GAME:
            return int(pid)
    return None


def thread_ticks(pid):
    ticks = {}
    try:
        for tid in os.listdir(f"/proc/{pid}/task"):
            with open(f"/proc/{pid}/task/{tid}/stat") as f:
                parts = f.read().rsplit(")", 1)[1].split()
            ticks[int(tid)] = int(parts[11]) + int(parts[12])
    except OSError:
        pass
    return ticks


def spinning_thread(pid, seconds=3.0):
    """The thread that used > 90 % of a core over `seconds`, or None."""
    a = thread_ticks(pid)
    time.sleep(seconds)
    b = thread_ticks(pid)
    hz = os.sysconf("SC_CLK_TCK")
    best = max(((b[t] - a.get(t, b[t]), t) for t in b), default=(0, None))
    return best[1] if best[0] >= 0.9 * hz * seconds else None


def gdb_stack(pid, tid):
    """EIP, the instructions around it and the bio4.exe return addresses on the spinning thread's stack."""
    try:
        info = subprocess.run(["gdb", "-q", "-batch", "-p", str(pid), "-ex", "info threads"], capture_output=True,
                              text=True, timeout=40).stdout
        row = next((l.split() for l in info.splitlines() if f"LWP {tid})" in l), None)  # "[*] <n>  Thread ..."
        num = (row[1] if row[0] == "*" else row[0]) if row else None
        if not num:
            return "gdb: thread not found"
        out = subprocess.run(["gdb", "-q", "-batch", "-p", str(pid), "-ex", f"thread {num}", "-ex", "info registers eip",
                              "-ex", "x/8i $eip-12", "-ex", "x/256wx $esp"], capture_output=True, text=True,
                             timeout=40).stdout
    except (subprocess.SubprocessError, OSError) as e:
        return f"gdb failed: {e}"
    keep, rets = [], []
    for l in out.splitlines():
        if l.startswith(("eip", "   0x", "=> 0x")):
            keep.append(l.rstrip())
        elif l.startswith("0x") and ":" in l:
            rets += [w for w in l.split(":", 1)[1].split() if 0x401000 <= int(w, 16) < 0x9A0000]
    return "\n".join(keep + ["bio4 addresses on the stack: " + " ".join(rets[:16])])


class Watch:
    def __init__(self, args):
        self.args = args
        self.mute = {k.strip().upper() for k in args.mute.split(",") if k.strip()}
        self.report = open(args.report, "a", buffering=1)
        self.incidents = {}   # kind -> list of (time, first line)
        self.info = {}        # info message -> count
        self.last_ts = 0.0
        self.last_transition = -99.0
        self.aim_rest = None  # (dist, fov) of the aim camera before the last shot
        self.aim_now = None
        self.shot_at = -99.0
        self.recent = []      # last lines, for context
        self.off_run = 0      # consecutive 0.5 s laser reports with the gun > 15 deg off

    def note(self, kind, t, line):
        self.report.write(f"{time.strftime('%F %T')} {kind}: {line}\n")
        lst = self.incidents.setdefault(kind, [])
        if lst and t - lst[-1][0] < 3.0:  # same incident
            lst[-1] = (t, lst[-1][1])
            return
        lst.append((t, line))
        if self.args.threshold and kind not in self.mute and len(lst) >= self.args.threshold:
            self.finish(f"{kind} x{len(lst)}")

    def line(self, raw):
        m = TS.match(raw)
        if not m:
            return
        t, text = secs(m), m[5]
        self.last_ts = t
        self.recent = (self.recent + [raw.rstrip()])[-8:]
        if INFO.search(text):
            key = INFO.search(text)[0]
            self.info[key] = self.info.get(key, 0) + 1
            self.report.write(f"{time.strftime('%F %T')} INFO: {raw.rstrip()}\n")
            return
        if ERROR.search(text):
            self.note("ERROR", t, raw.rstrip())
            return
        lm = LASER.search(text)
        if lm:
            self.off_run = self.off_run + 1 if float(lm[1]) > 15 else 0
            if self.off_run == 4:  # 2 s; the raise at aim start takes well under that
                self.note("AIMOFF", t, raw.rstrip())
            return
        if TRANSITION.search(text):
            if "aim" in text:
                self.off_run = 0
            self.last_transition = t
            if "aim start" in text:
                self.aim_rest = None
            return
        if text.startswith("diag shot"):
            if self.aim_now:
                self.aim_rest, self.shot_at = self.aim_now, t
            return
        cm = CAMLINE.search(text)
        if cm and cm[1] == "10":
            ex, ez, fov, lx, lz = float(cm[2]), float(cm[4]), float(cm[5]), float(cm[6]), float(cm[8])
            dist = ((ex - lx) ** 2 + (ez - lz) ** 2) ** 0.5
            aiming = cm[9] != "0" and cm[10] == "0"
            self.check_cam(t, dist, fov, aiming, raw)
            return
        jm = JUMP.search(text)
        if jm:
            turn, d0, d1, f0, f1, stick = (float(jm[1]), float(jm[2]), float(jm[3]), float(jm[6]), float(jm[7]),
                                           float(jm[8]))
            aiming = jm[9] != "0" and jm[10] == "0"
            self.check_cam(t, d1, f1, aiming, raw)
            if t - self.last_transition > 0.5 and d1 < 6000 and d0 < 6000 and (
                    abs(d1 - d0) > 500 or abs(f1 - f0) > 5 or (abs(turn) > 30 and stick < 0.1)):
                self.note("CAMJUMP", t, raw.rstrip())

    def check_cam(self, t, dist, fov, aiming, raw):
        if dist > 6000 or fov > 95:
            self.note("FARCAM", t, raw.rstrip())
        self.aim_now = (dist, fov) if aiming else None
        if aiming and self.aim_rest and t - self.shot_at < 1.0 and t - self.last_transition > 0.5:
            d0, f0 = self.aim_rest
            if abs(dist - d0) > 300 or abs(fov - f0) > 3:
                self.note("AIMSHOT", t, f"{raw.rstrip()}  (before the shot: dist {d0:.0f} fov {f0:.1f})")
                self.aim_rest = None  # one note per shot

    def summary(self):
        parts = [f"{k} x{len(v)}" for k, v in self.incidents.items()]
        parts += [f"info '{k}' x{n}" for k, n in self.info.items()]
        return ", ".join(parts) or "nothing"

    def finish(self, why, detail=""):
        # The compiled report: every kind with its count and first two examples (muted kinds included).
        lines = []
        for kind, lst in self.incidents.items():
            muted = " (muted)" if kind in self.mute else ""
            lines.append(f"{kind}{muted}: {len(lst)} incidents")
            lines += [f"  {l[:220]}" for _, l in lst[:2]]
        lines += [f"info '{k}': {n}x" for k, n in self.info.items()]
        detail = (detail + "\n" if detail else "") + ("\n".join(lines) or "no anomalies")
        text = f"WATCH END: {why}\n{detail}\nreport: {self.args.report}"
        self.report.write(f"{time.strftime('%F %T')} END {text}\n")
        print(text, flush=True)
        sys.exit(0)

    def run(self):
        f, pos, ino = None, 0, None
        pid, last_check = None, 0.0
        print_start = f"watching {LOG} (mute: {','.join(sorted(self.mute)) or '-'})"
        self.report.write(f"{time.strftime('%F %T')} START {print_start}\n")
        while True:
            try:
                st = os.stat(LOG)
                if f is None or st.st_ino != ino or st.st_size < pos:  # new game start rewrites the log
                    if f:
                        f.close()
                    f = open(LOG, errors="replace")
                    if ino is None:
                        f.seek(0, os.SEEK_END)
                    ino = st.st_ino
                    pos = f.tell()
                for raw in f:
                    self.line(raw)
                pos = f.tell()
            except OSError:
                pass
            now = time.time()
            if now - last_check >= 2.0:
                last_check = now
                cur = game_pid()
                if pid and not cur:
                    self.finish("EXIT (game process gone)", "last log lines:\n" + "\n".join(self.recent[-5:]))
                if cur and pid != cur:
                    self.report.write(f"{time.strftime('%F %T')} game running, pid {cur}\n")
                pid = cur
                if pid and now - os.stat(LOG).st_mtime > 20:
                    tid = spinning_thread(pid)
                    if tid and spinning_thread(pid) == tid and now - os.stat(LOG).st_mtime > 20:
                        self.finish("FREEZE (log silent, one game thread at 100 %)",
                                    f"thread {tid}\n" + gdb_stack(pid, tid) + "\nlast log lines:\n" +
                                    "\n".join(self.recent[-5:]))
            time.sleep(0.25)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mute", default="")
    p.add_argument("--threshold", type=int, default=0)  # 0: anomalies never end the watch
    p.add_argument("--report", default=os.path.expanduser("~/.cache/re4watch/report.txt"))
    p.add_argument("--replay", help="analyse a saved log instead of watching; prints what it would have reported")
    args = p.parse_args()
    if args.replay:
        args.threshold, args.report = 10**9, os.devnull
        w = Watch(args)
        with open(args.replay, errors="replace") as f:
            for raw in f:
                w.line(raw)
        for kind, lst in w.incidents.items():
            print(f"{kind}: {len(lst)} incidents, first: {lst[0][1][:160]}")
        print("so far:", w.summary())
        return
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    Watch(args).run()


if __name__ == "__main__":
    main()
