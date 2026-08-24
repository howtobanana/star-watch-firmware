#!/usr/bin/env python3
"""Star Watch - Firmware signieren und Manifest erzeugen.

Entschieden am 22.08.2026 (Umsetzungsplan 3.7.5): Update-Images werden
SIGNIERT. Nicht Secure Boot, nicht nur HTTPS - eine Signatur, die die Uhr
selbst prueft, bevor sie ein Image aktiviert.

WARUM DIESE STUFE
-----------------
Zieht die Uhr selbstaendig Firmware von einer URL, kann jeder, der DNS oder
das Netz kontrolliert, beliebigen Code auf ein Geraet in einer fremden
Wohnung spielen. HTTPS allein bindet das Geraet an ein Zertifikat mit
Ablaufdatum; eine Signatur ist vom Transportweg unabhaengig und gilt, solange
der Schluessel gilt.

WAS DIESE STUFE NICHT LEISTET, UND ZWAR BEWUSST
-----------------------------------------------
Sie schuetzt gegen Angriffe UEBER DAS NETZ, nicht gegen physischen Zugriff.
Wer die Uhr aufschraubt und einen Programmer anschliesst, kann den Flash
ueberschreiben - dagegen hilft nur Secure Boot mit Flash-Verschlüsselung, und
das ist irreversibel und bei verlorenem Schluessel Elektroschrott. Fuer eine
Wanduhr ist das die falsche Abwaegung (Briefing Abschnitt 8).

VERFAHREN
---------
Signiert wird NICHT das Image direkt, sondern ein kleiner Textblock, der den
Hash des Images UND die Version enthaelt:

    starwatch-fw-1
    version=<semver>
    groesse=<bytes>
    sha256=<hex>
    mindest=<semver>

Signatur = ECDSA mit P-256 ueber SHA-256 dieses Blocks, als DER, base64.

Zwei Gruende fuer den Umweg ueber den Block statt einer Signatur ueber das
Image:

1. **Die Version ist mitsigniert.** Sonst koennte jemand ein altes, echt
   signiertes Image mit einer erfundenen hohen Versionsnummer im Manifest
   ausliefern - ein Downgrade auf eine Fassung mit bekannter Luecke.
2. **Die URL ist NICHT mitsigniert, und das ist Absicht.** Sie darf sich
   aendern (Spiegel, Umzug des Hostings). Der Schutz liegt am Hash: woher das
   Image kommt, ist gleichgueltig, es muss zum signierten Hash passen.

ZWEI SCHLUESSEL
---------------
Die Firmware kennt ZWEI oeffentliche Schluessel und akzeptiert eine Signatur,
die zu einem von beiden passt. Der zweite liegt offline und wird nie zum
Signieren benutzt - er ist der Notausgang, falls der erste kompromittiert
wird. Ohne ihn muesste jedes Geraet wieder ans Kabel.

DER PRIVATE SCHLUESSEL
----------------------
Gehoert NICHT in dieses Repository (`.gitignore` faengt `*.pem`). Er gehoert
auf ein Medium, das nicht am Netz haengt, mit einer Kopie an einem zweiten
Ort. Geht er verloren, sind keine Updates mehr moeglich; wird er gestohlen,
sind alle Geraete angreifbar.

BENUTZUNG
---------
    ./signieren.py schluessel                       einmalig, erzeugt das Paar
    ./signieren.py header  fw-signaturschluessel.pub.pem [zweiter.pub.pem]
    ./signieren.py manifest  firmware.bin  1.4.2  https://…/fw-1.4.2.bin \\
                   --schluessel fw-signaturschluessel.pem  --mindest 1.0.0
    ./signieren.py pruefen  manifest.json  firmware.bin  fw-…pub.pem

Der letzte Aufruf macht genau das, was die Uhr macht - damit laesst sich das
Verfahren ohne Geraet pruefen.
"""

import argparse
import base64
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile

SCHEMA = "starwatch-fw-1"
KURVE = "prime256v1"  # NIST P-256, im Arduino-Core als
                      # CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED verfuegbar


def openssl(*args: str, eingabe: bytes | None = None) -> bytes:
    """openssl aufrufen. Bewusst das Kommandozeilenwerkzeug: es ist auf jedem
    Rechner da, und fuer Signaturen ist das die konservative Wahl.

    NACHTRAG 24.08.2026: die urspruengliche Begruendung lautete "es gibt keine
    Abhaengigkeit, die in zwei Jahren fehlt" - und die war zu streng. Auf
    Festlegung des Projektinhabers gilt: nicht jede Bibliothek verschwindet,
    bei grossen und etablierten laesst sich darauf bauen. `geraetezettel.py`
    benutzt deshalb `segno`. Fuer die Signatur bleibt es trotzdem bei openssl -
    dort ist Vorsicht angebracht, nicht Bequemlichkeit."""
    e = subprocess.run(["openssl", *args], input=eingabe, capture_output=True)
    if e.returncode != 0:
        sys.exit(f"openssl {' '.join(args)} fehlgeschlagen:\n"
                 f"{e.stderr.decode(errors='replace')}")
    return e.stdout


def semver(s: str) -> tuple[int, int, int]:
    m = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", s)
    if not m:
        sys.exit(f"Version '{s}' ist nicht MAJOR.MINOR.PATCH.")
    return tuple(int(x) for x in m.groups())  # type: ignore[return-value]


def block_bauen(version: str, groesse: int, sha256_hex: str, mindest: str) -> bytes:
    """Der signierte Block. Genau diese Bytefolge - kein JSON, keine
    Kanonisierung, keine Ueberraschung. Die Firmware baut sie identisch
    zusammen und prueft die Signatur darueber."""
    semver(version)
    semver(mindest)
    return (f"{SCHEMA}\n"
            f"version={version}\n"
            f"groesse={groesse}\n"
            f"sha256={sha256_hex}\n"
            f"mindest={mindest}\n").encode()


def sha256_datei(pfad: pathlib.Path) -> tuple[str, int]:
    h = hashlib.sha256()
    n = 0
    with pfad.open("rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
            n += len(chunk)
    return h.hexdigest(), n


# ---------------------------------------------------------------------------


def cmd_schluessel(a: argparse.Namespace) -> None:
    priv = pathlib.Path(a.name + ".pem")
    pub = pathlib.Path(a.name + ".pub.pem")
    if priv.exists():
        sys.exit(f"{priv} liegt schon da. NICHT ueberschreiben - ein neuer "
                 f"Schluessel macht jedes bereits ausgelieferte Geraet "
                 f"update-unfaehig, solange der alte oeffentliche Schluessel "
                 f"in seiner Firmware steht.")
    openssl("ecparam", "-name", KURVE, "-genkey", "-noout", "-out", str(priv))
    priv.chmod(0o600)
    pub.write_bytes(openssl("ec", "-in", str(priv), "-pubout"))
    print(f"privat  {priv}   <- NICHT ins Repository, Kopie an einen zweiten Ort")
    print(f"oeffentlich  {pub}   <- gehoert in die Firmware")
    print("\nZWEITEN Schluessel als Notausgang erzeugen (und offline legen):")
    print(f"  ./signieren.py schluessel --name {a.name}-notfall")


def cmd_header(a: argparse.Namespace) -> None:
    """Die oeffentlichen Schluessel als C-Header, zum Einbetten."""
    teile = []
    for i, p in enumerate(a.pubkey):
        pfad = pathlib.Path(p)
        pem = pfad.read_text().strip()
        if "PUBLIC KEY" not in pem:
            sys.exit(f"{pfad} sieht nicht wie ein oeffentlicher Schluessel aus.")
        zeilen = "\n".join(f'    "{z}\\n"' for z in pem.splitlines())
        teile.append(f"// {pfad.name}\nstatic const char FW_PUBKEY_{i}[] =\n{zeilen};")

    liste = ", ".join(f"FW_PUBKEY_{i}" for i in range(len(a.pubkey)))
    print(f"""// ---------------------------------------------------------------------------
// Star Watch - oeffentliche Schluessel fuer die Pruefung von Update-Images.
// ERZEUGT von werkzeuge/signieren.py - nicht von Hand aendern.
//
// Diese Datei enthaelt KEIN Geheimnis: oeffentliche Schluessel duerfen im
// Repository liegen und in jedem ausgelieferten Geraet stehen.
//
// Der zweite Schluessel ist der Notausgang. Er wird nie zum Signieren
// benutzt und liegt offline - wird der erste kompromittiert, laesst sich
// damit ein Update ausliefern, das den ersten entfernt. Ohne ihn muesste
// jedes Geraet wieder ans Kabel.
// ---------------------------------------------------------------------------
#pragma once

{chr(10).join(teile)}

static const char *FW_PUBKEYS[] = {{{liste}}};
static const size_t FW_PUBKEY_ANZAHL = {len(a.pubkey)};""")


def cmd_manifest(a: argparse.Namespace) -> None:
    bild = pathlib.Path(a.image)
    if not bild.is_file():
        sys.exit(f"{bild} gibt es nicht.")
    sha, groesse = sha256_datei(bild)
    block = block_bauen(a.version, groesse, sha, a.mindest)

    with tempfile.NamedTemporaryFile() as t:
        t.write(block)
        t.flush()
        sig = openssl("dgst", "-sha256", "-sign", a.schluessel, t.name)

    manifest = {
        "schema": SCHEMA,
        "version": a.version,
        "groesse": groesse,
        "sha256": sha,
        "mindest": a.mindest,
        "kanal": a.kanal,
        "url": a.url,
        "signatur": base64.b64encode(sig).decode(),
    }
    ziel = pathlib.Path(a.aus)
    ziel.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{ziel} geschrieben.")
    print(f"  Version {a.version}, {groesse} Bytes, sha256 {sha[:16]}…")
    print(f"  mindestens laufende Version {a.mindest} fuer den direkten Sprung")
    print("\nGegenprobe - macht genau das, was die Uhr macht:")
    print(f"  ./signieren.py pruefen {ziel} {bild} <oeffentlicher-schluessel>")


def cmd_pruefen(a: argparse.Namespace) -> None:
    m = json.loads(pathlib.Path(a.manifest).read_text())
    fehler: list[str] = []

    if m.get("schema") != SCHEMA:
        fehler.append(f"fremdes Schema '{m.get('schema')}', erwartet '{SCHEMA}'")

    sha, groesse = sha256_datei(pathlib.Path(a.image))
    if m.get("sha256") != sha:
        fehler.append(f"Hash weicht ab: Manifest {m.get('sha256')}, Datei {sha}")
    if m.get("groesse") != groesse:
        fehler.append(f"Groesse weicht ab: Manifest {m.get('groesse')}, Datei {groesse}")

    if not fehler:
        block = block_bauen(m["version"], m["groesse"], m["sha256"], m["mindest"])
        with tempfile.NamedTemporaryFile() as tb, tempfile.NamedTemporaryFile() as ts:
            tb.write(block)
            tb.flush()
            ts.write(base64.b64decode(m["signatur"]))
            ts.flush()
            e = subprocess.run(
                ["openssl", "dgst", "-sha256", "-verify", a.pubkey,
                 "-signature", ts.name, tb.name],
                capture_output=True)
        if e.returncode != 0:
            fehler.append("SIGNATUR PASST NICHT zu diesem oeffentlichen Schluessel")

    if fehler:
        print("ABGELEHNT:")
        for f in fehler:
            print(f"  - {f}")
        sys.exit(1)
    print(f"OK. Version {m['version']}, {m['groesse']} Bytes, Signatur gueltig.")
    print(f"Die Uhr wuerde dieses Image einspielen, wenn ihre laufende Version")
    print(f"mindestens {m['mindest']} ist und kleiner als {m['version']}.")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    u = p.add_subparsers(dest="cmd", required=True)

    s = u.add_parser("schluessel", help="Schluesselpaar erzeugen (einmalig)")
    s.add_argument("--name", default="fw-signaturschluessel")
    s.set_defaults(fn=cmd_schluessel)

    h = u.add_parser("header", help="oeffentliche Schluessel als C-Header")
    h.add_argument("pubkey", nargs="+", help="ein oder zwei .pub.pem")
    h.set_defaults(fn=cmd_header)

    m = u.add_parser("manifest", help="Image signieren, Manifest schreiben")
    m.add_argument("image")
    m.add_argument("version")
    m.add_argument("url")
    m.add_argument("--schluessel", required=True)
    m.add_argument("--mindest", default="0.0.0",
                   help="kleinste laufende Version fuer den direkten Sprung")
    m.add_argument("--kanal", default="stabil", choices=["stabil", "test"])
    m.add_argument("--aus", default="manifest.json")
    m.set_defaults(fn=cmd_manifest)

    v = u.add_parser("pruefen", help="Manifest gegen Image und Schluessel pruefen")
    v.add_argument("manifest")
    v.add_argument("image")
    v.add_argument("pubkey")
    v.set_defaults(fn=cmd_pruefen)

    a = p.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
