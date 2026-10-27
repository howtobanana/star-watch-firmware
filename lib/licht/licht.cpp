#include "licht.h"

#include <Arduino.h>
#include <geraet.h>
#include <zeiger.h>

#include <driver/ledc.h>

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
///
/// 2 kHz statt 20 kHz, also log2(80e6 / 2000) = 15,3 - der C3
/// kann aber hoechstens 14 Bit. 13 Bit = 8192 Stufen, und die braucht es:
/// ein Kanal mit Farbwert 20 bei 5 % Helligkeit steht bei 0,4 %
/// Tastverhaeltnis, das sind mit 13 Bit noch 32 Stufen statt 8.
constexpr uint8_t LEDC_BITS = 13;

static_assert((80000000UL / (1UL << LEDC_BITS)) >= geraet::LED_PWM_HZ,
              "LEDC: Frequenz und Aufloesung passen nicht zum 80-MHz-Takt. "
              "Es gilt hoechste Aufloesung = log2(80e6 / Frequenz).");
constexpr uint32_t LEDC_MAX = (1u << LEDC_BITS) - 1;

// Arduino-Core 3.x rechnet mit PINS, nicht mit Kanaelen: ledcAttach(pin,
// freq, bits) statt ledcSetup(kanal, ...) + ledcAttachPin(pin, kanal), und
// ledcWrite(pin, wert) statt ledcWrite(kanal, wert). Die Kanalvergabe macht
// der Core selbst. Die alte API existiert in Core 3 nicht mehr - der Build
// hat es beim Umstieg sofort gemeldet.

// ---------------------------------------------------------------------------
// LEDC UEBER ESP-IDF, NICHT UEBER ledcAttach().
//
// Der Grund ist die Umkehrung. ledcAttach() setzt beim Einrichten Tastver-
// haeltnis 0 - der Pin geht also LOW, und LOW heisst hier "voll an". Bis
// das erste ledcWrite() kommt, blitzten alle drei Farben mit 294 mA. Die
// Arbeitsliste 13.3 verbietet genau das.
//
// ledc_channel_config() nimmt die Umkehrung (`flags.output_invert`) im
// SELBEN Aufruf wie den Startwert 0 - der Pin wechselt von "hochohmig mit
// Pull-up = HIGH" direkt auf "LEDC, invertiert, 0 = HIGH". Zu keinem
// Zeitpunkt LOW. Und die Umkehrung steht damit an genau EINER Stelle; das
// Tastverhaeltnis darueber ist die Helligkeit.
// ---------------------------------------------------------------------------
constexpr uint8_t KANAL_PIN[3] = {g::PIN_LED_PWM, g::PIN_LED_G_PWM,
                                  g::PIN_LED_B_PWM};
constexpr ledc_channel_t KANAL_LEDC[3] = {LEDC_CHANNEL_0, LEDC_CHANNEL_1,
                                          LEDC_CHANNEL_2};
constexpr ledc_mode_t LEDC_MODUS = LEDC_LOW_SPEED_MODE;  // der C3 hat nur diesen
constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;

bool hw_beginn() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_MODUS;
  t.duty_resolution = static_cast<ledc_timer_bit_t>(LEDC_BITS);
  t.timer_num = LEDC_TIMER;
  t.freq_hz = g::LED_PWM_HZ;
  t.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&t) != ESP_OK)
    return false;

  for (int k = 0; k < 3; k++) {
    ledc_channel_config_t c = {};
    c.gpio_num = KANAL_PIN[k];
    c.speed_mode = LEDC_MODUS;
    c.channel = KANAL_LEDC[k];
    c.intr_type = LEDC_INTR_DISABLE;
    c.timer_sel = LEDC_TIMER;
    c.duty = 0;
    c.hpoint = 0;
    c.flags.output_invert = g::LED_INVERTIERT ? 1 : 0;
    if (ledc_channel_config(&c) != ESP_OK)
      return false;
  }
  return true;
}

void hw_schreiben(int kanal, float anteil) {
  const uint32_t duty = static_cast<uint32_t>(lroundf(anteil * LEDC_MAX));
  ledc_set_duty(LEDC_MODUS, KANAL_LEDC[kanal], duty);
  ledc_update_duty(LEDC_MODUS, KANAL_LEDC[kanal]);
}

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
    case Effekt::blende:        return "blende";
    case Effekt::stundeneffekt: return "Stundeneffekt";
    case Effekt::alarm:         return "Alarm";
    case Effekt::rampe:         return "Rampe";
    case Effekt::treppe:        return "Treppe";
    case Effekt::gleiten:       return "gleiten";
    case Effekt::puls:          return "Puls";
    case Effekt::blitz:         return "Blitz";
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
  bereit_ = hw_beginn();
  min_ = g::LED_MIN_PROZENT;
  farbe_ = g::LED_FARBE_VORGABE;
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
  kanaele_schreiben(false);
}

void Licht::kanaele_schreiben(bool direkt) {
  direkt_ = direkt;
  ausgeben();
}

void Licht::ausgeben() {
  // DER VORRANG, an genau einer Stelle - siehe licht.h.
  const Signal *s = nullptr;
  if (sig_[static_cast<int>(Ebene::zustand)].aktiv)
    s = &sig_[static_cast<int>(Ebene::zustand)];
  else if (!effekt_laeuft() && !meldung_ruht_ &&
           sig_[static_cast<int>(Ebene::meldung)].aktiv)
    s = &sig_[static_cast<int>(Ebene::meldung)];

  if (s != nullptr)
    schreiben(s->hell * muster_anteil(s->muster, millis() - s->start_ms), s->farbe,
              false);
  else {
    const uint32_t f =
        (effekt_ == Effekt::zuenden || effekt_ == Effekt::loeschen ||
         effekt_ == Effekt::blende || effekt_ == Effekt::gleiten || effekt_ == Effekt::puls)
            ? effekt_farbe_
            : (vorrang_an_ ? vorrang_farbe_ : farbe_);
    basis_farbe_ = f;
    schreiben(ausgabe_, f, direkt_);
  }
}

void Licht::farbe_vorrang(bool an, uint32_t rgb) {
  vorrang_an_ = an;
  vorrang_farbe_ = rgb & 0xFFFFFF;
  kanaele_schreiben(direkt_);
}

void Licht::schreiben(float prozent, uint32_t farbe, bool direkt) {
  zeige_prozent_ = prozent;
  zeige_farbe_ = farbe;
  if (!bereit_)
    return;
  // DIE LEISTUNGSGRENZE GILT AUF JEDEM WEG, auch fuer pwm_direkt() und die
  // Blinkzeichen. Vorgabe vom 27.10.2026: nie mehr als 95 % der 3 W, sonst
  // ueberhitzt die LED. Ein Messwerkzeug, das daran vorbeifuehrt, waere
  // genau die Hintertuer, durch die es doch passiert.
  float k[3];
  const float anteil = direkt ? prozent / 100.0f : tastverhaeltnis(prozent, min_);
  leistung_w_ = kanal_anteile(anteil, farbe, g::LED_WATT_VOLL, g::LED_MAX_W, k);
  for (int i = 0; i < KANAELE; i++)
    hw_schreiben(i, k[i]);
}

void Licht::farbe(uint32_t rgb, bool zuenden) {
  const uint32_t neu = rgb & 0xFFFFFF;
  // Was die Klinge GERADE zeigt, ist der Ausgang - auch mitten in einer
  // Zuendung, die noch zur vorigen Farbe unterwegs war.
  // Seit 0.20.0 die GEZEIGTE Farbe - auch aus einem Uebergang, einem Puls
  // oder einer Ueberblendung. Vorher die gespeicherte, und von der aus
  // gleitete die Klinge auch dann, wenn sie laengst etwas anderes zeigte.
  const uint32_t alt = gezeigte_farbe();
  farbe_ = neu;
  // MIT ZUENDUNG nur, wenn die Klinge brennt, sich die Farbe wirklich aendert
  // und kein anderer Effekt die Ausgabe besitzt (Alarm, Countdown,
  // Stundeneffekt laufen weiter - nur ihre Farbe wechselt).
  if (zuenden && an_ && neu != alt &&
      (effekt_ == Effekt::keiner || effekt_ == Effekt::zuenden ||
       effekt_ == Effekt::blende)) {
    if (effekte_)
      zuenden_von(alt);
    else {
      const float basis = soll_ * dimm_;
      blende_starten(basis > 0.0f ? ausgabe_ / basis : 1.0f, 1.0f, alt);  // Farbe gleitet
    }
    return;
  }
  // Sonst wirkt die Farbe sofort - auch mitten in einem Effekt. Der Effekt
  // besitzt die Helligkeit, nicht die Farbe.
  kanaele_schreiben(direkt_);
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
  // In der eingestellten Farbe, ohne Kennlinie - aber MIT der
  // Leistungsgrenze. Eine reine Farbe liegt darunter: 'lf ff0000' und
  // 'w 100' sind wirklich 100 %, an R9 also 200 mV (Arbeitsliste 13.7).
  kanaele_schreiben(true);
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

void Licht::dimmfaktor(float faktor) {
  if (faktor < 0.0f)
    faktor = 0.0f;
  if (faktor > 1.0f)
    faktor = 1.0f;
  // NaN faellt durch beide Vergleiche - und ein NaN im Tastverhaeltnis ist
  // eine dunkle Klinge ohne jede Meldung.
  if (!(faktor >= 0.0f))
    faktor = 1.0f;
  dimm_ = faktor;

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
  ausgabe_setzen(an_ ? soll_ * dimm_ : 0.0f);
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

void Licht::zuenden() { zuenden_von(vorrang_an_ ? vorrang_farbe_ : farbe_); }

void Licht::zuenden_von(uint32_t alt) {
  // WIE EIN LICHTSCHWERT, NICHT WIE EINE LEUCHTSTOFFROEHRE (28.09.2026).
  // Bis dahin stand hier der Glimmzuender einer Roehre: vier harte Blitze,
  // ein Nachzittern, 650 ms, in festen Stufen und sofort in der Zielfarbe.
  // Man bekam ihn kaum mit. Jetzt rechnet zuend_bild() (lib/zeiger,
  // host-getestet) jedes Bild aus der Zeit: Zuendversuche, Ausfahren mit
  // kurzem Aufleuchten, zwei kleiner werdende Wellen, und eine Farbe, die
  // erst "heiss" ist und von der alten zur neuen gleitet. Alles RELATIV zum
  // eingestellten Wert - nachts auf 20 % gibt es keinen grellen Blitz.
  if (!effekte_) {
    // Effekt aus: einblenden - von dem, was gerade leuchtet.
    const float basis = soll_ * dimm_;
    an_ = true;
    blende_starten(basis > 0.0f ? ausgabe_ / basis : 1.0f, 1.0f, alt);
    return;
  }
  anzahl_ = 0;
  index_ = 0;
  zuend_alt_ = alt & 0xFFFFFF;
  zuend_ab_ = millis();
  zuend_letzt_ = zuend_ab_;
  zuend_zufall_ = esp_random() | 1u;
  an_ = true;
  effekt_ = Effekt::zuenden;
  zuend_schritt();
}

void Licht::blende_starten(float von, float nach, uint32_t alt) {
  anzahl_ = 0;
  index_ = 0;
  zuend_alt_ = alt & 0xFFFFFF;
  blende_von_ = von < 0.0f ? 0.0f : (von > 1.5f ? 1.5f : von);
  blende_nach_ = nach;
  zuend_ab_ = millis();
  zuend_letzt_ = zuend_ab_;
  effekt_ = Effekt::blende;
  zuend_schritt();
}

void Licht::zuend_schritt() {
  // Zuenden UND Einfahren - beide zeitgesteuert, beide relativ zum
  // eingestellten Wert (soll_ * dimm_, auch wenn an_ beim Einfahren schon
  // false ist - am Ende setzt zurueck() dann auf 0).
  const uint32_t t = millis() - zuend_ab_;
  const bool ein = (effekt_ == Effekt::zuenden);
  const bool blende = (effekt_ == Effekt::blende);
  const uint32_t dauer = blende ? BLENDE_DAUER_MS : (ein ? ZUEND_DAUER_MS : LOESCH_DAUER_MS);
  if (t >= dauer) {
    effekt_ = Effekt::keiner;
    zurueck();
    return;
  }
  const uint32_t ziel = vorrang_an_ ? vorrang_farbe_ : farbe_;
  const Zuendbild b =
      blende ? blende_bild(t, blende_von_, blende_nach_, zuend_alt_, ziel)
      : ein  ? zuend_bild(t, zuend_alt_, ziel, zuend_zufall_)
             : loesch_bild(t, zuend_alt_, zuend_zufall_);
  effekt_farbe_ = b.farbe;
  ausgabe_setzen(soll_ * dimm_ * b.anteil);
}

void Licht::loeschen() {
  // DIE KLINGE FAEHRT EIN (28.09.2026) - loesch_bild() in lib/zeiger, das
  // Gegenstueck zur Zuendung. Vorher zuckte sie zweimal und war weg; bei der
  // Nachtabschaltung ging sie einfach aus.
  if (!an_ && ausgabe_ <= 0.0f)
    return;  // ist schon dunkel
  if (!effekte_) {
    const uint32_t jetzt_farbe = gezeigte_farbe();
    const float basis = soll_ * dimm_;
    const float von = basis > 0.0f ? ausgabe_ / basis : 1.0f;
    an_ = false;
    blende_starten(von, 0.0f, jetzt_farbe);
    return;
  }
  anzahl_ = 0;
  index_ = 0;
  zuend_alt_ = gezeigte_farbe();
  zuend_ab_ = millis();
  zuend_letzt_ = zuend_ab_;
  zuend_zufall_ = esp_random() | 1u;
  an_ = false;
  effekt_ = Effekt::loeschen;
  zuend_schritt();
}

// ===========================================================================
// Uebergang, Puls, Blitz (0.20.0)
// ===========================================================================

void Licht::weich_setzen(const Weich &w, uint32_t dauer_ms) {
  // DER STARTPUNKT ZUERST, bevor irgendein Wert sich aendert: das, was die
  // Klinge in diesem Augenblick zeigt.
  const float von_p = ausgabe_;
  const uint32_t von_f = gezeigte_farbe();

  // Ein laufender Puls endet hier. Ohne neue Werte gilt der Zustand davor.
  if (effekt_ == Effekt::puls)
    an_ = puls_vor_an_;

  if (w.farbe_da)
    farbe_ = w.farbe & 0xFFFFFF;
  if (w.hell >= 0.0f)
    soll_ = w.hell > 100.0f ? 100.0f : w.hell;
  if (w.an >= 0)
    an_ = (w.an == 1);

  anzahl_ = 0;
  index_ = 0;
  if (dauer_ms == 0) {
    effekt_ = Effekt::keiner;
    zurueck();
    return;
  }
  gl_von_prozent_ = von_p;
  gl_von_farbe_ = von_f;
  gl_ab_ = millis();
  gl_dauer_ = dauer_ms;
  zuend_letzt_ = gl_ab_;
  effekt_ = Effekt::gleiten;
  gleit_schritt();
}

void Licht::gleit_schritt() {
  // DAS ZIEL WIRD IN JEDEM BILD NEU GELESEN: dreht die Lichtautomatik oder
  // der Regler waehrend des Uebergangs, gleitet die Klinge dorthin statt am
  // Ende zu springen.
  const uint32_t t = millis() - gl_ab_;
  if (t >= gl_dauer_) {
    effekt_ = Effekt::keiner;
    zurueck();
    return;
  }
  const float ziel_p = an_ ? soll_ * dimm_ : 0.0f;
  const uint32_t ziel_f = vorrang_an_ ? vorrang_farbe_ : farbe_;
  effekt_farbe_ = farbe_mischen(gl_von_farbe_, ziel_f,
                                static_cast<float>(t) / static_cast<float>(gl_dauer_));
  ausgabe_setzen(uebergang_wert(gl_von_prozent_, ziel_p, t, gl_dauer_));
}

void Licht::puls_einstellen(uint32_t periode_ms, float min_anteil) {
  // Die Phase bleibt - eine neue Periode greift ohne Sprung.
  puls_periode_ = puls_periode_begrenzt(periode_ms);
  puls_min_ = puls_min_begrenzt(min_anteil);
}

void Licht::puls_starten(bool farbe_da, uint32_t farbe, float max_prozent) {
  const bool lief = (effekt_ == Effekt::puls);
  if (!lief)
    puls_vor_an_ = an_;
  const uint32_t jetzt_f = gezeigte_farbe();

  if (farbe_da)
    puls_farbe_ = farbe & 0xFFFFFF;
  else if (!lief)
    puls_farbe_ = vorrang_an_ ? vorrang_farbe_ : farbe_;
  if (max_prozent > 0.0f)
    puls_max_ = max_prozent > 100.0f ? 100.0f : max_prozent;
  else if (!lief)
    puls_max_ = 100.0f;

  // Die Farbe gleitet in 0,5 s von der gezeigten zur Pulsfarbe.
  puls_von_farbe_ = jetzt_f;
  puls_farb_ab_ = millis();
  // Neu gestartet: die Phase so waehlen, dass der Puls dort beginnt, wo die
  // Klinge gerade steht - kein Sprung auf Minimum oder Maximum.
  if (!lief)
    puls_phase_ = puls_phase_fuer(puls_max_ > 0.0f ? ausgabe_ / puls_max_ : 0.0f, puls_min_);
  puls_letzt_ = millis();
  zuend_letzt_ = puls_letzt_;
  an_ = true;
  anzahl_ = 0;
  index_ = 0;
  effekt_ = Effekt::puls;
  puls_schritt();
}

void Licht::puls_schritt() {
  const uint32_t jetzt = millis();
  puls_phase_ = puls_phase_weiter(puls_phase_, jetzt - puls_letzt_, puls_periode_);
  puls_letzt_ = jetzt;
  const uint32_t tf = jetzt - puls_farb_ab_;
  effekt_farbe_ = tf >= 500 ? puls_farbe_
                            : farbe_mischen(puls_von_farbe_, puls_farbe_, tf / 500.0f);
  // OHNE dimm_: waehrend des Pulses schreibt die Lichtautomatik nicht mit.
  ausgabe_setzen(puls_max_ * puls_anteil(puls_phase_, puls_min_));
}

void Licht::blitzen(int n) {
  if (n < 1)
    n = 1;
  if (static_cast<size_t>(2 * n) > MAX_SCHRITTE)
    n = static_cast<int>(MAX_SCHRITTE / 2);
  // Relativ zum eingestellten Wert, aber sichtbar: nachts auf 5 % waere ein
  // Blitz auf 5 % keiner.
  const float hoch = (soll_ * dimm_ < 30.0f) ? 30.0f : soll_ * dimm_;
  anzahl_ = 0;
  for (int i = 0; i < n; i++) {
    kette_[anzahl_++] = {0.0f, 180};
    kette_[anzahl_++] = {hoch, 220};
  }
  an_ = true;
  kette_starten(Effekt::blitz);
}

void Licht::stundeneffekt() {
  anzahl_ = 0;
  kette_[anzahl_++] = {100.0f, 60000};  // eine Minute auf 100 %
  an_ = true;
  kette_starten(Effekt::stundeneffekt);
}

void Licht::alarm() {
  // DREI schnelle Blitze. Der Zeiger wird dabei BEWUSST nicht bewegt - er
  // steht auf zwoelf, und das ist die Aussage.
  //
  // Frueher waren es zwoelf ueber rund 3,6 s. Das war laenger als die Frist,
  // nach der die Uhr auf die Zeit zurueckwechselt - der Alarm lief also noch,
  // waehrend der Zeiger schon wegfuhr. Drei Blitze zu 250 ms sind nach 0,75 s
  // vorbei und passen in die drei Sekunden davor.
  static_assert(MAX_SCHRITTE >= 6, "Der Alarm braucht 6 Schritte.");
  anzahl_ = 0;
  for (int i = 0; i < 3; i++) {
    kette_[anzahl_++] = {100.0f, 110};
    kette_[anzahl_++] = {0.0f, 140};
  }
  an_ = true;
  kette_starten(Effekt::alarm);
}

void Licht::countdown(int sekunden) {
  // Ein Blitz je Sekunde: kurz hell, dann dunkel bis zur naechsten Sekunde.
  // Die Summe je Schritt ist genau 1000 ms, damit der Countdown so lange
  // dauert, wie er behauptet - der Timer startet danach.
  if (sekunden < 1)
    sekunden = 1;
  if (sekunden > MAX_SCHRITTE / 2)
    sekunden = MAX_SCHRITTE / 2;
  anzahl_ = 0;
  for (int i = 0; i < sekunden; i++) {
    kette_[anzahl_++] = {100.0f, 160};
    kette_[anzahl_++] = {0.0f, 840};
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

void Licht::signal(Ebene e, uint32_t farbe, Muster m, float hell, uint32_t dauer_ms) {
  Signal &s = sig_[static_cast<int>(e)];
  if (!(hell > 0.0f))
    hell = soll_ < 20.0f ? 20.0f : soll_;
  if (hell > 100.0f)
    hell = 100.0f;
  s.aktiv = true;
  s.farbe = farbe & 0xFFFFFF;
  s.muster = m;
  s.hell = hell;
  s.start_ms = millis();
  s.dauer_ms = dauer_ms;
  ausgeben();
}

void Licht::signal_aus(Ebene e) {
  Signal &s = sig_[static_cast<int>(e)];
  if (!s.aktiv)
    return;
  s.aktiv = false;
  ausgeben();
}

Licht::Signalzustand Licht::signal_zustand(Ebene e) const {
  const Signal &s = sig_[static_cast<int>(e)];
  Signalzustand z;
  z.aktiv = s.aktiv;
  z.farbe = s.farbe;
  z.muster = s.muster;
  z.hell = s.hell;
  if (s.aktiv && s.dauer_ms > 0) {
    const uint32_t vergangen = millis() - s.start_ms;
    z.rest_ms = vergangen < s.dauer_ms ? s.dauer_ms - vergangen : 1;
  }
  return z;
}

void Licht::tick() {
  // --- Signale: ablaufen lassen und die Muster fortschreiben -------------
  bool sig_an = false;
  for (Signal &s : sig_) {
    if (s.aktiv && s.dauer_ms > 0 && millis() - s.start_ms >= s.dauer_ms) {
      s.aktiv = false;
      ausgeben();  // zurueck auf das, was darunter liegt
    }
    sig_an = sig_an || s.aktiv;
  }
  // Alle 20 ms neu schreiben - fuer ein ruhiges Pulsieren genug, und die
  // LEDC-Register werden nicht in jedem Schleifendurchlauf angefasst.
  if (sig_an && millis() - sig_zuletzt_ms_ >= 20) {
    sig_zuletzt_ms_ = millis();
    ausgeben();
  }

  // Uebergang und Puls: alle 20 ms ein neues Bild, aus der Zeit gerechnet.
  if (effekt_ == Effekt::gleiten || effekt_ == Effekt::puls) {
    if (millis() - zuend_letzt_ >= 20) {
      zuend_letzt_ = millis();
      if (effekt_ == Effekt::gleiten)
        gleit_schritt();
      else
        puls_schritt();
    }
    return;
  }

  // Die Zuendung ist zeitgesteuert: alle 20 ms ein neues Bild.
  if (effekt_ == Effekt::zuenden || effekt_ == Effekt::loeschen ||
      effekt_ == Effekt::blende) {
    if (millis() - zuend_letzt_ >= 20) {
      zuend_letzt_ = millis();
      zuend_schritt();
    }
    return;
  }

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
