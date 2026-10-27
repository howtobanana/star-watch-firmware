// ---------------------------------------------------------------------------
// Star Watch - lib/antrieb: Motor und Treiber. AP 2.2.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Impulse und Register. Nichts sonst.
//
//   - FastAccelStepper fuer die Schritte: MCPWM in Hardware, Rampen und
//     Positionszaehler ueber PCNT. NICHT die Hauptschleife - im Pruefstand
//     haengen die Schrittabstaende an ihr, und der Lauf ruckelt deshalb.
//   - TMCStepper fuer die Register ueber UART.
//
// OHNE INDEX. FastAccelStepper zaehlt seine eigenen Impulse in Hardware;
// eine Rueckmeldeleitung braucht dieser Weg nicht. Warum INDEX bei diesem
// Aufbau nichts liefert, steht in docs/Arbeitsstand.md Abschnitt 4 - es ist
// fuer das Endgeraet ohne Folgen und keine offene Baustelle.
//
// ===========================================================================
// GRENZE
// ===========================================================================
// Diese Bibliothek faehrt auf ein absolutes Ziel und sagt, wo sie steht.
// Sie weiss nicht, wie spaet es ist, und sie entscheidet nicht, in welche
// Richtung gefahren wird - das tut lib/zeiger (delta_vorwaerts) und
// lib/betrieb. Wer hier eine Positionsformel schreibt, schreibt sie an eine
// Stelle, die kein Host-Test erreicht.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

#include <FastAccelStepper.h>
#include <TMCStepper.h>

namespace starwatch {

/// Was der Treiber zuletzt gemeldet hat. Reihenfolge nach Dringlichkeit.
enum class Treiberzustand {
  schweigt,          ///< keine gueltige Antwort auf dem UART
  ok,
  war_spannungslos,  ///< Reset - die Register sind auf Werkseinstellung
  zu_warm,           ///< Vorwarnung, laeuft noch
  uebertemperatur,   ///< Endstufe hat abgeschaltet
  wicklung_offen,    ///< Stecker?
  kurzschluss,
  fehler,
};

/// Kurztext zum Zustand, ohne Umlaute (geht auch ueber die serielle Konsole).
const char *treiberzustand_text(Treiberzustand z);

class Antrieb {
 public:
  // -------------------------------------------------------------------------
  // Aufbau
  // -------------------------------------------------------------------------

  /// UART aufbauen, Register schreiben, Impulserzeuger starten.
  ///
  /// REIHENFOLGE IST WESENTLICH und am Aufbau gelernt: die Register werden
  /// geschrieben, BEVOR die Endstufe eingeschaltet wird. Sonst gilt bis dahin
  /// der Werkszustand - I_scale_analog=1, IRUN am Anschlag, Strom nach
  /// Trimmpoti, ab Werk rund 0,9 A statt der geplanten 0,4 A.
  ///
  /// Gibt false zurueck, wenn der Treiber nicht antwortet. Der Antrieb ist
  /// dann trotzdem benutzbar - die Impulse kommen aus dem ESP32, und der
  /// Strom steht dank Vref 0,55 V auch ohne UART in einem sicheren Bereich.
  /// Das ist Absicht: ein schweigender UART soll das Geraet nicht lahmlegen.
  bool beginn();

  /// Den ganzen Registersatz neu schreiben. Nach `war_spannungslos` faellig.
  void register_setzen(uint16_t mikroschritte);

  /// IC-Version aus dem Chip. 0x21 ist ein TMC2209, 0x20 ein TMC2208.
  uint8_t ic_version();

  /// Antwortet der Treiber auf dem UART?
  bool treiber_antwortet() { return ic_version() == 0x21; }

  // -------------------------------------------------------------------------
  // Endstufe
  // -------------------------------------------------------------------------

  /// Endstufe ein oder aus. AUS heisst Freilauf, und dann kippt die Klinge
  /// aus ihrer Lage - nur zum Messen benutzen, nie im Betrieb.
  void endstufe(bool ein);
  bool endstufe_an() const { return endstufe_an_; }
  /// Wie oft die Endstufe seit dem Start abgeschaltet wurde. Im Freilauf
  /// kippt die Klinge, und der Zaehler weiss nichts davon - eine Abweichung
  /// bei der naechsten Referenzfahrt ist dann KEIN Schrittverlust.
  uint32_t freilaeufe() const { return freilaeufe_; }

  // -------------------------------------------------------------------------
  // Fahren. Alle Positionen sind Mikroschritte, nie Grad.
  // -------------------------------------------------------------------------

  void tempo(uint32_t schritte_je_s);
  void rampe(uint32_t beschleunigung, uint32_t bremsung);

  /// Tempo in MILLI-Schritten je Sekunde, und zwar SOFORT - auch auf eine
  /// laufende Fahrt, ohne neues Ziel. Fuer die gleitende Timerfahrt: ein
  /// 12-h-Timer braucht 74 mHz, das geht in ganzen Hertz nicht.
  ///
  /// Gibt false zurueck, wenn FastAccelStepper den Wert ablehnt - langsamer
  /// als rund 250 Mio. Ticks je Schritt (15,6 s) geht nicht. Dann ist nichts
  /// veraendert.
  bool tempo_mhz(uint32_t milli_hz);

  /// Was gerade gilt. Gemerkt statt aus FastAccelStepper zurueckgelesen -
  /// die Bibliothek gibt beides nicht wieder her, und fuer die Anzeige und
  /// die Timer-Mindestdauer muss es abfragbar sein.
  uint32_t tempo() const { return tempo_; }
  uint32_t beschleunigung() const { return beschleunigung_; }

  /// Auf eine absolute Position fahren - mit der eingestellten Rampe, OHNE
  /// weichen Einsatz. Fuer Referenzfahrt, Grenzsuche und Prueflaeufe: deren
  /// Messungen haengen am genauen Bewegungsablauf.
  void fahre_zu(int32_t position);

  /// Auf eine absolute Position fahren, WEICH (28.09.2026) - fuer alles, was
  /// man an der Wand sieht: Uhrbetrieb, Timer, Handfahrten. Die
  /// Beschleunigung setzt allmaehlich ein (weicher_einsatz), und kurze
  /// Spruenge dauern mindestens WEICH_MIN_MS (weiche_beschleunigung) - der
  /// Zeiger gleitet, statt zu zucken. Die eingestellte Rampe bleibt die
  /// Obergrenze.
  void fahre_weich_zu(int32_t position);

  /// Um eine Anzahl Schritte weiterfahren, bezogen auf das ZIEL - nicht auf
  /// die Istposition. Laeuft gerade eine Fahrt, wird ihr Ziel weitergedreht
  /// statt sie abzuschneiden; genau so macht es der Uhrbetrieb im Pruefstand.
  void fahre_weiter(int32_t schritte);
  /// Wie fahre_weiter, aber weich - siehe fahre_weich_zu.
  void fahre_weich_weiter(int32_t schritte);

  int32_t position() const;
  int32_t ziel() const;
  bool laeuft() const;
  bool ziel_erreicht() const;

  /// Geregelt herunterbremsen. NICHT hart abschneiden - ein Stillstand ohne
  /// Rampe reisst dem Riemen einen Zahn ab, und das sind 18 Grad = 36 min
  /// Anzeigefehler.
  void stopp_mit_rampe();

  /// Zaehler auf einen Wert setzen, ohne zu fahren. Das ist die Referenz, die
  /// die Referenzfahrt setzt. Nur im STILLSTAND erlaubt - waehrend der Fahrt
  /// zaehlt die Hardware weiter und das Ergebnis ist beliebig.
  /// Gibt false zurueck, wenn der Motor lief; dann ist nichts passiert.
  bool zaehler_setzen(int32_t position);

  // -------------------------------------------------------------------------
  // Diagnose. JEDER dieser Aufrufe liest Register ueber den UART und
  // blockiert dabei rund 100 ms.
  //
  // Im Pruefstand war genau das die Ursache eines Ruckelns: zwei Sensoren
  // alle 5 s hiessen, dass der Zeiger alle 5 s zwei Zehntelsekunden stand,
  // weil die Impulse damals aus der Hauptschleife kamen. Hier kommen sie aus
  // der Hardware, das Ruckeln entfaellt - die 100 ms Blockade bleiben aber.
  // Also auf Anforderung lesen, nicht im Takt.
  // -------------------------------------------------------------------------

  /// Tatsaechlicher Motorstrom in mA RMS, aus dem Chip gerechnet.
  uint16_t strom_ma();

  /// Haltestrom in mA RMS zur Laufzeit setzen (01.10.2026). Er fliesst rund
  /// um die Uhr - ohne Licht der groesste Verbraucher. Begrenzt auf
  /// HALTESTROM_MIN..STROM_LAUF; nie 0, sonst kippt die Klinge.
  void haltestrom(uint16_t ma);
  uint16_t haltestrom() const { return halt_ma_; }

  /// StallGuard-Rohwert. Faellt, wenn der Motor an seine Grenze kommt - bei
  /// der Grenzsuche das interessanteste Signal, weil es den Schrittverlust
  /// ankuendigt, bevor er hoerbar wird.
  uint16_t last();

  uint32_t drv_status();

  /// Zustand aus DRV_STATUS und GSTAT. Liest ueber UART.
  Treiberzustand zustand();

  /// Eingestellte Mikroschritte, aus dem Chip gelesen.
  uint16_t mikroschritte();

  /// stealthChop (false) oder spreadCycle (true).
  void spreadcycle(bool ein);

  /// Mikroschrittzaehler des Treibers (MSCNT, 0..1023 in 1/256-Schritten).
  /// Zaehlt die STEP-Impulse, die WIRKLICH am Chip ankamen - die Differenz zum
  /// eigenen Zaehler trennt verlorene Impulse von einem rutschenden Rotor
  /// (27.09.2026). Nur im Stillstand aussagekraeftig.
  uint16_t mscnt();

 private:
  /// Strom aus dem CS-Wert. TMC2209:
  ///   I_rms = (CS+1)/32 * Vfs/(Rsense+0,02) / sqrt(2)
  /// `vsense` waehlt Vfs und wird von TMCStepper selbst gesetzt, also aus dem
  /// Chip lesen und nicht annehmen.
  uint16_t ma_aus_cs(uint8_t cs);

  TMC2209Stepper *tmc_ = nullptr;
  FastAccelStepperEngine engine_ = FastAccelStepperEngine();
  FastAccelStepper *motor_ = nullptr;
  bool endstufe_an_ = false;
  uint32_t freilaeufe_ = 0;

  // mutable, weil ziel() den abgebrochenen Lauf nachzieht - das ist ein
  // Zwischenspeicher, kein Zustand nach aussen.
  mutable int32_t ziel_ = 0;
  mutable bool stopp_angefordert_ = false;

  uint32_t tempo_ = 0;
  uint32_t beschleunigung_ = 0;
  uint16_t halt_ma_ = 0;
};

}  // namespace starwatch
