#include "zeiger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

bool Feinfahrtende::fertig(int32_t position, float messwert) {
  const float ab = std::fabs(messwert - ruhe_);
  if (ab > gipfel_) {
    gipfel_ = ab;
    gipfel_pos_ = position;
  }
  if (gipfel_ < mindestgipfel_)
    return false;
  if (position - gipfel_pos_ < nach_gipfel_)
    return false;
  return ab < gipfel_ * anteil_;
}

int32_t feldbreite(const int32_t *positionen, const float *messwerte, size_t anzahl,
                   float ruhe, float schwelle) {
  if (positionen == nullptr || messwerte == nullptr)
    return 0;
  bool gesehen = false;
  int32_t von = 0, bis = 0;
  for (size_t i = 0; i < anzahl; i++) {
    if (std::fabs(messwerte[i] - ruhe) <= schwelle)
      continue;
    if (!gesehen || positionen[i] < von)
      von = positionen[i];
    if (!gesehen || positionen[i] > bis)
      bis = positionen[i];
    gesehen = true;
  }
  return gesehen ? bis - von : 0;
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
// Homing-Verlauf
// ===========================================================================

Verlustbefund schrittverlust(int32_t abweichung, int32_t periode, int32_t toleranz) {
  Verlustbefund b;
  if (periode <= 0)
    return b;
  const int32_t betrag = abweichung < 0 ? -abweichung : abweichung;
  b.verlust = betrag > toleranz;
  // Symmetrisch um null runden - Integerdivision schneidet bei negativen
  // Zahlen sonst in die falsche Richtung ab.
  const int32_t p = (betrag + periode / 2) / periode;
  b.perioden = abweichung < 0 ? -p : p;
  b.rest = abweichung - b.perioden * periode;
  const int32_t rest_betrag = b.rest < 0 ? -b.rest : b.rest;
  b.periodisch = b.verlust && rest_betrag <= toleranz;
  return b;
}

const char *hominganlass_text(Hominganlass a) {
  switch (a) {
    case Hominganlass::start:     return "Start";
    case Hominganlass::hand:      return "Hand";
    case Hominganlass::alles_ein: return "Alles ein";
    case Hominganlass::taeglich:  return "taeglich";
    case Hominganlass::nachholen: return "nachholen";
    case Hominganlass::offset:    return "Offset";
  }
  return "?";
}

void Homingverlauf::hinzu(const Homingeintrag &e) {
  d_.e[d_.naechster] = e;
  d_.naechster = static_cast<uint8_t>((d_.naechster + 1) % N);
  if (d_.anzahl < N)
    d_.anzahl++;
}

const Homingeintrag &Homingverlauf::neuer(size_t i) const {
  static const Homingeintrag leer;
  if (i >= d_.anzahl)
    return leer;
  return d_.e[(d_.naechster + N - 1 - i) % N];
}

bool Homingverlauf::laden(const void *daten, size_t groesse) {
  d_ = Daten();
  if (daten == nullptr || groesse != sizeof(Daten))
    return false;
  Daten d;
  memcpy(&d, daten, sizeof(Daten));
  if (d.version != VERSION || d.anzahl > N || d.naechster >= N)
    return false;
  // Ein nicht voller Puffer ist von vorn gefuellt: naechster == anzahl.
  if (d.anzahl < N && d.naechster != d.anzahl)
    return false;
  d_ = d;
  return true;
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

bool alias_pruefen(const std::string &roh, std::string *sauber) {
  size_t a = 0;
  size_t e = roh.size();
  while (a < e && roh[a] == ' ')
    a++;
  while (e > a && roh[e - 1] == ' ')
    e--;
  const std::string s = roh.substr(a, e - a);

  size_t zeichen = 0;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t folge = 0;
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7F || c == '"' || c == '\\' || c == '<' || c == '>')
        return false;
    } else if ((c & 0xE0) == 0xC0 && c >= 0xC2) {
      folge = 1;
    } else if ((c & 0xF0) == 0xE0) {
      folge = 2;
    } else if ((c & 0xF8) == 0xF0 && c <= 0xF4) {
      folge = 3;
    } else {
      return false;  // Folgebyte ohne Anfang oder ungueltiges Startbyte
    }
    for (size_t k = 1; k <= folge; k++) {
      if (i + k >= s.size() ||
          (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
        return false;
    }
    i += folge + 1;
    zeichen++;
  }
  if (zeichen > ALIAS_MAX_ZEICHEN)
    return false;
  *sauber = s;
  return true;
}

bool update_url_umstellen(std::string *url, const char *ordner,
                          const char *stabil, const char *beta) {
  if (url->rfind(ordner, 0) != 0 || *url == stabil || *url == beta)
    return false;
  *url = (url->find("beta") != std::string::npos) ? beta : stabil;
  return true;
}

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

uint32_t weiche_beschleunigung(uint32_t weg, uint32_t a_max, uint32_t min_ms) {
  if (a_max == 0)
    return 1;
  if (weg == 0 || min_ms == 0)
    return a_max;
  const double t = static_cast<double>(min_ms) / 1000.0;
  const double a = 4.0 * static_cast<double>(weg) / (t * t);
  if (a >= static_cast<double>(a_max))
    return a_max;
  return a < 1.0 ? 1u : static_cast<uint32_t>(a);
}

uint32_t weicher_einsatz(uint32_t weg, uint32_t max_schritte) {
  const uint32_t viertel = weg / 4;
  return viertel < max_schritte ? viertel : max_schritte;
}

namespace {
float klemme01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
float glatt(float x) {  // smoothstep
  x = klemme01(x);
  return x * x * (3.0f - 2.0f * x);
}
uint32_t mische(uint32_t a, uint32_t b, float t) {
  t = klemme01(t);
  uint32_t r = 0;
  for (int s = 16; s >= 0; s -= 8) {
    const float ka = static_cast<float>((a >> s) & 0xFF);
    const float kb = static_cast<float>((b >> s) & 0xFF);
    const float k = ka + (kb - ka) * t;
    r |= static_cast<uint32_t>(std::lround(k)) << s;
  }
  return r;
}
/// Kleiner Zufallsgenerator mit festem Samen - dieselbe Zuendung bei
/// demselben `zufall`, damit die Host-Tests etwas pruefen koennen.
uint32_t lcg(uint32_t *z) {
  *z = *z * 1664525u + 1013904223u;
  return *z >> 8;
}
/// Ein sanfter Flackerer: um `tiefe` einbrechen, `dauer` ms lang, mit
/// Cosinus-Flanken - kein harter Wechsel. 0 ausserhalb.
float flacker(float t, float ab, float dauer, float tiefe) {
  if (t < ab || t >= ab + dauer)
    return 0.0f;
  const float x = (t - ab) / dauer;
  return tiefe * 0.5f * (1.0f - std::cos(6.2831853f * x));
}
}  // namespace

uint32_t waermer(uint32_t farbe, float staerke) {
  farbe &= 0xFFFFFF;
  staerke = klemme01(staerke);
  if (staerke <= 0.0f)
    return farbe;
  const float r = ((farbe >> 16) & 0xFF) / 255.0f;
  const float g = ((farbe >> 8) & 0xFF) / 255.0f;
  const float b = (farbe & 0xFF) / 255.0f;
  const float mx = std::fmax(r, std::fmax(g, b));
  const float mn = std::fmin(r, std::fmin(g, b));
  if (mx <= 0.0f)
    return farbe;  // schwarz bleibt schwarz
  const float d = mx - mn;
  const float s = d / mx;  // Saettigung (HSV)
  float h = 0.0f;           // Farbton in Grad
  if (d > 0.0f) {
    if (mx == r)
      h = 60.0f * std::fmod((g - b) / d, 6.0f);
    else if (mx == g)
      h = 60.0f * ((b - r) / d + 2.0f);
    else
      h = 60.0f * ((r - g) / d + 4.0f);
    if (h < 0.0f)
      h += 360.0f;
  }
  // 1. FARBTON RICHTUNG ROT, auf dem kuerzeren Weg und um einen Anteil des
  //    Abstands: Gelb wird Orange, Gruen Gelbgruen, Blau Violett, Rot bleibt.
  const float weg = (h <= 180.0f) ? -h : (360.0f - h);
  h += weg * 0.35f * staerke;
  if (h < 0.0f)
    h += 360.0f;
  if (h >= 360.0f)
    h -= 360.0f;
  // Zurueck nach RGB, gleiche Saettigung und Helligkeit.
  const float c = mx * s;
  const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
  const float m = mx - c;
  float rr = 0, gg = 0, bb = 0;
  if (h < 60.0f)       { rr = c; gg = x; }
  else if (h < 120.0f) { rr = x; gg = c; }
  else if (h < 180.0f) { gg = c; bb = x; }
  else if (h < 240.0f) { gg = x; bb = c; }
  else if (h < 300.0f) { rr = x; bb = c; }
  else                 { rr = c; bb = x; }
  rr += m; gg += m; bb += m;
  // 2. WENIG GESAETTIGTES (Weiss, Pastell) wird ueber den Weissabgleich warm:
  //    weniger Blau, etwas weniger Gruen - wie eine Gluehlampe. Gesaettigte
  //    Farben beruehrt das kaum.
  const float blass = 1.0f - s;
  gg *= 1.0f - 0.25f * staerke * blass;
  bb *= 1.0f - 0.60f * staerke * blass;
  auto k = [](float v) { return static_cast<uint32_t>(std::lround(klemme01(v) * 255.0f)); };
  return (k(rr) << 16) | (k(gg) << 8) | k(bb);
}

Zuendbild zuend_bild(uint32_t t_ms, uint32_t alt, uint32_t ziel, uint32_t zufall) {
  // DRITTE FASSUNG (28.09.2026, am Geraet beurteilt). Die erste blinkte aus
  // dem Dunkeln hart zwischen aus und voll - Stroboskop, unangenehm. Die
  // zweite war dagegen glatt und traege: Flackern kaum zu sehen, nichts
  // Dynamisches. Jetzt: zuegiges Ausfahren und danach ein unruhiges Plasma
  // mit deutlichen, aber nie dunklen Flackerern.
  alt &= 0xFFFFFF;
  ziel &= 0xFFFFFF;
  if (t_ms >= ZUEND_DAUER_MS)
    return {1.0f, ziel};
  uint32_t z = zufall | 1u;
  const float t = static_cast<float>(t_ms);

  constexpr float VORGLUEH = 180.0f;
  constexpr float AUSFAHREN = 470.0f;
  constexpr float UNRUHE_BIS = 1700.0f;
  const float aus_ende = VORGLUEH + AUSFAHREN;  // 650 ms

  float anteil;
  float hitze;
  if (t < VORGLUEH) {
    anteil = 0.12f * glatt(t / VORGLUEH);
    hitze = 1.0f;
  } else if (t < aus_ende) {
    // Zuegig raus, zum Ende hin abbremsend (ease-out), bis 1,25.
    const float x = (t - VORGLUEH) / AUSFAHREN;
    anteil = 0.12f + 1.13f * (1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x));
    hitze = 1.0f;
  } else {
    // Von 1,25 ruhig auf 1,0 zurueck.
    const float x = (t - aus_ende) / (static_cast<float>(ZUEND_DAUER_MS) - aus_ende);
    anteil = 1.0f + 0.25f * (1.0f - glatt(x));
    hitze = std::exp(-(t - aus_ende) / 650.0f) * (1.0f - glatt(x));
  }

  // UNRUHIGES PLASMA: 4 bis 6 Flackerer zwischen dem Ausfahren und 1,7 s,
  // je 60-120 ms, 25-55 % tief, schnell rein und weicher raus, nach hinten
  // flacher. Danach mit 50 % ein kleiner Nachzucker.
  const int anzahl = 4 + static_cast<int>(lcg(&z) % 3);
  float ab = aus_ende + 40.0f + static_cast<float>(lcg(&z) % 80);
  float einbruch = 0.0f;
  for (int i = 0; i < anzahl && ab < UNRUHE_BIS; i++) {
    const float dauer = 60.0f + static_cast<float>(lcg(&z) % 60);
    const float spaet = (ab - aus_ende) / (UNRUHE_BIS - aus_ende);  // 0..1
    const float tiefe = (0.30f + 0.01f * static_cast<float>(lcg(&z) % 25)) * (1.0f - 0.45f * spaet);
    if (t >= ab && t < ab + dauer) {
      const float x = (t - ab) / dauer;
      // Rein in 30 % der Zeit, raus in 70 % - beides mit Cosinus-Flanken.
      const float f = x < 0.3f ? 0.5f * (1.0f - std::cos(3.14159265f * x / 0.3f))
                               : 0.5f * (1.0f + std::cos(3.14159265f * (x - 0.3f) / 0.7f));
      einbruch = tiefe * f;
    }
    ab += dauer + 80.0f + static_cast<float>(lcg(&z) % 170);
  }
  if ((lcg(&z) % 100) < 50) {
    const float nach_ab = 2000.0f + static_cast<float>(lcg(&z) % 400);
    einbruch += flacker(t, nach_ab, 110.0f, 0.15f);
  }
  anteil *= 1.0f - einbruch;
  if (anteil < 0.0f)
    anteil = 0.0f;

  // DIE FARBE IST VON ANFANG AN DIE NEUE - nur waermer, und sie kuehlt mit
  // der Hitze auf ihren echten Ton ab (01.10.2026: "wenn gelb eingestellt
  // wird ein warmes gelb und dann das eingestellte gelb"). Bis dahin glitt
  // sie von der alten zur neuen und wurde dabei Richtung Warmweiss #FFD764
  // gezogen - jeder Wechsel lief ueber Weiss, und zwischen weit entfernten
  // Farben (Blau -> Weiss) ueber ein trueben Braunrosa. `alt` wird deshalb
  // nicht mehr gebraucht; die Klinge ist beim Vorgluehen ohnehin fast dunkel.
  (void)alt;
  return {anteil, waermer(ziel, hitze)};
}

Zuendbild loesch_bild(uint32_t t_ms, uint32_t farbe, uint32_t zufall) {
  // Ebenso ohne Funkenregen: ein langsames Aufwallen, ruhiges Ausblenden,
  // weiches Nachglimmen - mit EINEM leichten Flackerer beim Ausblenden.
  uint32_t z = zufall | 1u;
  const float fl_ab = 350.0f + static_cast<float>(lcg(&z) % 200);
  const float fl2_ab = fl_ab + 220.0f + static_cast<float>(lcg(&z) % 200);
  farbe &= 0xFFFFFF;
  if (t_ms >= LOESCH_DAUER_MS)
    return {0.0f, farbe};
  const float t = static_cast<float>(t_ms);
  float anteil;
  float hitze;
  if (t < 250.0f) {
    const float x = t / 250.0f;
    anteil = 1.0f + 0.08f * std::sin(3.14159265f * x);
    hitze = 0.25f * x;
  } else if (t < 1500.0f) {
    const float x = glatt((t - 250.0f) / 1250.0f);
    anteil = 1.0f - 0.95f * x;
    hitze = 0.25f + 0.3f * x;
  } else {
    const float x = (t - 1500.0f) / (static_cast<float>(LOESCH_DAUER_MS) - 1500.0f);
    anteil = 0.05f * (1.0f - glatt(x));
    hitze = 0.55f;
  }
  anteil *= 1.0f - flacker(t, fl_ab, 100.0f, 0.35f) - flacker(t, fl2_ab, 80.0f, 0.25f);
  // Beim Einfahren wird die Farbe waermer, wie ein abkuehlender Gluehfaden -
  // seit 01.10.2026 in derselben Farbe, nicht mehr Richtung Warmweiss.
  return {anteil, waermer(farbe, hitze / 0.55f)};
}

uint32_t farbe_mischen(uint32_t a, uint32_t b, float x) { return mische(a & 0xFFFFFF, b & 0xFFFFFF, x); }

float uebergang_wert(float von, float nach, uint32_t t_ms, uint32_t dauer_ms) {
  if (dauer_ms == 0 || t_ms >= dauer_ms)
    return nach;
  const float x = static_cast<float>(t_ms) / static_cast<float>(dauer_ms);
  return von + (nach - von) * x;
}

float puls_min_begrenzt(float anteil) {
  if (!(anteil == anteil))
    return PULS_MIN_VORGABE;
  return anteil < PULS_MIN_UNTEN ? PULS_MIN_UNTEN : (anteil > PULS_MIN_OBEN ? PULS_MIN_OBEN : anteil);
}

uint32_t puls_periode_begrenzt(uint32_t ms) {
  return ms < PULS_PERIODE_MIN_MS ? PULS_PERIODE_MIN_MS
                                  : (ms > PULS_PERIODE_MAX_MS ? PULS_PERIODE_MAX_MS : ms);
}

float puls_anteil(float phase, float min_anteil) {
  min_anteil = puls_min_begrenzt(min_anteil);
  const float welle = 0.5f - 0.5f * std::cos(6.2831853f * phase);
  return min_anteil + (1.0f - min_anteil) * welle;
}

float puls_phase_fuer(float anteil, float min_anteil) {
  min_anteil = puls_min_begrenzt(min_anteil);
  float w = (anteil - min_anteil) / (1.0f - min_anteil);
  w = klemme01(w);
  // welle = 0,5 - 0,5 cos(2 pi p)  ->  p = acos(1 - 2 w) / (2 pi), in 0..0,5
  return std::acos(1.0f - 2.0f * w) / 6.2831853f;
}

float puls_phase_weiter(float phase, uint32_t dt_ms, uint32_t periode_ms) {
  periode_ms = puls_periode_begrenzt(periode_ms);
  phase += static_cast<float>(dt_ms) / static_cast<float>(periode_ms);
  phase -= std::floor(phase);
  return phase;
}

namespace {
/// Stelle des Werts hinter "schluessel": in `json` ab `ab`, npos wenn fehlt.
size_t wert_nach(const std::string &json, const char *schluessel, size_t ab = 0,
                 size_t bis = std::string::npos) {
  const std::string k = std::string("\"") + schluessel + "\"";
  size_t p = json.find(k, ab);
  if (p == std::string::npos || (bis != std::string::npos && p >= bis))
    return std::string::npos;
  p += k.size();
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
    p++;
  if (p >= json.size() || json[p] != ':')
    return std::string::npos;
  p++;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t'))
    p++;
  return p < json.size() ? p : std::string::npos;
}
std::string text_ab(const std::string &json, size_t p) {
  if (p == std::string::npos || json[p] != '"')
    return "";
  const size_t e = json.find('"', p + 1);
  return e == std::string::npos ? "" : json.substr(p + 1, e - p - 1);
}
bool zahl_ab(const std::string &json, size_t p, double *z) {
  if (p == std::string::npos)
    return false;
  char *ende = nullptr;
  const double v = std::strtod(json.c_str() + p, &ende);
  if (ende == json.c_str() + p)
    return false;
  *z = v;
  return true;
}
}  // namespace

bool lichtbefehl_lesen(const char *json, Lichtbefehl *b) {
  if (json == nullptr || b == nullptr)
    return false;
  *b = Lichtbefehl{};
  const std::string j(json);

  const std::string st = text_ab(j, wert_nach(j, "state"));
  if (st == "ON") b->state = 1;
  else if (st == "OFF") b->state = 0;

  double z = 0.0;
  if (zahl_ab(j, wert_nach(j, "brightness"), &z))
    b->hell = z < 0.0 ? 0 : (z > 100.0 ? 100 : static_cast<int>(z + 0.5));

  const size_t pc = wert_nach(j, "color");
  if (pc != std::string::npos && j[pc] == '{') {
    const size_t ende = j.find('}', pc);
    double r = -1, g = -1, bl = -1;
    if (zahl_ab(j, wert_nach(j, "r", pc, ende), &r) && zahl_ab(j, wert_nach(j, "g", pc, ende), &g) &&
        zahl_ab(j, wert_nach(j, "b", pc, ende), &bl) && r >= 0 && r <= 255 && g >= 0 &&
        g <= 255 && bl >= 0 && bl <= 255) {
      b->farbe_da = true;
      b->farbe = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                 static_cast<uint32_t>(bl);
    }
  }

  if (zahl_ab(j, wert_nach(j, "transition"), &z)) {
    if (z < 0.0) z = 0.0;
    if (z > 3600.0) z = 3600.0;
    b->uebergang_ms = static_cast<int32_t>(z * 1000.0 + 0.5);
  }

  const size_t pe = wert_nach(j, "effect");
  if (pe != std::string::npos) {
    const std::string e = text_ab(j, pe);
    b->effekt = (e == "none" || e.empty()) ? 0 : (e == "Puls" ? 1 : 2);
  }

  const std::string fl = text_ab(j, wert_nach(j, "flash"));
  if (fl == "short") b->blitze = 1;
  else if (fl == "long") b->blitze = 3;
  return true;
}

Zuendbild blende_bild(uint32_t t_ms, float von, float nach, uint32_t alt, uint32_t ziel) {
  alt &= 0xFFFFFF;
  ziel &= 0xFFFFFF;
  if (t_ms >= BLENDE_DAUER_MS)
    return {nach, ziel};
  const float x = glatt(static_cast<float>(t_ms) / static_cast<float>(BLENDE_DAUER_MS));
  return {von + (nach - von) * x, mische(alt, ziel, x)};
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

int32_t timer_sprungziel(int32_t start, uint32_t verstrichen_ms, uint32_t dauer_ms,
                         int32_t rev) {
  return timer_ziel(start, verstrichen_ms - verstrichen_ms % 1000u, dauer_ms, rev);
}

uint32_t timer_gleittempo_mhz(int32_t rev, uint32_t dauer_ms, int32_t rueckstand,
                              uint32_t ausgleich_ms, uint32_t tempo_max) {
  if (rev <= 0 || dauer_ms == 0)
    return 0;

  // In double: rev * 1e6 sprengt bei 1/256 Mikroschritten die 32 Bit.
  const double nenn = static_cast<double>(rev) * 1.0e6 / static_cast<double>(dauer_ms);
  double mhz = nenn;
  if ((rueckstand > 1 || rueckstand < -1) && ausgleich_ms > 0)
    mhz += static_cast<double>(rueckstand) * 1.0e6 / static_cast<double>(ausgleich_ms);

  const double unten = nenn / 2.0;
  const double oben = static_cast<double>(tempo_max) * 1000.0;
  if (mhz < unten)
    mhz = unten;
  if (oben > 0.0 && mhz > oben)
    mhz = oben;
  if (mhz < 1.0)
    mhz = 1.0;
  return static_cast<uint32_t>(std::lround(mhz));
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
// Farbe
// ===========================================================================

namespace {
int hexziffer(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

namespace {
struct Farbname {
  const char *name;
  uint32_t farbe;
};
// Am Geraet abgestimmt. Weiss (27.09.2026) ist warm, 255/215/100 - volles
// RGB wirkt kalt-blaeulich; es liegt mit rund 2,1 W unter der
// Leistungsgrenze und leuchtet voll. Kaltweiss geht weiter als ffffff.
// Gelb, Blau und Violett nachgezogen am 28.09.2026 nach Vorgabe:
//   Gelb    255/110/0   (vorher 255/128/0 - noch zu gruenlich)
//   Blau    0/20/255    (ein Hauch Gruen, reines Blau wirkt an der Klinge tintig)
//   Violett 155/0/255   (vorher 255/0/255 - das war eher Magenta)
constexpr Farbname FARBNAMEN[] = {
    {"rot", 0xFF0000},     {"gruen", 0x00FF00},   {"gr\xc3\xbcn", 0x00FF00},
    {"blau", 0x0014FF},    {"gelb", 0xFF6E00},    {"violett", 0x9B00FF},
    {"weiss", 0xFFD764},   {"wei\xc3\x9f", 0xFFD764}, {"orange", 0xFF3000},
    {"tuerkis", 0x00FFFF}, {"t\xc3\xbcrkis", 0x00FFFF},
};

/// Vergleich ohne Ruecksicht auf Gross/klein (nur ASCII - die Umlaute der
/// Namen oben kommen klein an oder gar nicht). `ende` ist das erste Zeichen
/// nach dem Wort.
bool wort_gleich(const char *a, const char *ende, const char *b) {
  while (a < ende && *b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (x != y)
      return false;
    a++;
    b++;
  }
  return a == ende && *b == '\0';
}
}  // namespace

bool farbe_lesen(const char *text, uint32_t *farbe, uint32_t weiss) {
  if (text == nullptr || farbe == nullptr)
    return false;
  while (*text == ' ')
    text++;

  // Ein Name, ganz fuer sich (danach hoechstens Leerzeichen).
  {
    const char *e = text;
    while (*e != '\0' && *e != ' ')
      e++;
    const char *r = e;
    while (*r == ' ')
      r++;
    if (*r == '\0')
      for (const Farbname &f : FARBNAMEN)
        if (wort_gleich(text, e, f.name)) {
          *farbe = f.farbe == 0xFFD764 ? (weiss & 0xFFFFFF) : f.farbe;
          return true;
        }
  }

  // Drei Dezimalzahlen "r g b". Erkannt daran, dass ein Leerzeichen folgt -
  // "255" allein ist KEINE Farbe, sondern ein Vertipper.
  const char *p = text;
  uint32_t teile[3] = {0, 0, 0};
  int n = 0;
  bool dezimal = true;
  while (n < 3) {
    while (*p == ' ')
      p++;
    if (*p < '0' || *p > '9') {
      dezimal = false;
      break;
    }
    uint32_t w = 0;
    int stellen = 0;
    while (*p >= '0' && *p <= '9') {
      w = w * 10 + static_cast<uint32_t>(*p - '0');
      if (++stellen > 3) {
        dezimal = false;
        break;
      }
      p++;
    }
    if (!dezimal || w > 255 || (*p != ' ' && *p != '\0')) {
      dezimal = false;
      break;
    }
    teile[n++] = w;
  }
  if (dezimal && n == 3) {
    while (*p == ' ')
      p++;
    if (*p == '\0') {
      *farbe = (teile[0] << 16) | (teile[1] << 8) | teile[2];
      return true;
    }
  }

  // Sechs Hexziffern, wahlweise mit '#' davor, danach nichts mehr.
  p = text;
  if (*p == '#')
    p++;
  uint32_t w = 0;
  for (int i = 0; i < 6; i++) {
    const int z = hexziffer(p[i]);
    if (z < 0)
      return false;
    w = (w << 4) | static_cast<uint32_t>(z);
  }
  p += 6;
  while (*p == ' ')
    p++;
  if (*p != '\0')
    return false;
  *farbe = w;
  return true;
}

float kanal_anteile(float anteil, uint32_t farbe, const float watt_voll[3],
                    float max_w, float aus[3]) {
  // NaN faellt durch jeden Vergleich - hier wie im Dimmfaktor ein dunkler
  // Kanal ohne jede Meldung. Deshalb als "nicht >= 0" gefangen.
  if (!(anteil >= 0.0f))
    anteil = 0.0f;
  if (anteil > 1.0f)
    anteil = 1.0f;
  // Eine unsinnige Grenze heisst DUNKEL, nicht unbegrenzt: hier geht es um
  // Waerme, und im Zweifel ist aus die sichere Seite.
  if (!(max_w >= 0.0f))
    max_w = 0.0f;

  const uint32_t teil[3] = {(farbe >> 16) & 0xFF, (farbe >> 8) & 0xFF,
                            farbe & 0xFF};
  float watt = 0.0f;
  for (int k = 0; k < 3; k++) {
    aus[k] = anteil * (static_cast<float>(teil[k]) / 255.0f);
    const float w = watt_voll[k] > 0.0f ? watt_voll[k] : 0.0f;
    watt += aus[k] * w;
  }
  if (watt > max_w) {
    const float faktor = watt > 0.0f ? max_w / watt : 0.0f;
    for (int k = 0; k < 3; k++)
      aus[k] *= faktor;
    watt = max_w;
  }
  return watt;
}

// ===========================================================================
// Lichtsignale
// ===========================================================================

const char *muster_text(Muster m) {
  switch (m) {
    case Muster::leuchten:  return "leuchten";
    case Muster::pulsieren: return "pulsieren";
    case Muster::blinken:   return "blinken";
  }
  return "leuchten";
}

bool muster_lesen(const char *text, Muster *m) {
  if (text == nullptr || m == nullptr)
    return false;
  const char *e = text;
  while (*e != '\0' && *e != ' ')
    e++;
  for (Muster x : {Muster::leuchten, Muster::pulsieren, Muster::blinken})
    if (wort_gleich(text, e, muster_text(x))) {
      *m = x;
      return true;
    }
  return false;
}

float muster_anteil(Muster m, uint32_t seit_ms) {
  switch (m) {
    case Muster::leuchten:
      return 1.0f;
    case Muster::pulsieren: {
      constexpr uint32_t ZUG_MS = 2400;
      constexpr float UNTEN = 0.15f;
      const float phase = static_cast<float>(seit_ms % ZUG_MS) / ZUG_MS;
      // cos beginnt bei 1: der Zug faengt oben an.
      const float welle = 0.5f + 0.5f * std::cos(phase * 6.2831853f);
      return UNTEN + (1.0f - UNTEN) * welle;
    }
    case Muster::blinken:
      return (seit_ms % 1000u) < 500u ? 1.0f : 0.0f;
  }
  return 1.0f;
}

bool meldung_lesen(const char *text, Meldung *m) {
  if (text == nullptr || m == nullptr)
    return false;
  Meldung neu;
  const char *p = text;
  while (*p == ' ')
    p++;
  if (*p == '\0')
    return false;

  // Die Farbe ist das erste Wort. farbe_lesen() will den Text fuer sich
  // allein - also das Wort herausschneiden.
  char wort[24];
  size_t n = 0;
  while (p[n] != '\0' && p[n] != ' ') {
    if (n + 1 >= sizeof(wort))
      return false;
    wort[n] = p[n];
    n++;
  }
  wort[n] = '\0';
  if (!farbe_lesen(wort, &neu.farbe))
    return false;
  p += n;

  int zahlen = 0;
  bool muster_da = false;
  while (true) {
    while (*p == ' ')
      p++;
    if (*p == '\0')
      break;
    if (*p >= '0' && *p <= '9') {
      uint32_t w = 0;
      int stellen = 0;
      while (*p >= '0' && *p <= '9') {
        if (++stellen > 6)
          return false;
        w = w * 10 + static_cast<uint32_t>(*p - '0');
        p++;
      }
      if (*p != ' ' && *p != '\0')
        return false;
      if (zahlen == 0) {
        if (w > MELDUNG_DAUER_MAX_S)
          return false;
        neu.dauer_s = w;
      } else if (zahlen == 1) {
        if (w < 1 || w > 100)
          return false;
        neu.hell = static_cast<float>(w);
      } else {
        return false;
      }
      zahlen++;
    } else {
      if (muster_da || !muster_lesen(p, &neu.muster))
        return false;
      muster_da = true;
      while (*p != '\0' && *p != ' ')
        p++;
    }
  }
  *m = neu;
  return true;
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

// ===========================================================================
// Umgebungslicht
// ===========================================================================

float licht_anteil(float lux, float lux_dunkel, float lux_hell,
                   float anteil_min) {
  // Unsinnige Grenzen: voll aufdrehen statt abdunkeln. Eine Uhr, die wegen
  // einer verdrehten Einstellung dunkel bleibt, sieht kaputt aus; eine, die
  // zu hell leuchtet, sieht nach einer falschen Einstellung aus - und das
  // ist es auch.
  if (!(lux_dunkel > 0.0f) || !(lux_hell > lux_dunkel))
    return 1.0f;

  if (anteil_min < 0.0f) anteil_min = 0.0f;
  if (anteil_min > 1.0f) anteil_min = 1.0f;

  // NaN faellt durch beide Vergleiche und kaeme sonst als NaN wieder heraus.
  // Ein NaN im Tastverhaeltnis ist ein dunkler Ausgang ohne Meldung.
  if (!(lux > lux_dunkel))
    return anteil_min;
  if (lux >= lux_hell)
    return 1.0f;

  const float t = std::log(lux / lux_dunkel) / std::log(lux_hell / lux_dunkel);
  return anteil_min + (1.0f - anteil_min) * t;
}

float dimm_nachfuehren(float ist, float ziel, uint32_t dt_ms, uint32_t tau_ms) {
  auto klemme = [](float v) {
    if (!(v >= 0.0f))  // auch NaN
      return v != v ? 1.0f : 0.0f;
    return v > 1.0f ? 1.0f : v;
  };
  ist = klemme(ist);
  ziel = klemme(ziel);
  if (tau_ms == 0)
    return ziel;
  const float k = 1.0f - std::exp(-static_cast<float>(dt_ms) / static_cast<float>(tau_ms));
  const float neu = ist + (ziel - ist) * k;
  if (std::fabs(ziel - neu) < 0.0005f)
    return ziel;
  return neu;
}

Lichtautomatik lichtautomatik_begrenzt(Lichtautomatik a, float lux_dunkel) {
  auto klemme = [](float v, float lo, float hi, float ersatz) {
    if (!(v == v))  // NaN aus einem kaputten Speicherinhalt
      return ersatz;
    return v < lo ? lo : (v > hi ? hi : v);
  };
  a.anteil_min = klemme(a.anteil_min, 0.05f, 1.0f, 0.30f);
  // Voll hell muss ueber der Dunkelgrenze liegen, sonst gibt es keine
  // Kennlinie mehr, sondern eine Stufe.
  const float hell_min = lux_dunkel * 10.0f > 10.0f ? lux_dunkel * 10.0f : 10.0f;
  a.lux_hell = klemme(a.lux_hell, hell_min, 2000.0f, 20.0f);
  a.nacht_aus = klemme(a.nacht_aus, 0.1f, 20.0f, 0.2f);
  if (a.nacht_aus * 2.0f > a.lux_hell)
    a.nacht_aus = a.lux_hell / 2.0f;
  return a;
}

float nacht_an_lux(const Lichtautomatik &a) {
  const float an = a.nacht_aus * 1.35f;
  const float grenze = a.lux_hell / 2.0f;
  return an < grenze ? an : grenze;
}

Nachtschalter::Nachtschalter(float aus_lux, float an_lux, uint32_t frist_ms,
                             uint32_t frist_an_ms)
    : aus_lux_(aus_lux), an_lux_(an_lux), frist_ms_(frist_ms),
      frist_an_ms_(frist_an_ms == UINT32_MAX ? frist_ms : frist_an_ms) {}

bool Nachtschalter::aktualisiere(float lux, uint32_t jetzt_ms) {
  // Welcher Wechsel steht ueberhaupt an? Nur EINER kann es sein - der
  // jeweils andere ist durch den aktuellen Zustand ausgeschlossen. Genau das
  // ist die Hysterese: zwischen den beiden Schwellen passiert nichts, und
  // zwar in beiden Richtungen.
  const bool will_aus = an_ && lux < aus_lux_;
  const bool will_an = !an_ && lux > an_lux_;

  if (!will_aus && !will_an) {
    // Laeuft der Countdown zum AUSschalten und liegt der Wert nur knapp ueber
    // der Schwelle, zaehlt er weiter - siehe abbruch_lux().
    if (an_ && wechsel_angemeldet_ && lux <= abbruch_lux())
      return an_;
    // Im Haltebereich. Eine angefangene Frist verfaellt - sie soll nur
    // zaehlen, solange der Grund dafuer anhaelt.
    wechsel_angemeldet_ = false;
    return an_;
  }

  if (!wechsel_angemeldet_) {
    wechsel_angemeldet_ = true;
    wechsel_seit_ = jetzt_ms;
  }

  // Die Subtraktion vorzeichenloser 32-Bit-Werte ist beim Ueberlauf von
  // millis() richtig - nach 49,7 Tagen Laufzeit. Ein Vergleich der beiden
  // Zeitpunkte waere es nicht.
  if (jetzt_ms - wechsel_seit_ >= (an_ ? frist_ms_ : frist_an_ms_)) {
    an_ = !an_;
    wechsel_angemeldet_ = false;
  }
  return an_;
}

uint32_t Nachtschalter::frist_laeuft_ms(uint32_t jetzt_ms) const {
  if (!wechsel_angemeldet_)
    return 0;
  return jetzt_ms - wechsel_seit_;
}

// ===========================================================================
// Einrichtung per Bluetooth
// ===========================================================================

static void json_zeichen(std::string *s, char ch) {
  const unsigned char u = static_cast<unsigned char>(ch);
  if (ch == '"' || ch == '\\') {
    s->push_back('\\');
    s->push_back(ch);
  } else if (u < 0x20) {
    char b[8];
    snprintf(b, sizeof(b), "\\u%04x", u);
    s->append(b);
  } else {
    s->push_back(ch);
  }
}

std::string netze_json(std::vector<Funknetz> netze, size_t max_bytes) {
  std::sort(netze.begin(), netze.end(),
            [](const Funknetz &a, const Funknetz &b) { return a.rssi > b.rssi; });
  std::string j = "[";
  std::vector<std::string> gesehen;
  for (const Funknetz &n : netze) {
    if (n.ssid.empty() || std::find(gesehen.begin(), gesehen.end(), n.ssid) != gesehen.end())
      continue;
    gesehen.push_back(n.ssid);
    std::string e = j.size() > 1 ? "," : "";
    e += "{\"ssid\":\"";
    for (char ch : n.ssid)
      json_zeichen(&e, ch);
    char rest[48];
    snprintf(rest, sizeof(rest), "\",\"rssi\":%d,\"offen\":%s}", n.rssi,
             n.offen ? "true" : "false");
    e += rest;
    if (j.size() + e.size() + 1 > max_bytes)
      break;
    j += e;
  }
  return j + "]";
}

bool wlan_auftrag_lesen(const std::string &daten, std::string *ssid, std::string *pass) {
  const size_t nl = daten.find('\n');
  const std::string s = daten.substr(0, nl);
  const std::string p = nl == std::string::npos ? "" : daten.substr(nl + 1);
  if (s.empty() || s.size() > 32)
    return false;
  if (!p.empty() && (p.size() < 8 || p.size() > 64))
    return false;
  *ssid = s;
  *pass = p;
  return true;
}

}  // namespace starwatch
