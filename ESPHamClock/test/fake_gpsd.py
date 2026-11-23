#!/usr/bin/env python3
"""Fake gpsd on port 2947 (what HamClock's gpsd.cpp expects). Like the real gpsd it sends VERSION, DEVICES
and WATCH lines first, then TPV. Use --sky-first to send a >1000 byte SKY line before the TPV."""
import argparse, json, socketserver, time

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=2947)
ap.add_argument("--sky-first", action="store_true")
A = ap.parse_args()

def dumps(o):
    return json.dumps(o, separators=(",", ":"))   # compact like real gpsd

def tpv():
    t = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())
    return dumps({"class": "TPV", "device": "/dev/ttyACM0", "mode": 3, "time": t, "ept": 0.005,
                       "lat": 34.567891234, "lon": -123.456789123, "altHAE": 12.3, "speed": 0.0})

SKY = dumps({"class": "SKY", "device": "/dev/ttyACM0",
                  "satellites": [{"PRN": i, "el": 10 + i, "az": 5 * i, "ss": 30.0, "used": True} for i in range(24)]})

class H(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            self.serve()
        except (BrokenPipeError, ConnectionResetError):
            pass    # client hung up, which HamClock does as soon as it has what it wants
        print("disconnected", self.client_address, flush=True)

    def serve(self):
        print("connected", self.client_address, flush=True)
        send = lambda s: self.wfile.write((s + "\n").encode())
        send('{"class":"VERSION","release":"3.25","rev":"3.25","proto_major":3,"proto_minor":15}')
        send('{"class":"DEVICES","devices":[{"class":"DEVICE","path":"/dev/ttyACM0","driver":"u-blox","activated":"2026-01-01T00:00:00.000Z"}]}')
        send('{"class":"WATCH","enable":true,"json":true,"nmea":false,"raw":0,"scaled":false,"timing":false}')
        if A.sky_first:
            send(SKY); print("sent SKY len", len(SKY), flush=True)
        for _ in range(20):
            send(tpv())
            time.sleep(0.3)

class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

print("fake gpsd on", A.port, flush=True)
S(("0.0.0.0", A.port), H).serve_forever()
