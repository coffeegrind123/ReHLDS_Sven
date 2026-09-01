#!/usr/bin/env python3
"""
A minimal Half-Life-dialect client, enough to drive a ReHLDS_Sven server through
the connectionless handshake and the netchan signon and dump every byte it sends
back.

The point is to make the Half-Life path testable without a Windows client: a
stock client reports a desync as a 32-entry message ring with no byte offsets
you can trust, while this prints the actual packet -- munge state, netchan
header, fragment headers and the svc stream -- which is what the argument is
ever about.

  python3 tools/hlprobe.py --host 172.17.0.2 --port 27015

Speaks the Half-Life dialect on purpose (COM_Munge2 on everything past byte 8),
which is what the server's dialect probe reads.
"""
import argparse, socket, struct, sys, time

# --- COM_Munge2 / COM_UnMunge2 (engine/common.cpp, REHLDS_FIXES unrolled form) --
MASKS = [0xFFFFE7A5, 0xBFEFFFE5, 0xFFBFEFFF, 0xBFEFBFED,
         0xBFAFEFBF, 0xFFBFAFEF, 0xFFEFBFAD, 0xFFFFEFBF,
         0xFFEFF7EF, 0xBFEFE7F5, 0xBFBFE7E5, 0xFFAFB7E7,
         0xBFFFAFB5, 0xBFAFFFAF, 0xFFAFA7FF, 0xFFEFA7A5]

def _bswap(x):
    return int.from_bytes((x & 0xFFFFFFFF).to_bytes(4, "little"), "big")

def _mseq(seq):
    return (_bswap(~seq & 0xFFFFFFFF) ^ seq) & 0xFFFFFFFF

def munge2(data, seq):
    out, m = bytearray(data), _mseq(seq)
    for i in range(len(data) // 4):
        v = int.from_bytes(out[i*4:i*4+4], "little")
        v = (_bswap(v) ^ m ^ MASKS[i & 15]) & 0xFFFFFFFF
        out[i*4:i*4+4] = v.to_bytes(4, "little")
    return bytes(out)

def unmunge2(data, seq):
    out, m = bytearray(data), _mseq(seq)
    for i in range(len(data) // 4):
        v = int.from_bytes(out[i*4:i*4+4], "little")
        v = _bswap((v ^ m ^ MASKS[i & 15]) & 0xFFFFFFFF)
        out[i*4:i*4+4] = v.to_bytes(4, "little")
    return bytes(out)

# --- svc names (engine/net.h) ---------------------------------------------------
SVC = ("svc_bad svc_nop svc_disconnect svc_event svc_version svc_setview svc_sound "
       "svc_time svc_print svc_stufftext svc_setangle svc_serverinfo svc_lightstyle "
       "svc_updateuserinfo svc_deltadescription svc_clientdata svc_stopsound svc_pings "
       "svc_particle svc_damage svc_spawnstatic svc_event_reliable svc_spawnbaseline "
       "svc_temp_entity svc_setpause svc_signonnum svc_centerprint svc_killedmonster "
       "svc_foundsecret svc_spawnstaticsound svc_intermission svc_finale svc_cdtrack "
       "svc_restore svc_cutscene svc_weaponanim svc_decalname svc_roomtype svc_addangle "
       "svc_newusermsg svc_packetentities svc_deltapacketentities svc_choke "
       "svc_resourcelist svc_newmovevars svc_resourcerequest svc_customization "
       "svc_crosshairangle svc_soundfade svc_filetxferfailed svc_hltv svc_director "
       "svc_voiceinit svc_voicedata svc_sendextrainfo svc_timescale svc_resourcelocation "
       "svc_sendcvarvalue svc_sendcvarvalue2 svc_exec").split()

def svcname(c):
    return SVC[c] if c < len(SVC) else ("usermsg#%d" % c if c >= 64 else "svc_reserve%d" % c)

MAX_ROUTEABLE_PACKET = 1400
SPLIT_HDR_HL = 9                      # netID(4) + sequenceNumber(4) + packetID(1)
SPLIT_STRIDE_HL = MAX_ROUTEABLE_PACKET - SPLIT_HDR_HL

def hexdump(b, n=64, indent="      "):
    b = b[:n]
    return "\n".join(indent + " ".join("%02x" % c for c in b[i:i+16]) +
                     "   " + "".join(chr(c) if 32 <= c < 127 else "." for c in b[i:i+16])
                     for i in range(0, len(b), 16))

class Probe:
    def __init__(self, host, port, name, cdkey, verbose):
        self.addr = (host, port)
        self.name, self.cdkey, self.verbose = name, cdkey, verbose
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(3.0)
        self.out_seq = 0            # our outgoing sequence
        self.in_seq = 0             # highest server sequence seen
        self.in_rel = 0             # server's reliable bit we ack
        self.rel_seq = 0            # our reliable toggle
        self.split = {}             # sequenceNumber -> {number: payload}
        self.frags = {0: [], 1: []} # normal, file
        self.packets = 0
        self.sent_sendres = False
        self.sent_spawn = False
        self.pending = []
        self.spawncount = 0
        self.dlfiles = []
        self.dumpdir = None
        self.usermsgs = {}
        self.msgsizes = []

    # -- connectionless ---------------------------------------------------------
    def oob(self, text):
        self.s.sendto(b"\xff\xff\xff\xff" + text.encode() + b"\x00", self.addr)

    def recv(self, timeout=3.0):
        self.s.settimeout(timeout)
        try:
            return self.s.recvfrom(65536)[0]
        except socket.timeout:
            return None

    def handshake(self):
        self.oob("getchallenge steam\n")
        r = self.recv()
        if not r:
            raise SystemExit("no reply to getchallenge")
        body = r[4:].rstrip(b"\x00\n").decode(errors="replace")
        print("<- challenge: %s" % body)
        if not body.startswith("A"):
            raise SystemExit("unexpected challenge reply")
        challenge = body.split()[1]

        protinfo = r"\prot\3\unique\-1\raw\steam\cdkey\%s" % self.cdkey
        userinfo = (r"\bottomcolor\6\cl_autowepswitch\1\cl_dlmax\512\cl_lc\1\cl_lw\1"
                    r"\cl_updaterate\60\hud_classautokill\1\model\gordon\topcolor\30"
                    r"\rate\50000\name\%s" % self.name)
        self.oob('connect 48 %s "%s" "%s"\n' % (challenge, protinfo, userinfo))
        r = self.recv()
        if not r:
            raise SystemExit("no reply to connect")
        body = r[4:].rstrip(b"\x00\n").decode(errors="replace")
        print("<- connect:   %s" % body)
        if body[:1] == "9" or body[:1] == "8":
            raise SystemExit("rejected: " + body[1:])
        return True

    # -- netchan ----------------------------------------------------------------
    def send_netchan(self, payload, reliable=True):
        self.out_seq += 1
        if reliable:
            self.rel_seq ^= 1
        w1 = self.out_seq | ((1 << 31) if reliable else 0)
        w2 = self.in_seq | (self.in_rel << 31)
        # The engine munges with the sequence it just wrote into w1, not the next one.
        body = munge2(payload, self.out_seq & 0xFF)
        pkt = struct.pack("<II", w1 & 0xFFFFFFFF, w2 & 0xFFFFFFFF) + body
        if len(pkt) < 16:
            pkt += munge2(b"\x01" * (16 - len(pkt)), self.out_seq & 0xFF)
        self.s.sendto(pkt, self.addr)

    def ack(self):
        """An empty unreliable packet: carries the acks and nothing else."""
        self.send_netchan(b"", reliable=False)

    def stringcmd(self, cmd):
        payload = bytearray(b"\x03")          # clc_stringcmd
        payload += cmd.encode() + b"\x00"
        while len(payload) % 4:               # munge works on whole dwords
            payload += b"\x01"                # clc_nop
        self.send_netchan(bytes(payload))
        print("-> clc_stringcmd %r  (seq %d)" % (cmd, self.out_seq))

    # -- receive side -----------------------------------------------------------
    def reassemble_split(self, data):
        """Returns a whole datagram, or None while parts are still missing."""
        if len(data) < SPLIT_HDR_HL or struct.unpack_from("<i", data, 0)[0] != -2:
            return data
        seqno, pid = struct.unpack_from("<iB", data, 4)
        count, number = pid & 0xF, pid >> 4
        body = data[SPLIT_HDR_HL:]
        hl_pid = data[8]
        sv_pid = int.from_bytes(data[8:10], "little")
        print("   [split] hdr %s | HL-read: part %d/%d payload=%d | SVEN-read: part %d/%d payload=%d"
              % (" ".join("%02x" % c for c in data[:12]),
                 (hl_pid >> 4) + 1, hl_pid & 0xF, len(data) - 9,
                 (sv_pid >> 8) + 1, sv_pid & 0xF, len(data) - 10))
        d = self.split.setdefault(seqno, {})
        d[number] = body
        if len(d) < count:
            return None
        buf = bytearray()
        for i in range(count):
            need = i * SPLIT_STRIDE_HL
            if len(buf) < need:
                print("   [split] !! gap of %d bytes before part %d "
                      "(server used a shorter stride)" % (need - len(buf), i + 1))
                buf += b"\x00" * (need - len(buf))
            buf[need:need + len(d[i])] = d[i]
        del self.split[seqno]
        print("   [split] reassembled %d bytes from %d parts" % (len(buf), count))
        return bytes(buf)

    def handle(self, data):
        if data[:4] == b"\xff\xff\xff\xff":
            print("<- oob: %r" % data[4:].rstrip(b"\x00\n")[:200])
            return
        self.packets += 1
        raw = self.reassemble_split(data)
        if raw is None:
            return
        if len(raw) < 8:
            print("   short packet (%d bytes)" % len(raw)); return
        seq, ack = struct.unpack_from("<II", raw, 0)
        rel = seq >> 31
        has_frags = bool(seq & (1 << 30))
        seqn = seq & 0x3FFFFFFF
        body = unmunge2(raw[8:], seqn & 0xFF)
        print("<- pkt #%d  sz=%d seq=%d rel=%d frags=%d" %
              (self.packets, len(raw), seqn, rel, has_frags))
        self.in_seq = seqn
        if rel:
            self.in_rel ^= 1

        off = 0
        fraginfo = []
        if has_frags:
            for stream in range(2):
                present = body[off]; off += 1
                if present:
                    fragid = struct.unpack_from("<I", body, off)[0]; off += 4
                    fo, fl = struct.unpack_from("<HH", body, off); off += 4
                    fraginfo.append((stream, fragid, fo, fl))
                    print("   frag[%s] id=%d(%d/%d) offset=%d length=%d" %
                          ("normal" if stream == 0 else "file", fragid,
                           fragid >> 16, fragid & 0xFFFF, fo, fl))
        if has_frags and not any(st == 0 for st, _, _, _ in fraginfo):
            print("   note: normal stream absent -> byte 8 of this packet is 0x00, "
                  "which is svc_bad to anything that parses from offset 8")
        msg = bytearray(body[off:])
        base = 8 + off
        for stream, fragid, fo, fl in reversed(fraginfo):
            chunk = bytes(msg[fo:fo + fl])
            self.frags[stream].append((fragid, chunk))
            del msg[fo:fo + fl]
        if msg:
            if self.verbose:
                print(hexdump(bytes(msg), 128))
            self.parse(bytes(msg), base, "packet payload")
        for stream in (0,):
            if self.frags[stream]:
                fid = self.frags[stream][-1][0]
                if (fid >> 16) == (fid & 0xFFFF):
                    blob = b"".join(c for _, c in self.frags[stream])
                    self.frags[stream] = []
                    if blob[:4] == b"BZ2\x00":
                        import bz2
                        try:
                            blob = bz2.decompress(blob[4:])
                            print("   [normal fragments] decompressed to %d bytes" % len(blob))
                        except Exception as e:
                            print("   [normal fragments] !! bz2 failed: %s" % e); continue
                    if self.dumpdir:
                        import os
                        fn = os.path.join(self.dumpdir, "payload-%05d.bin" % len(blob))
                        open(fn, "wb").write(blob)
                        print("   [dumped %s]" % fn)
                    self.parse(blob, 0, "reassembled normal fragments")

    def note(self, cmd, buf=None, i=0):
        """Advance the signon the way a real client does."""
        if cmd == 11 and not self.sent_sendres:      # svc_serverinfo
            self.sent_sendres = True
            self.pending.append("sendres")
        if cmd == 45 and not self.sent_spawn:        # svc_resourcerequest
            self.sent_spawn = True
            self.spawncount = struct.unpack_from("<i", buf, i + 1)[0]
            print("      (spawncount %d)" % self.spawncount)
            for f in self.dlfiles:
                self.pending.append("dlfile %s" % f)
            self.pending.append("spawn %d 0" % self.spawncount)

    def parse(self, buf, base, what):
        print("   -- %s (%d bytes, offsets from %d)" % (what, len(buf), base))
        i = 0
        while i < len(buf):
            cmd = buf[i]
            extra = ""
            if cmd in (2, 8, 9, 49):
                try:
                    extra = "  %r" % buf[i+1:buf.index(b"\x00", i + 1)].decode(errors="replace")
                except ValueError:
                    extra = "  <unterminated>"
            print("      %04d %s%s" % (base + i, svcname(cmd), extra))
            self.note(cmd, buf, i)
            if cmd == 0:
                print("      !!! svc_bad at offset %d -- a stock client Host_Errors here" % (base + i))
                print(hexdump(buf[max(0, i - 16):i + 48]))
                return False
            n = self.msglen(buf, i)
            if n is None:
                print("      (stopping: %s has no fixed length in this parser)" % svcname(cmd))
                return True
            i += n
        return True

    def _str(self, buf, i):
        e = buf.index(b"\x00", i)
        return buf[i:e].decode(errors="replace"), e + 1

    def msglen(self, buf, i):
        """Byte length of the messages this harness needs to walk past."""
        cmd = buf[i]
        if cmd == 11:                         # svc_serverinfo
            j = i + 1
            proto, spawncount, crc = struct.unpack_from("<iii", buf, j); j += 12
            j += 16                           # clientdll md5
            maxclients, playernum, dm = buf[j], buf[j+1], buf[j+2]; j += 3
            gamedir, j = self._str(buf, j)
            hostname, j = self._str(buf, j)
            mapname, j = self._str(buf, j)
            maplist, j = self._str(buf, j)
            j += 1                            # isVAC2Secure
            print("           protocol=%d spawncount=%d maxclients=%d slot=%d" %
                  (proto, spawncount, maxclients, playernum))
            print("           gamedir=%r hostname=%r map=%r" % (gamedir, hostname, mapname))
            self.spawncount = spawncount
            return j - i
        if cmd == 54:                         # svc_sendextrainfo: string, byte
            _, j = self._str(buf, i + 1)
            return j + 1 - i
        if cmd == 32:                         # svc_cdtrack
            return 3
        if cmd == 5:                          # svc_setview
            return 3
        if cmd == 25:                         # svc_signonnum
            return 2
        if cmd in (1,):                       # svc_nop
            return 1
        if cmd in (8, 9, 36):                 # print, stufftext, decalname-ish strings
            e = buf.index(b"\x00", i + 1)
            return e - i + 1
        if cmd == 2:                          # disconnect
            e = buf.index(b"\x00", i + 1)
            return e - i + 1
        if cmd == 37:                         # roomtype: short
            return 3
        if cmd == 49:                         # filetxferfailed: string
            e = buf.index(b"\x00", i + 1)
            return e - i + 1
        if cmd == 39:                         # newusermsg: index, size, 16-byte name
            self.usermsgs[buf[i+1]] = (buf[i+2], buf[i+3:i+19].split(b"\x00")[0].decode(errors="replace"))
            return 19
        if cmd >= 64:                         # user message
            size, name = self.usermsgs.get(cmd, (255, "?"))
            if size == 255:                   # variable length: one length byte
                n = buf[i+1]
                self.msgsizes.append((n, cmd, name))
                return 2 + n
            self.msgsizes.append((size, cmd, name))
            return 1 + size
        if cmd == 13:                         # updateuserinfo
            return None
        return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="172.17.0.2")
    ap.add_argument("--port", type=int, default=27015)
    ap.add_argument("--name", default="hlprobe")
    ap.add_argument("--cdkey", default="a1b2c3d4e5f60718293a4b5c6d7e8f90")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--dlfile", action="append", default=[],
                    help="request this file once the resource list has arrived "
                         "(exercises the FRAG_FILE_STREAM path)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--dump", help="write each reassembled payload to this directory")
    a = ap.parse_args()

    p = Probe(a.host, a.port, a.name, a.cdkey, a.verbose)
    p.dlfiles = a.dlfile
    p.dumpdir = a.dump
    p.handshake()
    p.stringcmd("new")
    # The netchan will not advance without acks: the server holds one reliable
    # payload in flight until the client acknowledges it, so a passive listener
    # sees exactly one packet and concludes, wrongly, that the server stopped.
    end = time.time() + a.seconds
    last_beat = 0.0
    while time.time() < end:
        d = p.recv(0.05)
        if d is not None:
            p.handle(d)
        if p.pending:
            p.stringcmd(p.pending.pop(0))
            last_beat = time.time()
        elif time.time() - last_beat > 0.05:
            last_beat = time.time()
            p.ack()
    print("\n%d packets seen" % p.packets)
    if p.msgsizes:
        p.msgsizes.sort(reverse=True)
        print("\nuser message payload sizes actually sent (stock client buffer is 192):")
        for n, cmd, name in p.msgsizes[:12]:
            print("   %4d bytes  msg %3d %-16s %s" % (n, cmd, name,
                  "OVERRUNS a stock client" if n > 192 else ""))
        print("   largest %d, %d of %d messages over 192" %
              (p.msgsizes[0][0], sum(1 for n,_,_ in p.msgsizes if n > 192), len(p.msgsizes)))

main()
