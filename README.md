# Star Watch — Firmware

Firmware einer Wanduhr, deren Zeiger eine leuchtende Klinge ist. Ein
Schrittmotor dreht sie, ein Hall-Sensor gibt ihr einen absoluten Nullpunkt,
eine Konstantstromquelle regelt das Licht. Läuft auf einem **ESP32 D1 Mini**.

Version 0.9.5 · [Apache-2.0](LICENSE)

> Dies ist der **Firmware-Teil** eines größeren Projekts. Mechanik, Optik, CAD
> und Fertigungsunterlagen gehören nicht dazu und werden nicht veröffentlicht.
> Quelltextkommentare verweisen an rund dreißig Stellen auf Dokumente
> („Briefing Abschnitt 6", „Umsetzungsplan AP 3.7.1"), die es hier nicht gibt —
> die Verweise stehen absichtlich noch drin, weil zwei Fassungen desselben
> Codes zu pflegen der Anfang vom Auseinanderdriften wäre.

---

## Was sie kann

| | |
|---|---|
| **Absoluter Nullpunkt** | Referenzfahrt über einen Hall-Sensor, ausgewertet über den Schwerpunkt des Feldes statt über eine Flanke. Beim Einschalten von selbst, danach täglich |
| **Gangarten** | Minutensprung, fließender Lauf, Standbild |
| **Timer** | mit Meldeblinken, danach zurück auf die Uhrzeit |
| **Effekte** | Kreise vor dem Ziel, weiche Rampen, bis 6 Umdrehungen je Sekunde |
| **Zeit aus dem Netz** | SNTP mit Zeitzonenregel, Zeitbasis wird im Minutentakt nachgezogen. Kein Uhrenbaustein |
| **Einrichtung ohne Kabel** | eigener Zugangspunkt mit Captive Portal, WLAN über den Browser |
| **Signiertes OTA** | ECDSA P-256 über ein Manifest, SHA-256 während des Ladens, zwei Partitionen, Rückrollen bei einem untauglichen Abbild |
| **Zugriffsschutz** | Token je Gerät, bei der Kopplung übergeben. Lesende Endpunkte offen, schreibende geschützt |
| **Werkseinstellungen** | über die Oberfläche oder fünf Stromzyklen hintereinander |

## Aufbau

```
lib/zeiger          reine Rechnung: Winkel, Wege, Zeiten — ohne Hardware, host-testbar
lib/geraet          alle Konstanten und Pins an einer Stelle
lib/antrieb         TMC2209 über UART, Schritte über MCPWM und PCNT
lib/hall            Referenzfahrt und Feldauswertung
lib/licht           LED-Regelung, Effektketten
lib/betrieb         Uhr, Gangarten, Timer, Kollisionsregel, Speicher
lib/testweb         Weboberfläche und WLAN
lib/aktualisierung  Manifest prüfen, Abbild laden, Rückrollen
src/main.cpp        Zusammenbau und Konsole
src/inbetriebnahme.cpp   eigenständiger Sketch zum Ausmessen eines neuen Aufbaus
```

`lib/zeiger` und `lib/geraet` hängen an keiner Hardware — genau die Teile, in
denen Rechenfehler sonst unbemerkt bleiben. Dafür gibt es **73 Tests**, die auf
dem Rechner laufen:

```bash
pio test -e native
```

## Bauen

```bash
pio run -e starwatch -t upload
```

| Umgebung | Wofür |
|---|---|
| `starwatch` | über USB |
| `starwatch_ota` | über das Netz, Passwort je Gerät in `STARWATCH_OTA_PW` |
| `starwatch_produkt` | für die Auslieferung — gibt keine Geheimnisse aus |
| `inbetriebnahme` | Messsketch für einen frischen Aufbau |
| `native` | die Tests |

Die Plattform ist auf **pioarduino 55.03.311** festgenagelt (Arduino 3.3.11,
ESP-IDF 5.5.5). Das ist Absicht: ein Wechsel der Plattform hat hier schon
einmal das Schritterzeugen verändert.

Vor dem ersten Bau `lib/testweb/zugangsdaten.example.h` nach `zugangsdaten.h`
kopieren — oder leer lassen, dann spannt das Gerät seinen eigenen Zugangspunkt
auf und das WLAN wird im Browser eingerichtet.

## Updates

Die Uhr sieht einmal täglich nach, ob es eine neue Fassung gibt:

```
updates/manifest.json        stabil — für ausgelieferte Uhren
updates/manifest-beta.json   beta   — Vorabfassungen
```

Ein Manifest ist erst gültig, wenn die Signatur zu einem der beiden
öffentlichen Schlüssel in `lib/aktualisierung/fw_pubkeys.h` passt. Der private
Schlüssel liegt nicht in diesem Repo und wird es nie. Startet die neue Fassung
nicht sauber, kehrt das Gerät von selbst zur vorigen zurück.

Ablauf einer Veröffentlichung: [`updates/LIESMICH.md`](updates/LIESMICH.md).

## Womit sie spricht

Serielle Konsole und Netzkonsole auf Port 23, `?` zeigt alle Kommandos. Die
Weboberfläche liegt auf Port 80 und ist ein Messgerät, keine API — sie zeigt
Sensorwerte, Zählerstände und ein Zifferblatt mit dem echten Nullpunkt.
