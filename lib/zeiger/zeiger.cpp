#include "zeiger.h"

#include <cmath>
#include <cstdio>

namespace starwatch {

namespace {
constexpr int32_t HALBTAG_SEK = 43200;  // 12 h, eine Zeigerumdrehung
}

int32_t normiere(int32_t position, int32_t rev) {
  if (rev <= 0)
    return 0;
  return ((position % rev) + rev) % rev;
}

int32_t soll_position(int32_t sekunden, int32_t rev, float sek_je_schritt) {
  if (rev <= 0 || sek_je_schritt <= 0.0f)
    return 0;
  const int32_t s = ((sekunden % HALBTAG_SEK) + HALBTAG_SEK) % HALBTAG_SEK;
  // Runden, nicht abschneiden: abschneiden liesse den Zeiger im Mittel einen
  // halben Schritt nachlaufen. Das Modulo faengt den Fall kurz vor zwoelf ab,
  // wo das Runden auf rev aufgeht.
  return normiere(static_cast<int32_t>(std::lround(s / sek_je_schritt)), rev);
}

int32_t delta_vorwaerts(int32_t ist, int32_t ziel, int32_t rev) {
  return normiere(ziel - ist, rev);
}

bool warten_statt_umrunden(int32_t delta, int32_t rev, int32_t toleranz) {
  if (rev <= 0 || toleranz <= 0)
    return false;
  return delta > rev - toleranz;
}

int32_t drift(int32_t ist, int32_t referenz, int32_t rev) {
  if (rev <= 0)
    return 0;
  int32_t d = normiere(ist, rev) - normiere(referenz, rev);
  if (d > rev / 2)
    d -= rev;
  if (d < -rev / 2)
    d += rev;
  return d;
}

bool schwerpunkt(const int32_t *positionen, const float *messwerte, size_t anzahl,
                 float ruhe, float schwelle, float *ergebnis) {
  if (positionen == nullptr || messwerte == nullptr || ergebnis == nullptr || anzahl == 0)
    return false;

  double summe_gewicht = 0.0;
  double summe_gewicht_mal_position = 0.0;

  for (size_t i = 0; i < anzahl; i++) {
    const float auslenkung = std::fabs(messwerte[i] - ruhe);
    if (auslenkung < schwelle)
      continue;
    const double gewicht = auslenkung - schwelle;
    summe_gewicht += gewicht;
    summe_gewicht_mal_position += gewicht * static_cast<double>(positionen[i]);
  }

  if (summe_gewicht <= 0.0)
    return false;

  *ergebnis = static_cast<float>(summe_gewicht_mal_position / summe_gewicht);
  return true;
}

std::string zeigerzeit(int32_t position, int32_t rev, float sek_je_schritt) {
  if (rev <= 0 || sek_je_schritt <= 0.0f)
    return "--:--";

  const int32_t p = normiere(position, rev);
  const int32_t sekunden = static_cast<int32_t>(p * sek_je_schritt);

  int32_t stunden = sekunden / 3600;
  if (stunden == 0)
    stunden = 12;  // Zwoelferblatt, es gibt keine null Uhr

  const int32_t minuten = (sekunden % 3600) / 60;

  char puffer[8];
  std::snprintf(puffer, sizeof(puffer), "%d:%02d", static_cast<int>(stunden),
                static_cast<int>(minuten));
  return std::string(puffer);
}

// ===========================================================================
// Schwerpunktsammler
// ===========================================================================

Schwerpunktsammler::Schwerpunktsammler(float ruhe, float schwelle)
    : ruhe_(ruhe), schwelle_(schwelle < 0.0f ? 0.0f : schwelle) {}

void Schwerpunktsammler::nimm(int32_t position, float messwert) {
  const float auslenkung = std::fabs(messwert - ruhe_);
  if (auslenkung <= schwelle_)
    return;
  // Die Schwelle wird vom Gewicht abgezogen, damit der Rand der Kurve nicht
  // mitzieht - dieselbe Rechnung wie in der Array-Fassung.
  const double gewicht = auslenkung - schwelle_;
  summe_gewicht_ += gewicht;
  summe_gewicht_mal_position_ += gewicht * static_cast<double>(position);
  punkte_++;
}

bool Schwerpunktsammler::ergebnis(float *mitte) const {
  if (mitte == nullptr || summe_gewicht_ <= 0.0)
    return false;
  *mitte = static_cast<float>(summe_gewicht_mal_position_ / summe_gewicht_);
  return true;
}

bool schwerpunkt_symmetrisch(const int32_t *positionen, const float *messwerte,
                             size_t anzahl, float ruhe, float schwelle,
                             int32_t halbbreite, float *ergebnis) {
  if (positionen == nullptr || messwerte == nullptr || ergebnis == nullptr ||
      anzahl == 0 || halbbreite <= 0)
    return false;

  // -------------------------------------------------------------------------
  // 1. Startmitte: Schwerpunkt ueber ALLE Punkte.
  //
  // NICHT der Punkt mit dem groessten Betrag. Am Gipfel ist die Kurve flach,
  // und bei 2101 verrauschten Messpunkten ist das Maximum ein Zufallspunkt
  // irgendwo in der Gipfelregion. AM AUFBAU GEMESSEN (22.08.2026): mit dem
  // Maximalpunkt als Zentrum blieb ein Ausreisser von 33 mV = 25 Schritten in
  // einem von vier Laeufen. Ein Schwerpunkt ist gemittelt und damit stabil.
  // -------------------------------------------------------------------------
  double sg = 0.0, sgp = 0.0;
  for (size_t i = 0; i < anzahl; i++) {
    const float ab = std::fabs(messwerte[i] - ruhe);
    if (ab < schwelle)
      continue;
    const double g = ab - schwelle;
    sg += g;
    sgp += g * static_cast<double>(positionen[i]);
  }
  if (sg <= 0.0)
    return false;  // kein Feld gesehen
  double mitte = sgp / sg;

  // -------------------------------------------------------------------------
  // 2. ITERIEREN BIS ZUR RUHE: Fenster um die Mitte legen, Schwerpunkt neu
  //    rechnen, Fenster nachziehen. Der erste Wert ist durch einseitige
  //    Erfassung verschoben; jede Runde zieht ihn naeher an den echten Gipfel,
  //    und liegt das Fenster symmetrisch, bleibt er stehen.
  //
  //    Abbruch, wenn sich die Mitte um weniger als einen halben Schritt
  //    bewegt - feiner als ein Schritt braucht niemand. Eine FESTE Rundenzahl
  //    genuegt nicht: mit drei Runden blieb im Test ein Rest von 11 Schritten
  //    stehen, weil das Verfahren geometrisch konvergiert und nicht in einem
  //    Sprung. Die Obergrenze von 12 ist nur die Reissleine gegen ein
  //    Pendeln, das bei einer krummen Kurve auftreten koennte.
  // -------------------------------------------------------------------------
  for (int runde = 0; runde < 12; runde++) {
    const double vorher = mitte;
    const double von = mitte - halbbreite;
    const double bis = mitte + halbbreite;

    // Beidseitige Abdeckung pruefen. Fehlt eine Seite, ist das Ergebnis genau
    // so verzerrt wie ohne Fenster - dann lieber abbrechen als einen
    // Offset melden, dem niemand trauen kann.
    bool links = false, rechts = false;
    for (size_t i = 0; i < anzahl; i++) {
      if (positionen[i] <= von)
        links = true;
      if (positionen[i] >= bis)
        rechts = true;
    }
    if (!links || !rechts)
      return false;

    double g_sum = 0.0, gp_sum = 0.0;
    for (size_t i = 0; i < anzahl; i++) {
      if (positionen[i] < von || positionen[i] > bis)
        continue;
      const float ab = std::fabs(messwerte[i] - ruhe);
      if (ab < schwelle)
        continue;
      const double g = ab - schwelle;
      g_sum += g;
      gp_sum += g * static_cast<double>(positionen[i]);
    }
    if (g_sum <= 0.0)
      return false;
    mitte = gp_sum / g_sum;

    if (std::fabs(mitte - vorher) < 0.5)
      break;  // steht
  }

  *ergebnis = static_cast<float>(mitte);
  return true;
}

int32_t offset_einmessen(int32_t offset_alt, int32_t position, int32_t rev) {
  if (rev <= 0)
    return 0;
  return normiere(offset_alt - position, rev);
}

// ===========================================================================
// Timer
// ===========================================================================

namespace {
/// Eine Zahl aus der Version lesen und den Zeiger hinter den Punkt setzen.
uint32_t semver_teil(const char *&p) {
  uint32_t n = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + static_cast<uint32_t>(*p - '0');
    p++;
  }
  if (*p == '.')
    p++;
  else
    p = "";  // Rest ist Anhaengsel oder Ende - weitere Stellen sind 0
  return n;
}
}  // namespace

int semver_vergleich(const char *a, const char *b) {
  if (a == nullptr || b == nullptr)
    return 0;
  for (int i = 0; i < 3; i++) {
    const uint32_t x = semver_teil(a);
    const uint32_t y = semver_teil(b);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return 0;
}

uint32_t fahrzeit_ms(int32_t weg_schritte, uint32_t beschleunigung,
                     uint32_t tempo_max) {
  if (weg_schritte <= 0 || beschleunigung == 0 || tempo_max == 0)
    return 0;

  const double weg = static_cast<double>(weg_schritte);
  const double a = static_cast<double>(beschleunigung);
  const double vmax = static_cast<double>(tempo_max);

  // Spitzentempo, wenn nur beschleunigt und gebremst wird.
  const double v_dreieck = std::sqrt(weg * a);

  double sekunden;
  if (v_dreieck <= vmax) {
    sekunden = 2.0 * std::sqrt(weg / a);
  } else {
    // Trapez: hochfahren, halten, abbremsen.
    sekunden = 2.0 * vmax / a + (weg - vmax * vmax / a) / vmax;
  }
  return static_cast<uint32_t>(std::lround(sekunden * 1000.0));
}

uint32_t timer_mindestdauer_ms(int32_t rev, uint32_t beschleunigung,
                              uint32_t tempo_max) {
  // Eine Umdrehung ist nur ein Sonderfall der Fahrzeit.
  return fahrzeit_ms(rev, beschleunigung, tempo_max);
}

int32_t timer_ziel(int32_t start, uint32_t verstrichen_ms, uint32_t dauer_ms,
                   int32_t rev) {
  if (rev <= 0)
    return start;
  if (dauer_ms == 0)
    return start + rev;  // ohne Dauer ist der Timer sofort um

  // In double rechnen, nicht in float: bei zwoelf Stunden sind dauer_ms rund
  // 43,2 Millionen, und float hat dort nur noch 24 Bit Mantisse - der
  // Quotient wuerde in Stufen springen statt gleichmaessig zu wachsen.
  double anteil = static_cast<double>(verstrichen_ms) / static_cast<double>(dauer_ms);
  if (anteil < 0.0)
    anteil = 0.0;
  if (anteil > 1.0)
    anteil = 1.0;

  return start + static_cast<int32_t>(std::lround(anteil * static_cast<double>(rev)));
}

std::string timer_restzeit(uint32_t verstrichen_ms, uint32_t dauer_ms) {
  if (dauer_ms == 0)
    return "--:--:--";

  const uint32_t rest_ms = (verstrichen_ms >= dauer_ms) ? 0 : (dauer_ms - verstrichen_ms);
  // Aufrunden: solange noch eine angefangene Sekunde laeuft, soll nicht 0
  // dastehen. Sonst zeigt die Anzeige eine Sekunde lang "abgelaufen", waehrend
  // der Zeiger noch faehrt.
  const uint32_t rest_s = (rest_ms + 999) / 1000;

  char puffer[16];
  std::snprintf(puffer, sizeof(puffer), "%u:%02u:%02u",
                static_cast<unsigned>(rest_s / 3600),
                static_cast<unsigned>((rest_s % 3600) / 60),
                static_cast<unsigned>(rest_s % 60));
  return std::string(puffer);
}

// ===========================================================================
// Helligkeit
// ===========================================================================

float tastverhaeltnis(float prozent, float min_prozent) {
  if (prozent <= 0.0f)
    return 0.0f;  // 0 heisst wirklich aus, nicht Glimmschwelle
  if (prozent >= 100.0f)
    return 1.0f;

  // Unsinnige Untergrenzen nicht durchlassen - sonst spreizt die Rechnung
  // auf einen leeren Bereich und liefert Werte ausserhalb 0..1.
  float u = min_prozent;
  if (u < 0.0f || u >= 100.0f)
    u = 0.0f;

  // Der Regelweg 0..100 wird auf u..100 abgebildet: das erste Prozent am
  // Regler ist genau die Glimmschwelle.
  const float anteil = (u + (100.0f - u) * (prozent / 100.0f)) / 100.0f;
  if (anteil < 0.0f)
    return 0.0f;
  if (anteil > 1.0f)
    return 1.0f;
  return anteil;
}

// ===========================================================================
// Kollisionsregel
// ===========================================================================

Fuehrung fuehrung_bestimmen(bool homing_laeuft, bool hand_faehrt,
                            bool timer_laeuft, bool uhr_an) {
  if (homing_laeuft)
    return Fuehrung::homing;
  if (hand_faehrt)
    return Fuehrung::hand;
  if (timer_laeuft)
    return Fuehrung::timer;
  if (uhr_an)
    return Fuehrung::uhr;
  return Fuehrung::niemand;
}

}  // namespace starwatch
