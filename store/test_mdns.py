#!/usr/bin/env python3
"""
Offline tests for mdns_advertise.py — no network, no multicast, no daemon.

Builds the exact query bytes an mDNS browser (avahi, or the board's
MDNS.queryService) sends, feeds them to the responder with a fake socket, and
checks the answer it would have put on the wire. Run: python3 store/test_mdns.py
"""
import socket
import struct
import sys

# The fake socket below is a deliberate test double for the real UDP socket.
# pyright: reportAttributeAccessIssue=false, reportOptionalMemberAccess=false

from mdns_advertise import (StoreAdvertiser, encode_name, decode_name,
                            SERVICE_TYPE, SERVICES_ENUM, T_A, T_PTR, T_SRV, T_TXT)

FAILS = []


def check(label, got, want):
    if got == want:
        print(f"  ok    {label}")
    else:
        print(f"  FAIL  {label}\n          got  {got!r}\n          want {want!r}")
        FAILS.append(label)


def build_query(name, qtype=T_PTR, qclass=1):
    """Wire bytes for a single-question query."""
    return (struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
            + encode_name(name) + struct.pack("!HH", qtype, qclass))


def parse_reply(packet):
    """Returns (flags, [(name, rtype, rdata), ...]) from a response packet."""
    _id, flags, qd, an, _ns, _ar = struct.unpack("!HHHHHH", packet[:12])
    off = 12
    for _ in range(qd):
        _, off = decode_name(packet, off)
        off += 4
    records = []
    for _ in range(an):
        name, off = decode_name(packet, off)
        rtype, _rclass, _ttl, rdlen = struct.unpack("!HHIH", packet[off:off + 10])
        off += 10
        records.append((name, rtype, packet[off:off + rdlen]))
        off += rdlen
    return flags, records


class FakeSock:
    """Stands in for the UDP socket; records instead of transmitting."""
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr=None):
        self.sent.append((data, addr))
        return len(data)


def advertiser():
    adv = StoreAdvertiser(port=8082, ip="192.168.1.169")
    adv._sock = FakeSock()
    return adv


print("name encoding")
check("round-trip", decode_name(encode_name("_oneboard-store._tcp.local") + b"\x00", 0)[0],
      "_oneboard-store._tcp.local")
check("terminated", encode_name("a.bc")[-1:], b"\x00")
check("label lengths", encode_name("ab.cde"), b"\x02ab\x03cde\x00")
# A peer may compress: a pointer to offset 12 holding "_x.local"
compressed = b"\x00" * 12 + encode_name("_x.local") + b"\xc0\x0c"
check("compression pointer", decode_name(compressed, 12 + len(encode_name("_x.local")))[0],
      "_x.local")

print("\nptr query -> full answer set")
adv = advertiser()
adv._handle_query(build_query(SERVICE_TYPE), ("192.168.1.50", 12345))
check("exactly one reply", len(adv._sock.sent), 1)
packet, dest = adv._sock.sent[0]
check("replied to the multicast group", dest, ("224.0.0.251", 5353))
flags, records = parse_reply(packet)
check("marked as a response", bool(flags & 0x8000), True)
by_type = {rtype: rdata for _n, rtype, rdata in records}
# Two records share type PTR (the service and the enumeration answer), so pick
# the service PTR by name rather than by type.
service_ptrs = [rd for n, t, rd in records if t == T_PTR and n == SERVICE_TYPE]
check("records present (PTR+SRV+TXT+A+enum)", sorted(by_type), sorted([T_PTR, T_SRV, T_TXT, T_A]))
check("PTR -> our instance", decode_name(service_ptrs[0], 0)[0],
      "OneBoard Store." + SERVICE_TYPE)
check("SRV port", struct.unpack("!H", by_type[T_SRV][4:6])[0], 8082)
check("SRV target", decode_name(by_type[T_SRV], 6)[0], "oneboard-store.local")
check("A -> our address", socket.inet_ntoa(by_type[T_A]), "192.168.1.169")
check("TXT advertises the port",
      b"port=8082" in by_type[T_TXT], True)

print("\nunicast when the querier asks for it (QU bit)")
adv = advertiser()
adv._handle_query(build_query(SERVICE_TYPE, qclass=0x8001), ("192.168.1.50", 12345))
check("reply goes straight back to the asker",
      adv._sock.sent[0][1], ("192.168.1.50", 12345))

print("\nservice enumeration (avahi-browse -a)")
adv = advertiser()
adv._handle_query(build_query(SERVICES_ENUM), ("192.168.1.50", 12345))
check("listed our service type",
      decode_name(parse_reply(adv._sock.sent[0][0])[1][0][2], 0)[0], SERVICE_TYPE)

print("\nthings that must NOT get a reply / must not crash")
adv = advertiser()
adv._handle_query(build_query("_other-service._tcp.local"), ("192.168.1.50", 12345))
check("unrelated service ignored", len(adv._sock.sent), 0)

adv = advertiser()
advert_response = struct.pack("!HHHHHH", 0, 0x8400, 1, 0, 0, 0) + encode_name(SERVICE_TYPE) + struct.pack("!HH", T_PTR, 1)
adv._handle_query(advert_response, ("192.168.1.50", 12345))
check("responses are not answered (no packet storms)", len(adv._sock.sent), 0)

for junk in (b"", b"\x00", b"\xff" * 11, b"\xff" * 40):
    adv = advertiser()
    try:
        adv._handle_query(junk, ("192.168.1.50", 12345))
        ok = True
    except Exception as exc:                     # noqa: BLE001
        ok = f"raised {exc!r}"
    check(f"junk {len(junk)}B survives", ok, True)

print("\nttl=0 goodbye records")
adv = advertiser()
adv.goodbye()
ttls = []
for packet, _d in adv._sock.sent:
    off = 12
    qd, an = struct.unpack("!HH", packet[4:8])
    for _ in range(an):
        _n, off = decode_name(packet, off)
        _t, _c, ttl, rdlen = struct.unpack("!HHIH", packet[off:off + 10])
        ttls.append(ttl)
        off += 10 + rdlen
check("every record expires immediately", set(ttls), {0})

if FAILS:
    print(f"\n{len(FAILS)} FAILED: {', '.join(FAILS)}")
    sys.exit(1)
print("\nall mDNS tests passed")
