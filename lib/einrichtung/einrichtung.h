// Star Watch - Einrichtung per Bluetooth LE (23.10.2026).
//
// ZWEITER WEG NEBEN DEM OFFENEN ZUGANGSPUNKT, nicht statt seiner: der
// Zugangspunkt geht mit jedem Browser, Bluetooth braucht eine App. Beide gelten
// genau gleich lange - im Einrichtungszustand, also ohne gespeichertes WLAN -
// und nehmen dieselbe Angabe an (Testweb::wlan_setzen).
//
// PROTOKOLL, ein Dienst mit drei Merkmalen (UUIDs unten):
//   netze   lesen      JSON-Liste der Netze in Reichweite (netze_json)
//   wlan    schreiben  "SSID\nPasswort" - oder "?" fuer einen neuen Scan
//   status  lesen/melden  {"stand":"bereit|suche|verbinde|verbunden|fehler",...}
//                      "verbunden" traegt "ip" und "token" - die Kopplung,
//                      wie am Ende der Einrichtung im Browser.
//
// KOSTEN: rund 266 KB Flash (NimBLE, gemessen 23.10.2026). RAM nur, solange
// es laeuft: gestartet wird es nur im Einrichtungszustand, und danach gibt
// beenden() den Speicher endgueltig frei - bis zum naechsten Neustart.
#pragma once

#include <Arduino.h>

namespace starwatch {

class BleEinrichtung {
 public:
  static constexpr const char *DIENST = "f237da8b-b7e7-40d2-8da6-8c16471ab374";
  static constexpr const char *NETZE = "b9c63373-b2de-47c1-9690-e763bc0833bb";
  static constexpr const char *WLAN = "29b1ef99-1ece-4e4a-a72a-b59598b8857a";
  static constexpr const char *STATUS = "7371cf7f-a331-4e1b-b822-f07d0e5778a1";

  /// Werben unter `name`. Geht nur einmal je Start - siehe beenden().
  void starten(const String &name, const String &netze_json);
  /// Abschalten und den Speicher des Bluetooth-Stapels freigeben.
  void beenden();
  bool laeuft() const { return laeuft_; }
  bool beendet() const { return beendet_; }

  /// Neue Netzliste (nach einem Scan).
  void netze(const String &json);
  /// Status setzen und an eine verbundene App melden.
  void melden(const String &json);

  /// Hat die App etwas geschrieben? Aufrufen aus loop(), nie aus dem
  /// Bluetooth-Task: wlan_setzen() blockiert Sekunden.
  /// true = "SSID\nPasswort" in `daten`; `scan` = "?" (neu suchen).
  bool auftrag_abholen(String *daten, bool *scan);

 private:
  bool laeuft_ = false;
  bool beendet_ = false;
};

}  // namespace starwatch
