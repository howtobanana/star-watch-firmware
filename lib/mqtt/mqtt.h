#pragma once
// ===========================================================================
// MQTT - der Weg zu Home Assistant (AP 5a)
// ===========================================================================
//
// WARUM MQTT UND KEINE EIGENE HA-INTEGRATION: eine native Integration waere
// Python-Code, den ein Ein-Personen-Projekt gegen ein schnell bewegliches Ziel
// pflegen muesste, solange Uhren im Umlauf sind. MQTT Discovery ist ein
// Nachrichtenformat - einmal richtig geschrieben, laeuft es in fuenf Jahren
// noch. Shelly, Tasmota und Zigbee2MQTT machen es genauso.
//
// WAS FREIGEGEBEN IST, IST BEWUSST WENIG (festgelegt am 26.08.2026):
//
//   light         Klinge, an/aus und Helligkeit
//   switch        Betrieb, an/aus - als `config` markiert, siehe unten
//   button        Melden ausloesen
//   binary_sensor Timer laeuft
//   button        Meldung quittieren          (seit 25.09.2026)
//   binary_sensor Meldung steht               (seit 25.09.2026)
//
// MELDUNGEN (25.09.2026): die Klinge leuchtet in einer Farbe, bis quittiert
// wird oder die Dauer ablaeuft - "Pflanzen giessen" = gelb. Thema
// `<basis>/melden/set`, dasselbe wie der Melden-Knopf:
//
//   {"farbe":"gelb"}                                   ruhig, bis quittiert
//   {"farbe":"#00ff00","muster":"pulsieren","dauer":600,"hell":40}
//   {"farbe":{"r":255,"g":0,"b":255},"muster":"blinken"}
//   {"aus":true}   oder   OFF                          quittieren
//   alles andere (auch das PRESS des Knopfes)          drei Blitze wie bisher
//
//   farbe   rot gruen blau gelb violett weiss orange tuerkis, "#rrggbb"
//           oder {"r":..,"g":..,"b":..}
//   muster  leuchten (Vorgabe) | pulsieren | blinken
//   dauer   Sekunden, 0 oder fehlend = bis quittiert, hoechstens 86400
//   hell    1..100 %, fehlend = der Reglerwert
//
// Zurueck kommt `<basis>/melden/aktiv` (ON/OFF) und `<basis>/melden/state`
// als JSON. Die 2,4-W-Grenze der LED gilt auch hier. Eine Meldung
// UEBERSTEHT KEINEN NEUSTART - wer das braucht, laesst die Automatisierung
// nach `<basis>/status` = online erneut senden.
//
// Jede Entitaet ist ein VERTRAG. Sobald eine Uhr ausgeliefert ist, baut
// jemand Automatisierungen darauf; ein umbenanntes `unique_id` gibt ihm
// doppelte Entitaeten und tote Verweise. Lieber vier, die bleiben, als
// zwanzig, die man bereut.
//
// WARUM DER BETRIEBSSCHALTER `config` IST: "Betrieb aus" macht den Motor
// stromlos, und dann kippt die Klinge aus ihrer Lage - zurueck kommt sie nur
// ueber ein Homing. Home Assistant nimmt Entitaeten dieser Kategorie von
// bereichs- und geraeteweiten Aufrufen aus. Ein `homeassistant.turn_off` auf
// das ganze Wohnzimmer geht damit an ihm vorbei.
//
// NICHT FREIGEGEBEN: Kalibrierung, Treiberregister, Tempo, Rampe, Grenzsuche,
// WLAN, OTA, Werksreset. Das bleibt hinter dem Token auf der Weboberflaeche.
// Was von aussen erreichbar ist, darf die Uhr nicht verstellen.

#include <cstdint>
#include <string>

#include <antrieb.h>
#include <betrieb.h>
#include <licht.h>
#include <lux.h>
#include <testweb.h>

namespace starwatch {

class Mqtt {
 public:
  /// NACH dem WLAN aufzurufen. Verbindet noch nicht - das tut `tick()`,
  /// sobald `Betrieb` einen Broker kennt und eingeschaltet ist.
  /// `lux` darf nullptr sein - der Sensor an J8 ist optional. Dann entfaellt
  /// die Entitaet, statt eine anzulegen, die nie einen Wert bekommt.
  void beginn(Betrieb *betrieb, Licht *licht, Antrieb *antrieb, Lux *lux,
              Kommandofn kommando);

  /// Aus der Hauptschleife. Haelt die Verbindung und meldet Aenderungen.
  void tick();

  bool verbunden() const;

  /// Fuer die Zustandsanzeige: was zuletzt geschah, in einem Satz.
  const std::string &lage() const { return lage_; }

 private:
  void verbinden();
  void angemeldet();
  void discovery_senden();
  void zustand_senden(bool erzwingen);
  void nachricht(const char *thema, const char *inhalt);

  Betrieb *betrieb_ = nullptr;
  Licht *licht_ = nullptr;
  Antrieb *antrieb_ = nullptr;
  Lux *lux_ = nullptr;
  Kommandofn kommando_ = nullptr;

  std::string basis_;   ///< "starwatch/089C"
  std::string kennung_; ///< "starwatch_089C", Stamm aller unique_id
  std::string lage_ = "aus";

  uint32_t letzter_versuch_ms_ = 0;
  bool gestartet_ = false;

  // Zuletzt GESENDETER Zustand. Gemeldet wird nur, was sich geaendert hat -
  // ein Zustandsstrom im Sekundentakt blaeht die Datenbank in Home Assistant
  // auf, ohne irgendjemandem etwas zu sagen.
  bool s_licht_an_ = false;
  int s_hell_ = -1;
  uint32_t s_farbe_ = 0;
  bool s_meld_ = false;
  uint32_t s_meld_farbe_ = 0;
  int s_meld_muster_ = -1;
  bool s_betrieb_an_ = false;
  bool s_timer_ = false;
  bool s_la_an_ = true;
  bool s_la_nacht_ = true;
  bool s_puls_ = false;
  uint32_t s_puls_per_ = 0;
  int s_puls_min_ = -1;
  // Die Beleuchtungsstaerke aendert sich dauernd um Kleinigkeiten. Gemeldet
  // wird erst ab einem Unterschied, der fuer eine Automatisierung ueberhaupt
  // eine Rolle spielt - sonst steht in der Datenbank alle zwei Sekunden eine
  // neue Zeile, die nichts aussagt.
  float s_lux_ = -1.0f;
  bool zustand_gueltig_ = false;
  /// Geraetename der letzten Discovery - aendert er sich, geht sie neu raus.
  std::string s_name_;
};

}  // namespace starwatch
