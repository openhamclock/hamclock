#!/usr/bin/env python3
"""
Fake rotctld for testing HamClock's rotator pane without hardware.

Speaks the hamlib "extended response" protocol that HamClock uses (commands like "+\\get_info").
Simulates a rotator that slews toward the last set_pos target.

Usage:
    ./fake_rotctld.py                      # Info: None  (reproduces issue #546)
    ./fake_rotctld.py --info "Yaesu GS-232B"   # a rotator that reports a model name
    ./fake_rotctld.py --info missing       # reply has no Info line at all
    ./fake_rotctld.py --info error         # get_info fails with RPRT -11
    ./fake_rotctld.py --port 4533 --el-max 0   # az-only rotator (no elevation)
    ./fake_rotctld.py --selftest           # start server, query it, print results, exit

Then in HamClock setup point the rotator host at this machine, port 4533.
"""

import argparse
import socketserver
import threading
import time

ARGS = None
LOCK = threading.Lock()
STATE = {"az": 180.0, "el": 0.0, "az_t": 180.0, "el_t": 0.0, "t": time.time()}
SLEW_DEG_PER_S = 6.0


def step():
    """advance simulated position toward target"""
    now = time.time()
    dt = now - STATE["t"]
    STATE["t"] = now
    for k in ("az", "el"):
        d = STATE[k + "_t"] - STATE[k]
        m = SLEW_DEG_PER_S * dt
        STATE[k] = STATE[k + "_t"] if abs(d) <= m else STATE[k] + (m if d > 0 else -m)


def reply(cmd, body):
    """extended-protocol reply: echo line, body lines, RPRT"""
    lines = [cmd + ":"] + body
    return "\n".join(lines) + "\n"


def handle(line):
    line = line.strip()
    if not line.startswith("+"):
        return "RPRT -1\n"
    parts = line[1:].lstrip("\\").split()
    if not parts:
        return "RPRT -1\n"
    cmd, args = parts[0], parts[1:]

    with LOCK:
        step()

        if cmd == "get_info":
            if ARGS.info == "error":
                return reply(cmd, ["RPRT -11"])
            if ARGS.info == "missing":
                return reply(cmd, ["RPRT 0"])
            name = "None" if ARGS.info == "none" else ARGS.info
            return reply(cmd, ["Info: " + name, "RPRT 0"])

        if cmd == "get_pos":
            return reply(cmd, ["Azimuth: %f" % STATE["az"], "Elevation: %f" % STATE["el"], "RPRT 0"])

        if cmd == "set_pos":
            try:
                STATE["az_t"] = float(args[0])
                STATE["el_t"] = float(args[1]) if ARGS.el_max > 0 else 0.0
            except (IndexError, ValueError):
                return reply(cmd, ["RPRT -1"])
            return reply(cmd + " " + " ".join(args), ["RPRT 0"])

        if cmd == "stop":
            STATE["az_t"], STATE["el_t"] = STATE["az"], STATE["el"]
            return reply(cmd, ["RPRT 0"])

        if cmd == "dump_caps":
            return reply(cmd, [
                "Caps dump for model:\t1",
                "Model name:\tFake",
                "Min Azimuth:\t\t0.00",           # format per Hamlib tests/dumpcaps_rot.c
                "Max Azimuth:\t\t%.2f" % ARGS.az_max,
                "Min Elevation:\t\t0.00",
                "Max Elevation:\t\t%.2f" % ARGS.el_max,
                "RPRT 0",
            ])

    return reply(cmd, ["RPRT -4"])  # not implemented


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        peer = "%s:%d" % self.client_address
        print("[%s] connected" % peer)
        for raw in self.rfile:
            line = raw.decode("ascii", "replace")
            out = handle(line)
            print("[%s] %r -> %r" % (peer, line.strip(), out))
            self.wfile.write(out.encode())
        print("[%s] disconnected" % peer)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def selftest(port):
    """query the server like HamClock does and show what the pane title would be"""
    import socket

    def ask(cmd):
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        s.sendall(cmd.encode())
        buf = b""
        while b"RPRT" not in buf:
            buf += s.recv(1024)
        s.close()
        return buf.decode()

    r = ask("+\\get_info\n")
    print("get_info reply:", repr(r))

    # mimic gimbal.cpp connectHamlib(): value is the rest of the Info line; hide "None"
    title = ""
    for ln in r.splitlines():
        if ln.startswith("Info:"):
            v = ln.split(":", 1)[1].strip()
            if v.lower() != "none" and not v.startswith("RPRT"):
                title = v[:19]
    print("expected pane title: %r" % title)
    print("get_pos reply:", repr(ask("+\\get_pos\n")))
    print("dump_caps reply:", repr(ask("+\\dump_caps\n")))


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=4533)
    ap.add_argument("--info", default="none",
                    help='"none" (default), "missing", "error", or any model name string')
    ap.add_argument("--az-max", type=float, default=450.0)
    ap.add_argument("--el-max", type=float, default=180.0, help="0 = no elevation axis")
    ap.add_argument("--selftest", action="store_true")
    ARGS = ap.parse_args()

    srv = Server(("0.0.0.0", ARGS.port), Handler)
    print("fake rotctld on port %d, info=%r" % (ARGS.port, ARGS.info))
    if ARGS.selftest:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        time.sleep(0.2)
        selftest(ARGS.port)
        srv.shutdown()
        return
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
