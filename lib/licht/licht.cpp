#include "licht.h"

#include <Arduino.h>
#include <geraet.h>
#include <zeiger.h>

namespace starwatch {

namespace g = geraet;

namespace {
/// 11 Bit Aufloesung = 2048 Stufen.
///
/// NACHGERECHNET, nachdem das Geraet es gemeldet hat. Der LEDC-Timer laeuft
/// aus dem APB-Takt (80 MHz), und es gilt
///
///     hoechste Aufloesung = log2(80e6 / Frequenz)
///
/// Bei 20 kHz sind das log2(4000) = 11,97 - also **11** Bit, nicht 12. Mit 12
/// Bit braeuchte der Teiler 4096 Schritte, und die passen bei 80 MHz nur bis
/// 19531 Hz. Hier stand zuerst 12, mit genau dieser Rechnung im Kommentar und
/// der falschen Schlussfolgerung daraus; ledcAttach() ist dann fehlgeschlagen
/// und die Klinge waere schlicht dunkel geblieben.
///
/// 2048 Stufen sind reichlich: der Regler kennt 100 Prozentwerte, und selbst
/// die Glimmschwelle bei 1 % liegt noch auf Stufe 20.
constexpr uint8_t LEDC_BITS = 11;

static_assert((80000000UL / (1UL << LEDC_BITS)) >= geraet::LED_PWM_HZ,
              "LEDC: Frequenz und Aufloesung passen nicht zum 80-MHz-Takt. "
              "Es gilt hoechste Aufloesung = log2(80e6 / Frequenz).");
constexpr uint32_t LEDC_MAX = (1u << LEDC_BITS) - 1;

// Arduino-Core 3.x rechnet mit PINS, nicht mit Kanaelen: ledcAttach(pin,
// freq, bits) statt ledcSetup(kanal, ...) + ledcAttachPin(pin, kanal), und
// ledcWrite(pin, wert) statt ledcWrite(kanal, wert). Die Kanalvergabe macht
// der Core selbst. Die alte API existiert in Core 3 nicht mehr - der Build
// hat es beim Umstieg sofort gemeldet.

/// Zufallszahl im Bereich [von, von+spanne). Die Pausen der Zuendkette sind
/// absichtlich zufaellig: eine Roehre zuendet nie zweimal gleich, und
/// regelmaessiges Blinken wirkt sofort kuenstlich.
uint32_t streu(uint32_t von, uint32_t spanne) {
  if (spanne == 0)
    return von;
  return von + (esp_random() % spanne);
}
}  // namespace

const char *effekt_text(Effekt e) {
  switch (e) {
    case Effekt::keiner:        return "keiner";
    case Effekt::zuenden:       return "zuenden";
    case Effekt::loeschen:      return "loeschen";
    case Effekt::stundeneffekt: return "Stundeneffekt";
    case Effekt::alarm:         return "Alarm";
    case Effekt::rampe:         return "Rampe";
    case Effekt::treppe:        return "Treppe";
  }
  return "unbekannt";
}

// ===========================================================================
// Aufbau und Ausgabe
// ===========================================================================

bool Licht::beginn() {
  // Rueckgabewert statt log_e(): bei CORE_DEBUG_LEVEL=0 - und darauf steht
  // dieses Projekt - wird log_e wegoptimiert und die Meldung ist unsichtbar.
  // Genau so ist der Fehler oben beim ersten Lauf am Aufbau fast durchgerutscht:
  // zu sehen war nur die Meldung des LEDC-Treibers selbst.
  bereit_ = ledcAttach(g::PIN_LED_PWM, g::LED_PWM_HZ, LEDC_BITS);
  min_ = g::LED_MIN_PROZENT;
  an_ = false;
  // Nach dem Flashen soll das Geraet NICHT mit 290 mA anfangen - der
  // Pruefstand setzt dafuer restore_mode: ALWAYS_OFF.
  ausgabe_setzen(0.0f);
  return bereit_;
}

void Licht::ausgabe_setzen(float prozent) {
  if (prozent < 0.0f)
    prozent = 0.0f;
  if (prozent > 100.0f)
    prozent = 100.0f;
  ausgabe_ = prozent;

  const float anteil = tastverhaeltnis(prozent, min_);
  ledcWrite(g::PIN_LED_PWM, static_cast<uint32_t>(lroundf(anteil * LEDC_MAX)));
}

void Licht::pwm_direkt(float prozent) {
  // Ohne Kennlinie: genau dafuer ist diese Funktion da. Ein laufender Effekt
  // wuerde den Wert im naechsten Schritt ueberschreiben, also erst abbrechen.
  effekt_abbrechen();
  if (prozent < 0.0f)
    prozent = 0.0f;
  if (prozent > 100.0f)
    prozent = 100.0f;
  ausgabe_ = prozent;
  ledcWrite(g::PIN_LED_PWM, static_cast<uint32_t>(lroundf(prozent / 100.0f * LEDC_MAX)));
}

void Licht::soll_prozent(float prozent) {
  if (prozent < 0.0f)
    prozent = 0.0f;
  if (prozent > 100.0f)
    prozent = 100.0f;
  soll_ = prozent;
  // Laeuft ein Effekt, gehoert ihm die Ausgabe - er kehrt am Ende auf den
  // neuen Sollwert zurueck. Das ist die Trennung der zwei Groessen.
  if (!effekt_laeuft())
    zurueck();
}

void Licht::an(bool ein) {
  an_ = ein;

  // AUSSCHALTEN BRICHT EINEN LAUFENDEN EFFEKT AB - Einschalten nicht.
  //
  // Die Unterscheidung ist keine Willkuer. Ein Effekt besitzt die Ausgabe,
  // solange er laeuft; deshalb darf ein Zuenden nicht davon gestoert werden,
  // dass jemand die Helligkeit nachzieht. Aber "aus" ist kein Regler, sondern
  // die Ansage, KEIN LICHT abzugeben - und die muss gewinnen.
  //
  // Vorher tat sie es nicht: waehrend des Stundeneffekts (eine Minute auf
  // 100 %) meldete 'lo' brav "aus", waehrend die Klinge weiter mit voller
  // Leistung brannte. Ein Zustand, der luegt, ist schlimmer als einer, der
  // nicht folgt. Am 24.08.2026 reproduziert und hier behoben.
  // `effekt_abbrechen()` gab es laengst - es wurde von hier nur nie gerufen.
  // Es setzt die Kette zurueck UND ruft `zurueck()`, und weil `an_` oben schon
  // false ist, gibt das die Ausgabe 0.
  if (!ein)
    effekt_abbrechen();

  if (!effekt_laeuft())
    zurueck();
}


void Licht::zurueck() {
  ausgabe_setzen(an_ ? soll_ : 0.0f);
}

// ===========================================================================
// Effekte
// ===========================================================================

void Licht::kette_starten(Effekt e) {
  effekt_ = e;
  index_ = 0;
  schritt_ab_ = millis();
  if (anzahl_ > 0)
    ausgabe_setzen(kette_[0].prozent);
}

void Licht::effekt_abbrechen() {
  if (effekt_ == Effekt::keiner)
    return;
  effekt_ = Effekt::keiner;
  anzahl_ = 0;
  index_ = 0;
  // Zurueck auf den eingestellten Wert - das ist die Regel, die jeder Effekt
  // einhalten muss, auch der abgebrochene.
  zurueck();
}

void Licht::zuenden() {
  // Nachgebaut ist der Ablauf einer Roehre mit Glimmzuender: erst heizen die
  // Elektroden vor und glimmen schwach, dann versucht der Zuender zu ziehen -
  // das sind die hellen Blitze mit ungleichen Pausen - und beim dritten oder
  // vierten Mal steht die Entladung.
  // NEU GEFASST AM 23.08.2026: schneller und mit mehr Ereignissen. Die alte
  // Kette brauchte rund 1315 ms und bestand aus zehn Stufen - lang genug, dass
  // man beim Einschalten wartet, statt zu staunen. Diese hier braucht rund
  // 650 ms und hat zwanzig Stufen. Kuerzer UND unruhiger; auf Realismus wirkt
  // die Zahl der Ereignisse, nicht die Dauer.
  //
  // WAS NEU HINZUKAM, IST DAS NACHZITTERN. Vorher stand die Roehre mit einem
  // Sprung auf 100 % und blieb dort - und genau daran erkennt das Auge die
  // Nachahmung. Eine echte Entladung ist in den ersten Zehntelsekunden noch
  // instabil: sie faellt kurz ein, faengt sich, faellt schwaecher ein und
  // steht erst dann. Diese drei Einbrueche am Ende tun mehr fuer den Eindruck
  // als alle Zuendversuche davor.
  anzahl_ = 0;
  kette_[anzahl_++] = {0.0f, 40};
  // Vorglimmen der Elektroden - kurz und schwach.
  kette_[anzahl_++] = {4.0f, streu(70, 60)};
  kette_[anzahl_++] = {0.0f, streu(20, 30)};
  // Vier Zuendversuche. Kuerzere Blitze, kuerzere Pausen als vorher.
  for (int i = 0; i < 4; i++) {
    kette_[anzahl_++] = {100.0f, streu(10, 18)};
    kette_[anzahl_++] = {0.0f, streu(25, 55)};
  }
  // Ein halbherziger Versuch, der wieder abreisst.
  kette_[anzahl_++] = {55.0f, streu(18, 25)};
  kette_[anzahl_++] = {0.0f, streu(12, 30)};
  // Die Entladung steht - aber noch nicht ruhig.
  kette_[anzahl_++] = {100.0f, 40};
  kette_[anzahl_++] = {72.0f, streu(15, 20)};
  kette_[anzahl_++] = {100.0f, streu(30, 40)};
  kette_[anzahl_++] = {88.0f, streu(12, 18)};
  // Jetzt steht sie.
  kette_[anzahl_++] = {100.0f, 60};

  an_ = true;
  kette_starten(Effekt::zuenden);
}

void Licht::loeschen() {
  // Eine echte Roehre geht beim Abschalten einfach aus. Das kurze Zucken
  // davor ist Effekt, kein Nachbau - es soll zum Zuenden passen.
  anzahl_ = 0;
  kette_[anzahl_++] = {60.0f, 45};
  kette_[anzahl_++] = {0.0f, 60};
  kette_[anzahl_++] = {20.0f, 30};
  kette_[anzahl_++] = {0.0f, 1};

  an_ = false;
  kette_starten(Effekt::loeschen);
}

void Licht::stundeneffekt() {
  anzahl_ = 0;
  kette_[anzahl_++] = {100.0f, 60000};  // eine Minute auf 100 %
  an_ = true;
  kette_starten(Effekt::stundeneffekt);
}

void Licht::alarm() {
  // Zwoelf harte Blitze. Der Zeiger wird dabei BEWUSST nicht bewegt - er
  // steht auf zwoelf, und das ist die Aussage.
  static_assert(MAX_SCHRITTE >= 24, "Der Alarm braucht 24 Schritte.");
  anzahl_ = 0;
  for (int i = 0; i < 12; i++) {
    kette_[anzahl_++] = {100.0f, 120};
    kette_[anzahl_++] = {0.0f, 180};
  }
  an_ = true;
  kette_starten(Effekt::alarm);
}

void Licht::rampe() {
  // Messhilfe fuer AP 0.5: 0 bis 20 % in halben Prozent, 700 ms je Stufe.
  // Gesucht ist der Prozentwert, ab dem die LED erstmals glimmt.
  static_assert(MAX_SCHRITTE >= 40, "Die Messrampe braucht 40 Stufen.");
  anzahl_ = 0;
  for (int i = 1; i <= 40; i++)
    kette_[anzahl_++] = {i * 0.5f, 700};
  an_ = true;
  kette_starten(Effekt::rampe);
}

void Licht::treppe() {
  // Messhilfe fuer AP 0.5: 0 bis 100 % in Zehnerschritten, 3 s je Stufe.
  // Gesucht ist, ob die Helligkeit gleichmaessig laeuft.
  static_assert(MAX_SCHRITTE >= 11, "Die Messtreppe braucht 11 Stufen.");
  anzahl_ = 0;
  for (int i = 0; i <= 10; i++)
    kette_[anzahl_++] = {i * 10.0f, 3000};
  an_ = true;
  kette_starten(Effekt::treppe);
}

void Licht::tick() {
  if (effekt_ == Effekt::keiner || anzahl_ == 0)
    return;

  if (millis() - schritt_ab_ < kette_[index_].dauer_ms)
    return;

  index_++;
  if (index_ >= anzahl_) {
    // Kette zu Ende. loeschen() laesst die Klinge aus, alle anderen kehren
    // auf den eingestellten Wert zurueck.
    effekt_ = Effekt::keiner;
    anzahl_ = 0;
    index_ = 0;
    zurueck();
    return;
  }

  schritt_ab_ = millis();
  ausgabe_setzen(kette_[index_].prozent);
}

}  // namespace starwatch
