#!/usr/bin/env python3
"""Star Watch - Geraetezettel und Kopplungs-QR erzeugen.

Erzeugt ZWEI Dinge, und die Trennung ist der Punkt:

  1  ZETTEL FUERS GERAET (SVG, zum Aufkleben auf die Rueckseite)
     Enthaelt den QR, mit dem ein Handy dem Zugangspunkt der Uhr beitritt,
     dazu Geraetename und Seriennummer im Klartext.
     ENTHAELT NICHT das Zugriffstoken und NICHT das OTA-Passwort.

  2  GERAETEBLATT FUER DICH (Text, bleibt beim Erbauer)
     Alles, auch Token und OTA-Passwort. AP 6.2 verlangt "ein Geraeteblatt je
     Seriennummer, damit eine Uhr in zwei Jahren noch erreichbar ist".

WARUM DAS TOKEN NICHT AUF DEN ZETTEL GEHOERT
--------------------------------------------
Ein aufgedrucktes Token ist ein Geheimnis, das sich nie mehr aendern laesst -
ein Foto davon gilt fuer immer. Das Architekturdokument hat den Aufkleber
deshalb ausdruecklich verworfen: "kein Aufkleber, keine Passworteingabe".

Es ist auch unnoetig. Am eigenen Zugangspunkt gibt die Uhr das Token von
selbst heraus, und die Seite uebernimmt es in den Browser. Der QR muss also
nur ins WLAN der Uhr fuehren - den Rest erledigt die Kopplung.

WAS DER QR ENTHAELT
-------------------
    WIFI:T:WPA;S:StarWatch-089C;P:<schluessel>;;

Das ist die Standardform, mit der iOS und Android direkt aus der Kamera einem
WLAN beitreten. Danach greift das Captive Portal und die Seite oeffnet sich
von selbst.

ABHAENGIGKEIT
-------------
`segno` fuer den QR-Code. Am 24.08.2026 auf Festlegung des Projektinhabers
zugelassen: nicht jede Bibliothek verschwindet, und bei etablierten laesst
sich darauf bauen. Reine Python-Bibliothek ohne eigene Abhaengigkeiten.

    python3 -m pip install segno

BENUTZUNG
---------
    ./geraetezettel.py --von http://192.168.1.42 --token <token>
    ./geraetezettel.py --serie 089C --ap-schluessel a2s78b7rrkfw \\
                       --token <token> --ota <ota-passwort>

Ohne `--aus` landen die Dateien neben diesem Werkzeug.
"""

import argparse
import pathlib
import re
import sys
import urllib.request

try:
    import segno
except ImportError:
    sys.exit("segno fehlt.  python3 -m pip install segno")


def blatt_holen(adresse: str, token: str) -> dict:
    """Das Geraeteblatt von einer laufenden Uhr holen und zerlegen."""
    ziel = adresse.rstrip("/") + "/test/geraeteblatt"
    req = urllib.request.Request(ziel)
    if token:
        req.add_header("X-Starwatch-Token", token)
    try:
        roh = urllib.request.urlopen(req, timeout=10).read().decode()
    except Exception as e:
        sys.exit(f"{ziel} nicht erreichbar: {e}\n"
                 f"Ohne --token geht es nur, solange du am Zugangspunkt der "
                 f"Uhr haengst.")

    def feld(muster):
        m = re.search(muster, roh)
        return m.group(1).strip() if m else ""

    return {
        "geraet": feld(r"Geraet\s+(\S+)"),
        "serie": feld(r"Seriennummer\s+(\S+)"),
        "ota": feld(r"OTA-Passwort\s+(\S+)"),
        "ap_schluessel": feld(r"dessen Schluessel\s+(\S+)"),
        "token": feld(r"Zugriffstoken\s+(\S+)"),
    }


def zettel_svg(daten: dict) -> str:
    """Der Aufkleber: QR plus das Noetigste im Klartext.

    Bewusst SVG und nicht PNG: es laesst sich in jeder Groesse drucken, ohne
    dass der QR ausfranst - und ein ausgefranster QR wird nicht gelesen.
    """
    nutzlast = (f"WIFI:T:WPA;S:StarWatch-{daten['serie']};"
                f"P:{daten['ap_schluessel']};;")
    qr = segno.make(nutzlast, error="m")
    # Nur den QR als SVG-Fragment, damit er sich einbetten laesst.
    # segno schreibt BYTES - deshalb BytesIO und danach dekodieren.
    import io as _io
    puffer = _io.BytesIO()
    qr.save(puffer, kind="svg", xmldecl=False, svgns=False, scale=4,
            border=0, dark="#000000")
    qr_teil = puffer.getvalue().decode("utf-8")

    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="60mm" height="30mm"
     viewBox="0 0 240 120">
  <rect width="240" height="120" fill="#ffffff"/>
  <g transform="translate(8,8)">{qr_teil}</g>
  <g font-family="Helvetica,Arial,sans-serif" fill="#000000">
    <text x="118" y="26" font-size="15" font-weight="bold">Star Watch</text>
    <text x="118" y="46" font-size="13">{daten['geraet']}</text>
    <text x="118" y="70" font-size="10">Zum Einrichten den Code scannen</text>
    <text x="118" y="84" font-size="10">oder mit WLAN verbinden:</text>
    <text x="118" y="99" font-size="11" font-weight="bold">StarWatch-{daten['serie']}</text>
    <text x="118" y="113" font-size="10">Schluessel: {daten['ap_schluessel']}</text>
  </g>
</svg>
"""


def blatt_text(daten: dict) -> str:
    return f"""STAR WATCH - GERAETEBLATT {daten['serie']}
{'=' * 40}

DIESES BLATT BLEIBT BEIM ERBAUER. Es enthaelt Geheimnisse, die NICHT auf den
Zettel am Geraet gehoeren. Ohne es ist die Uhr in zwei Jahren nicht mehr
erreichbar (AP 6.2).

  Geraet             {daten['geraet']}
  Seriennummer       {daten['serie']}

  Zugangspunkt       StarWatch-{daten['serie']}
  dessen Schluessel  {daten['ap_schluessel']}     <- steht auch auf dem Zettel

  Zugriffstoken      {daten['token']}
  OTA-Passwort       {daten['ota']}

  Firmware einspielen:
    STARWATCH_OTA_PW={daten['ota']} \\
      ~/.platformio/penv/bin/pio run -e starwatch_ota -t upload

HINWEIS: Token und OTA-Passwort werden beim ERSTEN Start gewuerfelt und liegen
im Fertigungsbereich - sie ueberleben Werksreset und Update. Geht dieses Blatt
verloren, hilft nur noch der Werksreset durch fuenf Stromzyklen; der Zugang
ueber den eigenen Zugangspunkt bleibt dann der einzige Weg.
"""


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--von", help="Adresse einer laufenden Uhr, z. B. http://192.168.1.42")
    p.add_argument("--token", default="", help="Zugriffstoken (entfaellt am Zugangspunkt)")
    p.add_argument("--serie")
    p.add_argument("--ap-schluessel")
    p.add_argument("--ota", default="")
    p.add_argument("--aus", default=".", help="Zielordner")
    a = p.parse_args()

    if a.von:
        d = blatt_holen(a.von, a.token)
    elif a.serie and a.ap_schluessel:
        d = {"geraet": f"starwatch-{a.serie}", "serie": a.serie,
             "ap_schluessel": a.ap_schluessel, "token": a.token, "ota": a.ota}
    else:
        p.error("entweder --von, oder --serie und --ap-schluessel")

    if not d["serie"] or not d["ap_schluessel"]:
        sys.exit("Seriennummer oder Zugangspunkt-Schluessel fehlen.")

    ziel = pathlib.Path(a.aus)
    ziel.mkdir(parents=True, exist_ok=True)
    zettel = ziel / f"zettel-{d['serie']}.svg"
    blatt = ziel / f"geraeteblatt-{d['serie']}.txt"
    zettel.write_text(zettel_svg(d))
    blatt.write_text(blatt_text(d))

    print(f"{zettel}   <- ausdrucken und auf die Rueckseite kleben")
    print(f"{blatt}   <- BEHALTEN, enthaelt Token und OTA-Passwort")
    print()
    print("Der QR fuehrt ins WLAN der Uhr. Das Token steht NICHT darauf - es")
    print("wandert beim Koppeln von selbst in den Browser.")


if __name__ == "__main__":
    main()
