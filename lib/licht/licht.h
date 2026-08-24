// ---------------------------------------------------------------------------
// Star Watch - lib/licht: LED und Effekte. AP 2.5.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Das Tastverhaeltnis am LD24AJTA (PT4115) auf GPIO33, LEDC. Der STROM steht
// am Trimmer des Moduls und ist keine Sache der Firmware - hier kommt nur die
// Helligkeit her. DIM ist active HIGH: unter 0,3 V aus, ueber 2,5 V voll.
//
// Helligkeit nach aussen in PROZENT, nie als Rohwert des Timers. Die Kennlinie
// steht in lib/zeiger (tastverhaeltnis) und ist host-getestet.
//
// ===========================================================================
// DIE EINE REGEL, DIE JEDER EFFEKT EINHALTEN MUSS
// ===========================================================================
// Nach dem Effekt steht das Licht wieder auf dem EINGESTELLTEN Wert - auch
// wenn der Effekt abgebrochen wurde, auch wenn waehrenddessen ein neuer
// Sollwert kam, auch wenn die Referenzfahrt die Klinge zwischendurch
// ausgeschaltet hat.
//
// Der eingestellte Wert und der momentan ausgegebene sind deshalb ZWEI
// Groessen, nicht eine. Ein Effekt veraendert nur die zweite. Wer beide zu
// einer macht, verliert den Sollwert beim ersten Abbruch.
//
// ===========================================================================
// WARUM NICHT BLOCKIEREND
// ===========================================================================
// Im Pruefstand sind die Effekte Skripte mit `delay:`. Hier laufen sie als
// Schrittkette: `tick()` schaut nur, ob der naechste Schritt faellig ist.
// Der Grund ist der Stundeneffekt - er dauert 60 Sekunden, und solange darf
// weder die Konsole noch der Uhrbetrieb stehen.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

namespace starwatch {

/// Welcher Effekt gerade laeuft.
enum class Effekt {
  keiner,
  zuenden,        ///< Leuchtstoffroehre mit Glimmzuender
  loeschen,
  stundeneffekt,  ///< volle Stunde, eine Minute auf 100 %
  alarm,          ///< Timer abgelaufen: zwoelf harte Blitze
  rampe,          ///< Messhilfe: 0..20 % in halben Prozent
  treppe,         ///< Messhilfe: 0..100 % in Zehnerschritten
};

const char *effekt_text(Effekt e);

class Licht {
 public:
  /// Gibt false zurueck, wenn LEDC sich nicht einrichten liess - dann bleibt
  /// die Klinge dunkel, und der Aufrufer MUSS es melden. Ohne diese Meldung
  /// sucht man den Fehler am LED-Treiber oder an der Verdrahtung.
  bool beginn();

  /// Steht der Ausgang?
  bool bereit() const { return bereit_; }

  // -------------------------------------------------------------------------
  // Der eingestellte Wert
  // -------------------------------------------------------------------------

  /// Sollhelligkeit in Prozent. 0 heisst wirklich aus.
  /// Ein laufender Effekt wird dadurch NICHT abgebrochen - er kehrt am Ende
  /// auf den neuen Wert zurueck.
  void soll_prozent(float prozent);
  float soll_prozent() const { return soll_; }

  /// Klinge an oder aus, ohne den Reglerwert zu verlieren. `an(false)` ist
  /// nicht dasselbe wie `soll_prozent(0)`.
  void an(bool ein);
  bool ist_an() const { return an_; }

  /// Was gerade wirklich ausgegeben wird, in Prozent.
  float ausgabe_prozent() const { return ausgabe_; }

  /// Untergrenze, ab der die LED glimmt (AP 0.5). Startwert aus geraet.h.
  void min_prozent(float p) { min_ = p; }
  float min_prozent() const { return min_; }

  /// Tastverhaeltnis direkt setzen, unter Umgehung der Kennlinie. Zum Messen
  /// der Kennlinie selbst - sonst misst man die eigene Rechnung.
  void pwm_direkt(float prozent);

  // -------------------------------------------------------------------------
  // Effekte
  // -------------------------------------------------------------------------

  void zuenden();
  void loeschen();
  void stundeneffekt();
  void alarm();
  void rampe();
  void treppe();

  /// Laufenden Effekt abbrechen und auf den eingestellten Wert zurueck.
  void effekt_abbrechen();

  Effekt effekt() const { return effekt_; }
  bool effekt_laeuft() const { return effekt_ != Effekt::keiner; }

  /// Aus der Hauptschleife aufrufen. Ohne diesen Aufruf laeuft kein Effekt.
  void tick();

 private:
  struct Schritt {
    float prozent;      ///< Helligkeit in diesem Schritt
    uint32_t dauer_ms;  ///< wie lange, bevor der naechste kommt
  };

  /// Hoechstzahl der Schritte einer Kette. Die LAENGSTE ist die Messrampe mit
  /// 40 Stufen (0 bis 20 % in halben Prozent), danach der Alarm mit 24 und
  /// der Zuendablauf mit 11.
  ///
  /// Stand hier zuerst auf 26 - dann brach die Rampe bei 13 % ab, weil die
  /// Schleife gegen MAX_SCHRITTE prueft. Eine Messhilfe, die stillschweigend
  /// vor dem gesuchten Bereich endet, ist schlimmer als keine: man haette die
  /// Glimmschwelle nicht gefunden und dem Treiber die Schuld gegeben.
  static constexpr size_t MAX_SCHRITTE = 41;

  void kette_starten(Effekt e);
  void ausgabe_setzen(float prozent);
  void zurueck();

  Schritt kette_[MAX_SCHRITTE];
  size_t anzahl_ = 0;
  size_t index_ = 0;
  uint32_t schritt_ab_ = 0;
  Effekt effekt_ = Effekt::keiner;

  float soll_ = 60.0f;   ///< Reglerwert, wie im Pruefstand
  float min_ = 1.0f;
  float ausgabe_ = 0.0f;
  bool an_ = false;
  bool bereit_ = false;
};

}  // namespace starwatch
