"""Minimal pcap (libpcap classic format) reader + Ethernet/ARP/IPv4/UDP/TCP/ICMP/DHCP decoder.

Enough to ASSERT what really crossed the guest NIC (QEMU `-object filter-dump`), not what the guest says it did.
"""
import struct

DHCP_TYPES = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE", 5: "ACK", 6: "NAK", 7: "RELEASE", 8: "INFORM"}


def _ip(b):
    return ".".join(str(x) for x in b)


def read(path):
    """Yield decoded packet dicts: {n, t, kind, ...}."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return []
    if len(d) < 24:
        return []
    magic = struct.unpack("<I", d[:4])[0]
    endian = "<" if magic in (0xA1B2C3D4, 0xA1B23C4D) else ">"
    out = []
    off, n = 24, 0
    while off + 16 <= len(d):
        ts, us, cl, _ol = struct.unpack(endian + "IIII", d[off:off + 16])
        pkt = d[off + 16:off + 16 + cl]
        off += 16 + cl
        n += 1
        rec = decode(pkt)
        if rec:
            rec["n"] = n
            rec["t"] = ts + us / 1e6
            out.append(rec)
    return out


def decode(pkt):
    if len(pkt) < 14:
        return None
    et = struct.unpack(">H", pkt[12:14])[0]
    if et == 0x0806 and len(pkt) >= 42:
        return {"kind": "arp", "op": "request" if pkt[21] == 1 else "reply",
                "spa": _ip(pkt[28:32]), "tpa": _ip(pkt[38:42])}
    if et != 0x0800 or len(pkt) < 34:
        return None
    ihl = (pkt[14] & 15) * 4
    proto = pkt[23]
    src, dst = _ip(pkt[26:30]), _ip(pkt[30:34])
    l4 = 14 + ihl
    rec = {"src": src, "dst": dst, "proto": proto}
    if proto == 17 and len(pkt) >= l4 + 8:
        sp, dp = struct.unpack(">HH", pkt[l4:l4 + 4])
        rec.update(kind="udp", sport=sp, dport=dp)
        if (sp, dp) in ((68, 67), (67, 68)) and len(pkt) >= l4 + 8 + 240:
            opts, i, mt = pkt[l4 + 8 + 240:], 0, None
            while i < len(opts) and opts[i] != 255:
                if opts[i] == 0:
                    i += 1
                    continue
                if i + 1 >= len(opts):
                    break
                if opts[i] == 53 and i + 2 < len(opts):
                    mt = opts[i + 2]
                i += 2 + opts[i + 1]
            rec["kind"] = "dhcp"
            rec["dhcp"] = DHCP_TYPES.get(mt, str(mt))
        elif dp == 53 or sp == 53:
            rec["kind"] = "dns"
        return rec
    if proto == 6 and len(pkt) >= l4 + 14:
        sp, dp = struct.unpack(">HH", pkt[l4:l4 + 4])
        fl = pkt[l4 + 13]
        rec.update(kind="tcp", sport=sp, dport=dp, flags=fl, syn=bool(fl & 2), ack=bool(fl & 16),
                   fin=bool(fl & 1), rst=bool(fl & 4))
        return rec
    if proto == 1:
        rec.update(kind="icmp", icmp_type=pkt[l4] if len(pkt) > l4 else -1)
        return rec
    return rec


def match(rec, spec):
    """spec keys: kind, src, dst, sport, dport, dhcp, op, syn, ack, rst (all optional, all must match)."""
    for k, v in spec.items():
        if k in ("min", "max", "id", "why", "type", "stage", "depends_on", "severity", "vm", "after", "steps", "name", "needs_build"):
            continue
        if rec.get(k) != v:
            return False
    return True
