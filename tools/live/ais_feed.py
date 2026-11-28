r"""Live ships for gsa-backend: aisstream.io positions around South Africa, posted to /v1/live/ingest.

    python ais_feed.py --backend http://127.0.0.1:PORT [--bbox -38,14,-20,36] [--every 10] [--key-file PATH]

The aisstream.io key is read from AISSTREAM_KEY, else from a key file (one line, the key only):
    Windows        %APPDATA%\GridOfSA\aisstream.key
    macOS / Linux  ~/.config/gridofsa/aisstream.key
The file lives outside the repository on purpose; never commit it or put the key on the command line.
GSA_TOKEN is the back-end's token (not needed with --no-auth).

Standard library only. Connects to wss://stream.aisstream.io/v0/stream (through HTTPS_PROXY when set),
keeps the newest position per ship, and every --every seconds posts the batch to the back-end, which keeps
the latest positions, a 24 h history and the long history of watched ships. Reconnects with backoff.

What is sent per ship: MMSI (id), name, position, speed, course/heading, and from static reports the
ship type, call sign, IMO, destination and size. AIS carries no personal data about crew or owners.
"""
import argparse, base64, json, os, socket, ssl, struct, sys, time, urllib.request
from urllib.parse import urlparse

HOST = "stream.aisstream.io"

def tls_socket():
    ctx = ssl.create_default_context()
    if not ctx.cert_store_stats().get("x509_ca"):
        try:
            import certifi; ctx.load_verify_locations(certifi.where())
        except Exception:
            for p in ("/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/usr/local/etc/openssl/cert.pem"):
                if os.path.exists(p): ctx.load_verify_locations(p); break
    px = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy")
    if px:
        u = urlparse(px if "://" in px else "http://" + px)
        t = socket.create_connection((u.hostname, u.port or 80), timeout=30)
        auth = ""
        if u.username:
            auth = "Proxy-Authorization: Basic " + base64.b64encode(f"{u.username}:{u.password or ''}".encode()).decode() + "\r\n"
        t.sendall(f"CONNECT {HOST}:443 HTTP/1.1\r\nHost: {HOST}:443\r\n{auth}\r\n".encode())
        r = b""
        while b"\r\n\r\n" not in r:
            c = t.recv(4096)
            if not c: raise OSError("proxy closed")
            r += c
        if b" 200" not in r.split(b"\r\n")[0]: raise OSError("proxy: " + r.split(b"\r\n")[0].decode("latin1"))
    else:
        t = socket.create_connection((HOST, 443), timeout=30)
    return ctx.wrap_socket(t, server_hostname=HOST)

class WS:
    """Minimal WebSocket client (RFC 6455): text frames, ping/pong, fragmentation."""
    def __init__(self, sock):
        self.s, self.buf = sock, b""
        key = base64.b64encode(os.urandom(16)).decode()
        sock.sendall((f"GET /v0/stream HTTP/1.1\r\nHost: {HOST}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        while b"\r\n\r\n" not in self.buf:
            c = sock.recv(4096)
            if not c: raise OSError("closed during handshake")
            self.buf += c
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n")[0].decode("latin1")
        if " 101" not in status: raise OSError(status)
    def send(self, op, data):
        m = os.urandom(4); n = len(data); h = bytes([0x80 | op])
        h += bytes([0x80 | n]) if n < 126 else bytes([0xFE]) + struct.pack(">H", n) if n < 65536 else bytes([0xFF]) + struct.pack(">Q", n)
        self.s.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(data)))
    def _rd(self, n):
        while len(self.buf) < n:
            c = self.s.recv(65536)
            if not c: raise EOFError
            self.buf += c
        r, self.buf = self.buf[:n], self.buf[n:]
        return r
    def messages(self):
        frag = b""
        while True:
            b0, b1 = self._rd(2); op = b0 & 15; n = b1 & 127
            if n == 126: n = struct.unpack(">H", self._rd(2))[0]
            elif n == 127: n = struct.unpack(">Q", self._rd(8))[0]
            if b1 & 128:
                mk = self._rd(4); d = bytes(x ^ mk[i % 4] for i, x in enumerate(self._rd(n)))
            else:
                d = self._rd(n)
            if op == 9: self.send(10, d); continue
            if op == 8: raise EOFError("closed: " + d[2:].decode("utf8", "replace")[:160])
            if op in (0, 1, 2):
                frag += d
                if b0 & 128:
                    yield frag.decode("utf8", "replace"); frag = b""

def to_fix(msg, static):
    """One aisstream message -> a fix for /v1/live/ingest, or None. Static reports only update `static`."""
    kind = msg.get("MessageType"); meta = msg.get("MetaData") or {}; body = (msg.get("Message") or {}).get(kind) or {}
    mmsi = str(meta.get("MMSI") or body.get("UserID") or "")
    if not mmsi: return None
    if kind in ("ShipStaticData", "StaticDataReport"):
        dim = body.get("Dimension") or {}
        s = static.setdefault(mmsi, {})
        for k, v in (("type", body.get("Type")), ("callsign", (body.get("CallSign") or "").strip()), ("imo", body.get("ImoNumber")),
                     ("dest", (body.get("Destination") or "").strip()), ("length", (dim.get("A") or 0) + (dim.get("B") or 0) or None)):
            if v not in (None, "", 0): s[k] = v
        name = (body.get("Name") or meta.get("ShipName") or "").strip()
        if name: s["name"] = name
        return None
    if kind not in ("PositionReport", "StandardClassBPositionReport", "ExtendedClassBPositionReport"): return None
    lat, lon = meta.get("latitude", body.get("Latitude")), meta.get("longitude", body.get("Longitude"))
    if lat is None or lon is None or abs(lat) > 90 or abs(lon) > 180: return None
    t = time.time()
    ts = meta.get("time_utc")  # "2026-11-28 01:02:03.456 +0000 UTC"
    if ts:
        try: t = time.mktime(time.strptime(ts[:19], "%Y-%m-%d %H:%M:%S")) - time.timezone
        except ValueError: pass
    hdg = body.get("TrueHeading")
    if hdg is None or hdg >= 360: hdg = body.get("Cog")
    st = static.get(mmsi, {})
    extra = {k: v for k, v in st.items() if k != "name"}
    if body.get("NavigationalStatus") is not None: extra["nav"] = body["NavigationalStatus"]
    sog = body.get("Sog")
    return {"id": mmsi, "name": (st.get("name") or meta.get("ShipName") or "").strip(), "t": t, "lon": lon, "lat": lat,
            "spd": sog if sog is not None and sog < 102.3 else None, "hdg": hdg if hdg is not None and hdg < 360 else None, "extra": extra}

def post(backend, token, fixes):
    req = urllib.request.Request(backend.rstrip("/") + "/v1/live/ingest", method="POST",
                                 data=json.dumps({"kind": "ship", "source": "aisstream.io", "fixes": fixes}).encode(),
                                 headers={"Content-Type": "application/json", **({"Authorization": "Bearer " + token} if token else {})})
    # the back-end is local: never send this through the proxy
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=30) as r: return json.loads(r.read())

def default_key_file():
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "GridOfSA", "aisstream.key")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "gridofsa", "aisstream.key")

def read_key(path):
    key = os.environ.get("AISSTREAM_KEY", "").strip()
    if key: return key
    try:
        with open(path, encoding="utf-8") as f: return f.read().strip()
    except OSError:
        return ""

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--backend", required=True); ap.add_argument("--bbox", default="-38,14,-20,36", help="minlat,minlon,maxlat,maxlon")
    ap.add_argument("--every", type=float, default=10)
    ap.add_argument("--key-file", default=default_key_file())
    a = ap.parse_args()
    key = read_key(a.key_file); token = os.environ.get("GSA_TOKEN", "")
    if not key: sys.exit(f"no aisstream.io key: set AISSTREAM_KEY or save the key in {a.key_file}")
    la0, lo0, la1, lo1 = map(float, a.bbox.split(","))
    sub = json.dumps({"APIKey": key, "BoundingBoxes": [[[la0, lo0], [la1, lo1]]],
                      "FilterMessageTypes": ["PositionReport", "StandardClassBPositionReport", "ExtendedClassBPositionReport", "ShipStaticData", "StaticDataReport"]})
    static, pending, backoff = {}, {}, 5
    while True:
        try:
            s = tls_socket(); ws = WS(s); ws.send(1, sub.encode()); s.settimeout(300)
            print("connected to aisstream.io", flush=True); backoff = 5; last = time.time()
            for text in ws.messages():
                try: f = to_fix(json.loads(text), static)
                except ValueError: continue
                if f: pending[f["id"]] = f
                if time.time() - last >= a.every and pending:
                    try:
                        r = post(a.backend, token, list(pending.values()))
                        print(f"posted {len(pending)} ships, {r.get('history')} history points", flush=True); pending.clear()
                    except Exception as e:
                        print("back-end:", e, flush=True)
                    last = time.time()
        except KeyboardInterrupt:
            return
        except Exception as e:
            print(f"aisstream: {type(e).__name__} {str(e)[:160]}; retrying in {backoff} s", flush=True)
            time.sleep(backoff); backoff = min(backoff * 2, 300)

if __name__ == "__main__":
    main()
