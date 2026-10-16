#!/usr/bin/env python3
"""
Minimal mDNS/DNS-SD advertiser for the OneBoard behavior store.

Stdlib only, on purpose: serve.py is the thing you run to hand a board some JSON,
and it has no dependencies today. Pulling in a package to publish one A record
isn't worth it. So this speaks just enough mDNS to answer a PTR query for
_oneboard-store._tcp.local with the SRV + TXT + A records that the board's
MDNS.queryService() needs.

That is the whole point of the feature: the board browses for the store on boot
instead of being told an address, so nobody has to type an IP — and because the
lookup happens on every boot, a PC whose DHCP address moved is re-found
automatically.

Library use:
    adv = StoreAdvertiser(port=8082)
    adv.start()
    ...
    adv.stop()                      # sends the goodbye records

Standalone (handy for testing — then browse it from another shell):
    python3 store/mdns_advertise.py --port 8082
    avahi-browse -rt _oneboard-store._tcp
"""
import argparse
import socket
import struct
import threading
import time

MDNS_ADDR = "224.0.0.251"
MDNS_PORT = 5353

# DNS record types / classes
T_A, T_PTR, T_TXT, T_SRV, T_ANY = 1, 12, 16, 33, 255
C_IN = 1
FLUSH = 0x8000          # cache-flush bit, set on the records unique to us

SERVICE_TYPE = "_oneboard-store._tcp.local"     # what the firmware browses for
SERVICES_ENUM = "_services._dns-sd._udp.local"  # service enumeration (avahi-browse -a)
TTL = 120


def encode_name(name):
    """DNS wire name: length-prefixed labels, no compression (legal, just bigger)."""
    out = b""
    for label in name.rstrip(".").split("."):
        raw = label.encode("utf-8")
        if not raw:
            continue
        out += bytes([len(raw)]) + raw
    return out + b"\x00"


def decode_name(data, off):
    """Returns (name, new_offset). Handles the compression pointers peers may use."""
    labels = []
    while off < len(data):
        ln = data[off]
        if ln == 0:
            off += 1
            break
        if ln & 0xC0:                                   # 0b11 -> compression pointer
            ptr = struct.unpack("!H", data[off:off + 2])[0] & 0x3FFF
            sub, _ = decode_name(data, ptr)
            if sub:
                labels.append(sub)
            off += 2
            break
        labels.append(data[off + 1:off + 1 + ln].decode("utf-8", "replace"))
        off += 1 + ln
    return ".".join(labels), off


def lan_ip():
    """Primary LAN address. Uses the routing table — no packets are actually sent."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class StoreAdvertiser:
    """Publishes <instance>._oneboard-store._tcp.local -> this host:port."""

    def __init__(self, port, instance="OneBoard Store",
                 host="oneboard-store.local", ip=None, verbose=False):
        self.port = port
        self.instance = instance
        self.instance_fqdn = f"{instance}.{SERVICE_TYPE}"
        self.host = host
        self.ip = ip or lan_ip()
        self.verbose = verbose
        self._stop = threading.Event()
        self._sock = None
        self._thread = None

    # ---------- packet building ----------

    @staticmethod
    def _rr(name, rtype, ttl, rdata, flush=False):
        rclass = C_IN | (FLUSH if flush else 0)
        return (encode_name(name)
                + struct.pack("!HHIH", rtype, rclass, ttl, len(rdata))
                + rdata)

    def _records(self, ttl=TTL):
        txt = b""
        for kv in (f"port={self.port}", "path=/"):
            raw = kv.encode()
            txt += bytes([len(raw)]) + raw
        return [
            # PTR: service type -> our instance
            self._rr(SERVICE_TYPE, T_PTR, ttl, encode_name(self.instance_fqdn)),
            # SRV: instance -> host + port
            self._rr(self.instance_fqdn, T_SRV, ttl,
                     struct.pack("!HHH", 0, 0, self.port) + encode_name(self.host),
                     flush=True),
            # TXT: free-form hints
            self._rr(self.instance_fqdn, T_TXT, ttl, txt, flush=True),
            # A: host -> address
            self._rr(self.host, T_A, ttl, socket.inet_aton(self.ip), flush=True),
        ]

    def _enum_record(self, ttl=TTL):
        return [self._rr(SERVICES_ENUM, T_PTR, ttl, encode_name(SERVICE_TYPE))]

    @staticmethod
    def _packet(records):
        # id 0, flags 0x8400 = response + authoritative
        return (struct.pack("!HHHHHH", 0, 0x8400, 0, len(records), 0, 0)
                + b"".join(records))

    # ---------- socket ----------

    def _send(self, data, addr=None):
        try:
            self._sock.sendto(data, addr or (MDNS_ADDR, MDNS_PORT))
        except OSError:
            pass

    def announce(self):
        """Unsolicited announcement, so peers don't have to wait for a query."""
        self._send(self._packet(self._records()))
        self._send(self._packet(self._enum_record()))

    def goodbye(self):
        """TTL=0 records — tells caches to drop us promptly on shutdown."""
        self._send(self._packet(self._records(ttl=0)))

    # ---------- query handling ----------

    def _handle_query(self, data, addr):
        if len(data) < 12:                  # shorter than a DNS header
            return
        _id, flags, qdcount = struct.unpack("!HHH", data[:6])
        if flags & 0x8000:          # it's a response, not a query — ignore
            return
        off = 12
        for _ in range(qdcount):
            name, off = decode_name(data, off)
            if off + 4 > len(data):
                return
            qtype, qclass = struct.unpack("!HH", data[off:off + 4])
            off += 4
            lname = name.lower()
            if lname == SERVICE_TYPE.lower() and qtype in (T_PTR, T_ANY):
                # QU bit set -> answer unicast; otherwise answer to the group.
                dest = addr if (qclass & FLUSH) else (MDNS_ADDR, MDNS_PORT)
                self._send(self._packet(self._records() + self._enum_record()), dest)
                if self.verbose:
                    print(f"mDNS: answered {SERVICE_TYPE} for {addr[0]}")
                return
            if lname == SERVICES_ENUM.lower() and qtype in (T_PTR, T_ANY):
                self._send(self._packet(self._enum_record()))
                if self.verbose:
                    print(f"mDNS: listed services for {addr[0]}")
                return

    def run(self):
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(9000)
            except OSError:
                if self._stop.is_set():
                    return
                continue
            if len(data) < 12:
                continue
            try:
                self._handle_query(data, addr)
            except Exception:
                # A malformed packet from the network must never kill the thread.
                pass

    # ---------- lifecycle ----------

    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        except (AttributeError, OSError):
            pass
        self._sock.bind(("", MDNS_PORT))
        # Join the group on the LAN interface only — not on every interface.
        mreq = socket.inet_aton(MDNS_ADDR) + socket.inet_aton(self.ip)
        self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        try:
            self._sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                                  socket.inet_aton(self.ip))
        except OSError:
            pass
        self._thread = threading.Thread(target=self.run, name="mdns", daemon=True)
        self._thread.start()
        self.announce()
        # The spec asks for a second announcement ~1s later; queued off-thread so
        # start() doesn't block serve.py's startup.
        threading.Timer(1.0, self.announce).start()
        return self

    def stop(self):
        self._stop.set()
        try:
            self.goodbye()
        except Exception:
            pass
        if self._sock:
            try:
                self._sock.close()
            except OSError:
                pass
        if self._thread:
            self._thread.join(timeout=2)


def main():
    ap = argparse.ArgumentParser(description="Advertise a OneBoard store over mDNS")
    ap.add_argument("--port", type=int, default=8082,
                    help="port the store is actually serving on (default 8082)")
    ap.add_argument("--ip", default=None, help="override the advertised address")
    ap.add_argument("--name", default="OneBoard Store", help="service instance name")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    adv = StoreAdvertiser(port=args.port, ip=args.ip,
                          instance=args.name, verbose=args.verbose).start()
    print(f"Advertising {SERVICE_TYPE}")
    print(f"  -> {adv.instance_fqdn}")
    print(f"  -> http://{adv.ip}:{args.port}/")
    print("Ctrl-C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        adv.stop()
        print("stopped (goodbye sent)")


if __name__ == "__main__":
    main()
