#!/usr/bin/env python3
"""Ask a running Cemu (Debug > Launch with GDB stub) who calls a game function.

usage: cemu_gdb_stack.py ADDR [ADDR...] [--port 1337] [--hits 3]

Connects to Cemu's GDB remote stub, sets a software breakpoint on each ADDR, resumes the game and,
on every hit, prints PC, LR and the PowerPC back chain (the return address saved in each stack
frame), so the call path can be matched against the recompiled code. Detaches at the end.
"""
import argparse
import socket
import struct


class Gdb:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(None)
        self.buf = b""
        self.verbose = False

    def _read_packet(self):
        if self.verbose: print("  [rx wait]", flush=True)
        while True:
            while b"$" not in self.buf:
                got = self.s.recv(65536)
                if not got:
                    raise ConnectionError("stub closed the connection")
                if self.verbose: print("  [rx]", got[:200], flush=True)
                self.buf += got
            start = self.buf.index(b"$")
            while b"#" not in self.buf[start:] or len(self.buf) < self.buf.index(b"#", start) + 3:
                self.buf += self.s.recv(65536)
            end = self.buf.index(b"#", start)
            data = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b"+")
            return data.decode("latin-1")

    def cmd(self, payload, reply=True):
        csum = sum(payload.encode()) & 0xFF
        self.s.sendall(b"$%s#%02x" % (payload.encode(), csum))
        return self._read_packet() if reply else None

    def read32(self, addr):
        r = self.cmd("m%x,4" % addr)
        return int(r, 16) if r and not r.startswith("E") else None

    def regs(self):
        r = self.cmd("g")
        words = [int(r[i:i + 8], 16) for i in range(0, len(r) - 7, 8)]
        return words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("addrs", nargs="+")
    ap.add_argument("--port", type=int, default=1337)
    ap.add_argument("--hits", type=int, default=3)
    ap.add_argument("-v", action="store_true", help="print raw packets")
    a = ap.parse_args()
    g = Gdb(a.port)
    g.verbose = a.v
    g.s.sendall(b"+")
    print("connected:", g.cmd("?"))
    for x in a.addrs:
        print("break %s:" % x, g.cmd("Z0,%x,4" % int(x, 16)))
    for n in range(a.hits):
        stop = g.cmd("c")
        w = g.regs()
        # PowerPC GDB register layout: r0-r31, f0-f31 (64-bit, 2 words each), pc, msr, cr, lr, ctr, xer
        r = w[:32]
        pc = w[32 + 64]
        lr = w[32 + 64 + 3]
        print("\nhit %d (%s): pc=%08X lr=%08X r3=%08X r4=%08X r1=%08X" % (n + 1, stop, pc, lr, r[3], r[4], r[1]))
        sp, chain = r[1], []
        for _ in range(16):
            prev = g.read32(sp)
            if not prev or prev <= sp:
                break
            ret = g.read32(prev + 4)
            chain.append("%08X" % ret if ret is not None else "?")
            sp = prev
        print("  back chain:", " <- ".join(chain))
    for x in a.addrs:
        g.cmd("z0,%x,4" % int(x, 16))
    g.cmd("D")


if __name__ == "__main__":
    main()
