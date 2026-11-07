#!/usr/bin/env python3
"""Prueft an einer echten Uhr, dass mehrere Geraete gleichzeitig an Port 23
haengen koennen (STAR-60, ab Firmware 0.33.0).

    python3 port23_mehrere.py <ip>

Zwei Verbindungen melden sich mit dem Token an (geholt ueber /test/token,
geht nur im Heimnetz). Erwartet:
  1. beide bekommen auf 'pz' eine '#p'-Zeile - keine wirft die andere hinaus,
  2. steuert A mit 'ps', meldet B im neunten Feld 1 (anderes Geraet steuert),
     A selbst 0.
Am Ende faehrt 'hu' die Uhr zurueck zur Uhrzeit.
"""
import socket
import sys
import time
import urllib.request


def token(ip):
    r = urllib.request.Request(f"http://{ip}/test/token", method="POST", headers={"X-Starwatch": "1"})
    return urllib.request.urlopen(r, timeout=3).read().decode().strip()


def verbinden(ip, t):
    s = socket.create_connection((ip, 23), timeout=3)
    s.settimeout(0.3)
    s.sendall(f"token {t}\n".encode())
    time.sleep(0.5)
    lesen(s)
    return s


def lesen(s):
    text = ""
    ende = time.time() + 0.5
    while time.time() < ende:
        try:
            text += s.recv(4096).decode(errors="replace")
        except socket.timeout:
            pass
    return text


def position(s):
    s.sendall(b"pz\n")
    zeilen = [z for z in lesen(s).splitlines() if z.startswith("#p ")]
    return zeilen[-1].split() if zeilen else None


def main(ip):
    t = token(ip)
    a = verbinden(ip, t)
    b = verbinden(ip, t)
    pa, pb = position(a), position(b)
    assert pa and pb, f"Beide brauchen eine #p-Zeile: A={pa} B={pb}"
    print("1. beide verbunden:", " ".join(pa), "|", " ".join(pb))
    a.sendall(f"ps {(int(pa[1]) + 400) % 3200}\n".encode())
    time.sleep(0.3)
    pa, pb = position(a), position(b)
    assert len(pb) >= 9, f"Firmware zu alt (vor 0.33.0)? {pb}"
    assert pb[8] == "1" and pa[8] == "0", f"Erwartet B=1, A=0: A={pa[8]} B={pb[8]}"
    print("2. B sieht: anderes Geraet steuert; A nicht")
    a.sendall(b"hu\n")
    a.close()
    b.close()
    print("OK")


if __name__ == "__main__":
    main(sys.argv[1])
