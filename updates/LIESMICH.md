# Update-Images für die Uhren

Hier liegt, was eine ausgelieferte Uhr sich selbst holt:

```
manifest.json        Kanal stabil — verschenkte Uhren
manifest-beta.json   Kanal beta   — die eigene Uhr
```

Signiert wird im **Schema `starwatch-fw-3`** (seit 28.09.2026). Das Schema
ist zugleich die Hardware-Prüfung: die Alpha-Platinen kennen fw-3 nicht und
weisen es ab, die heutige Uhr nimmt nur fw-3 an. Liegt hier noch ein altes
Manifest der Alpha B (fw-1), schadet es also nicht — es wird beim nächsten
Veröffentlichen einfach überschrieben. Eine Uhr, die sich noch einen älteren
Manifestnamen aus diesem Ordner gemerkt hat, stellt ihn beim Start selbst um.

Die Uhr lädt das darin genannte Image — aber **nur**, wenn die Signatur zu
einem der zwei öffentlichen Schlüssel passt, die in ihrer Firmware stehen.

## Zwei Repositories, und warum

| | |
|---|---|
| `star-watch` | **privat.** Alles: Firmware, Docs, CAD-Verweise, Historie, Zweig `beta` |
| `star-watch-firmware` | **öffentlich.** Nur der Firmware-Quelltext und diese Manifeste |

Das Projektrepo ist privat, weil die Konstruktionsunterlagen darin liegen. Ein privates Repo antwortet einem Gerät ohne Anmeldung mit **404** —
die Uhr könnte dort also nie ein Update finden. Deshalb zeigt
`UPDATE_URL_VORGABE` auf das öffentliche Firmware-Repo:

    https://raw.githubusercontent.com/howtobanana/star-watch-firmware/main/updates/manifest.json

> **Achtung Zweig.** Die Adresse enthält `/main/`. Im öffentlichen Repo gibt es
> nur `main` — dort landet je Veröffentlichung ein Commit. Liegt eine Datei
> nicht dort, meldet die Uhr `HTTP 404` und tut nichts. Ungefährlich, aber es
> passiert eben auch kein Update.

---

## Eine neue Fassung veröffentlichen

**1. Version erhöhen** in `firmware/lib/geraet/geraet.h`:

    constexpr const char *FIRMWARE_VERSION = "1.0.0";

**2. Bauen** — für die Auslieferung immer `starwatch_produkt`, sonst gibt das
Gerät seine Geheimnisse im Klartext aus:

    ~/.platformio/penv/bin/pio run -e starwatch_produkt

**3. Öffentliches Repo auffrischen** — mit dem Werkzeug aus dem privaten Repo
(spiegelt nur von Git verfolgte Dateien, Geheimnisse können so nicht mitgehen):

    python3 werkzeuge/veroeffentlichen.py

**4. Image hineinlegen** — der Name trägt die Version, damit alte Fassungen
erreichbar bleiben:

    cp ~/.pio-builds/star-watch/starwatch_produkt/firmware.bin \
       ../Star-Watch-Firmware/updates/firmware-1.0.0.bin

**5. Manifest erzeugen und signieren** — aus `firmware/werkzeuge/`:

    python3 signieren.py manifest ../../Star-Watch-Firmware/updates/firmware-1.0.0.bin 1.0.0 \
      "https://raw.githubusercontent.com/howtobanana/star-watch-firmware/main/updates/firmware-1.0.0.bin" \
      --schluessel fw-signaturschluessel.pem \
      --mindest 0.9.0 \
      --aus ../../Star-Watch-Firmware/updates/manifest.json

Für eine Vorabfassung dasselbe mit `--aus …/manifest-beta.json`. Die
Version muss `MAJOR.MINOR.PATCH` sein — `signieren.py` weist Anhängsel wie
`-beta1` ab.

**6. Gegenprobe** — macht genau das, was die Uhr macht:

    python3 signieren.py pruefen ../../Star-Watch-Firmware/updates/manifest.json \
      ../../Star-Watch-Firmware/updates/firmware-1.0.0.bin fw-signaturschluessel.pub.pem

**7. Im öffentlichen Repo committen und pushen.**

> **`raw.githubusercontent.com` speichert rund fünf Minuten zwischen.** Direkt
> nach dem Push liefert es noch das alte Manifest — am 25.08.2026 gemessen:
> das Repo zeigte bereits 0.9.3, die URL noch 0.9.2. Wer gleich danach `yp`
> auf der Uhr auslöst, sieht das Alte und hält es für einen Fehler. Fünf
> Minuten warten, oder mit `git show origin/main:updates/manifest.json`
> nachsehen, was wirklich im Repo liegt.

---

## Welche Uhr welchen Kanal liest

    yk s    stabil — so gehen Uhren aus dem Haus
    yk b    beta   — nur die eigene
    y       zeigt Modus, Kanal und Manifest-URL

Der Kanal ist **kein eigener Speicherwert**: er wird aus der Manifest-URL
abgelesen. Damit kann es keine Uhr geben, die „beta" gespeichert hat, aber auf
das stabile Manifest zeigt.

## Was `--mindest` bedeutet

Die niedrigste Version, die **direkt** auf diese springen darf. Solange nichts
umgewandelt werden muss, bleibt sie niedrig. Muss eine Zwischenfassung etwas
umbauen (etwa an den NVS-Werten), wird `--mindest` auf genau diese
Zwischenfassung gesetzt — dann überspringt niemand sie.

## Was hier NICHT hineingehört

- **Kein privater Schlüssel.** `.gitignore` fängt `*.pem`, aber verlass dich
  nicht darauf: schau vor dem Hochladen hin.
- **Keine Dokumente.** Das öffentliche Repo trägt Quelltext und Manifeste,
  sonst nichts. `veroeffentlichen.py` sorgt dafür — von Hand kopieren ist der
  Weg, auf dem versehentlich ein Briefing mitwandert.
- **Keine Bauartefakte.** Nur die Images, die wirklich ausgeliefert werden.

## Wenn ein Update schiefgeht

Die Uhr fängt das selbst ab. Ein frisches Image läuft **auf Bewährung** und
bestätigt sich erst, wenn WLAN steht, die Zeit gültig ist und eine
Referenzfahrt gelaufen ist. Tut es das nicht, fährt der Bootloader beim
nächsten Start die vorige Fassung. Am 24.08.2026 mit einem absichtlich
untauglichen Image nachgewiesen.

Der Zustand steht im Kommando `y`:

    Partition: app0, bestaetigt          <- alles gut
    Partition: app1, AUF BEWAEHRUNG …    <- läuft noch zur Probe
