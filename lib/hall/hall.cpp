#include "hall.h"

#include <Arduino.h>
#include <geraet.h>
#include <zeiger.h>

#include <utility>  // std::swap

namespace starwatch {

namespace g = geraet;

namespace {
/// Weg der Grobsuche. Etwas mehr als eine Umdrehung, damit der Magnet auch
/// dann gefunden wird, wenn er beim Start knapp hinter dem Sensor stand.
constexpr int32_t GROB_RESERVE = 200;

/// Weg der Feinanfahrt. ABGELEITET, nicht getippt: der Rueckzug muss wieder
/// aufgeholt werden, danach ist Platz fuer das Fenster.
///
/// Der Pruefstand hat hier 360 bei einem Rueckzug von 160 - also 200 Schritte
/// nach der Flanke. Wird der Rueckzug wegen eines breiteren Fensters erhoeht
/// (AP 0.1), muss dieser Weg mitwachsen, sonst ueberstreicht die langsame
/// Fahrt das Fenster nicht mehr ganz. Genau deshalb steht hier eine Rechnung.
/// Weg NACH der Flanke. Muss das Fenster abdecken, nicht nur hineinreichen:
/// gemessen sind 429 Schritte (22.08.2026). Mit den 200 des Pruefstands endete
/// die Fahrt im Feld, und dann fehlt dem Schwerpunkt die abfallende Flanke -
/// er wird nach vorn verzerrt.
/// 23.08.2026: 550 deckte die neu gemessenen 520..567 Schritte nicht mehr
/// ganz ab. 650 tut es wieder, mit derselben Reserve wie der Rueckzug.
constexpr int32_t FEIN_NACH_FLANKE = 650;
constexpr int32_t FEIN_WEG = g::HALL_RUECKZUG_SCHRITTE + FEIN_NACH_FLANKE;

/// Tempo der Feinanfahrt. Bei 25 Schritten/s und einer Messung alle 20 ms
/// fallen rund zwei Messpunkte je Schritt an. Weil die Fahrt gleichfoermig
/// laeuft, sind die Punkte gleichmaessig ueber den Weg verteilt - der
/// Schwerpunkt wird dadurch nicht verzerrt.
/// Tempo der Feinanfahrt. 25 Schritte/s bei einer Messung alle 20 ms gibt
/// ZWEI Messpunkte je Schritt.
///
/// Kurz auf 50 gestanden, weil der Weg mit dem gemessenen Fenster auf 1050
/// Schritte gewachsen ist. Dann streute der Schwerpunkt aber ueber drei Laeufe
/// um 13 Schritte (22.08.2026) - bei nur einem Punkt je Schritt schlaegt das
/// ADC-Rauschen voll durch. Die 42 s je Referenzfahrt sind der Preis; sie
/// laeuft beim Start und einmal taeglich, nicht im Betrieb.
constexpr uint32_t FEIN_TEMPO = 25;
constexpr uint32_t FEIN_MESSTAKT_MS = 20;

/// Hoechstzahl der Messpunkte einer Feinanfahrt.
///
/// WARUM GESAMMELT UND NICHT LAUFEND GERECHNET: die Rauschschwelle des
/// Schwerpunkts soll ein ANTEIL DER GIPFELHOEHE sein (geraet.h,
/// HALL_SCHWELLE_ANTEIL = 20 %), nicht ein fester Spannungswert. Waehrend der
/// Fahrt kennt man den Gipfel aber noch nicht.
///
/// Vorher stand hier eine feste Schwelle von 0,05 V, wie im Pruefstand. Bei
/// den gemessenen 255 mV Aussteuerung ist das ein Fuenftel des Signals - also
/// zufaellig etwa richtig. Bei einem staerkeren Magneten waere sie zu klein
/// und der Kurvenrand zoege den Schwerpunkt mit, bei einem schwaecheren zu
/// gross. Ein Anteil passt sich an.
///
/// Gesammelt wird deshalb, und danach rechnet `starwatch::schwerpunkt()` -
/// dieselbe Funktion, die vier Host-Tests abdecken, darunter der falsch herum
/// eingeklebte Magnet und eine schiefe Flanke.
///
/// 1300 Schritte Weg mal zwei Punkte je Schritt plus Reserve. 3000 Punkte
/// kosten 24 KB, statisch belegt - der ESP32 hat 320 KB, und der Antrieb
/// braucht davon nichts.
///
/// 23.08.2026 von 2400 heraufgesetzt, weil Rueckzug und FEIN_NACH_FLANKE auf
/// je 650 gewachsen sind. DIESER PUFFER MUSS MITWACHSEN: laeuft er voll,
/// verwirft die Feinanfahrt oben bei FEIN_MAX_PUNKTE stillschweigend den REST
/// des Wegs - also die abfallende Flanke - und der Schwerpunkt wandert nach
/// vorn. Genau der Fehler, den die Rechnung vermeiden soll.
constexpr size_t FEIN_MAX_PUNKTE = 3000;
int32_t fein_pos[FEIN_MAX_PUNKTE];
float fein_wert[FEIN_MAX_PUNKTE];

/// Mindestweg der Feinanfahrt, damit das Ergebnis gilt. Weniger heisst: der
/// Zaehler hat sich nicht bewegt, und dann ist der Schwerpunkt wertlos.
constexpr int32_t FEIN_MINDESTWEG = 300;

/// Halbbreite des Fensters, ueber das der Schwerpunkt gerechnet wird.
///
/// AM AUFBAU GEMESSEN, 22.08.2026: ueber ALLE Punkte gerechnet streute die
/// physische Zeigerlage um rund 23 Schritte, weil die Fahrt links und rechts
/// vom Gipfel unterschiedlich viel erfasste - und das haengt daran, wo die
/// Grobsuche stehenblieb. Ein FESTES, symmetrisches Fenster um den Gipfel
/// macht das Ergebnis davon unabhaengig.
///
/// 150 ist knapp unter der halben gemessenen Fensterbreite (398/2 = 199) -
/// so bleibt beidseitig Rand, den die Fahrt sicher abdeckt.
constexpr int32_t SCHWERPUNKT_HALBBREITE = 150;

/// Messtakt und Mittelung der Spitzenwerterfassung.
///
/// AM GERAET GEMESSEN (22.08.2026): ungetaktet und ohne Mittelung zeigte die
/// Spanne im STILLSTAND 311 bis 383 mV - das Fenster fuer die Aussteuerung
/// ist 250 bis 550 mV, die Messung waere also unbrauchbar gewesen. Einzelne
/// Messungen schwanken nur um rund 36 mV; das Problem war, dass tick() bei
/// JEDEM Schleifendurchlauf misst und dabei jeden Ausreisser des ESP32-ADC
/// einsammelt.
///
/// 20 ms ist derselbe Takt, mit dem die Feinanfahrt messt, und der Pruefstand
/// liest den ADC alle 50 ms.
/// NACHGEMESSEN am 22.08.2026, zweiter Durchgang: mit 20 ms Takt und 4 Proben
/// blieben immer noch 167 bis 180 mV Rauschspanne im Stillstand. Das Fenster
/// fuer die Aussteuerung ist 250 bis 550 mV - eine echte Auslenkung von 250 mV
/// waere davon kaum zu unterscheiden gewesen.
///
/// Der Grund, dass Mittelung allein nicht reicht: min/max sind EXTREMWERTE.
/// Ein einzelner Ausreisser bleibt fuer immer darin stehen, egal wie gut der
/// Rest gemittelt ist. Dagegen hilft nur, den Ausreisser selbst zu entfernen -
/// deshalb der Median aus drei Mittelwerten.
constexpr uint32_t TICK_TAKT_MS = 50;   // wie der ADC-Takt des Pruefstands
constexpr uint8_t TICK_PROBEN = 16;     // Mittelung: Rauschen / sqrt(16) = /4
constexpr uint8_t TICK_MEDIAN = 3;      // gegen einzelne Ausreisser

/// Entprellung von D0.
///
/// AM GERAET GEMESSEN (22.08.2026): im Stillstand zaehlte die Firmware
/// **16071 Flanken**. Der LM393-Komparator der KY-024 hat keine Hysterese;
/// liegt das Feld nahe der Poti-Schwelle, kippt sein Ausgang dauernd.
///
/// Der Pruefstand hat dafuer `delayed_off: 10ms` - das fehlte hier, und es
/// waere teuer geworden: die Grobsuche der Referenzfahrt endet bei der ERSTEN
/// Flanke. Sie haette also irgendeine Zufallsflanke genommen und den Rueckzug
/// von einer Stelle aus gerechnet, an der kein Magnet ist.
///
/// 10 ms sind bei 800 Schritten/s rund 8 Schritte - fuer die Grobsuche
/// unerheblich, weil die Genauigkeit ohnehin aus dem Schwerpunkt kommt.
constexpr uint32_t D0_ENTPRELL_MS = 10;

constexpr uint32_t GROB_TIMEOUT_MS = 120000;
constexpr uint32_t RUECKZUG_TIMEOUT_MS = 30000;
constexpr uint32_t FEIN_TIMEOUT_MS = 90000;  // Weg ist gewachsen, siehe oben
}  // namespace

const char *hominggrund_text(Hominggrund grund) {
  switch (grund) {
    case Hominggrund::nicht_gelaufen: return "noch nicht gelaufen";
    case Hominggrund::ok:            return "OK";
    case Hominggrund::kein_magnet:   return "ABBRUCH: kein Magnet gefunden";
    case Hominggrund::zaehler_steht: return "ABBRUCH: Zaehler bewegt sich nicht";
    case Hominggrund::kein_feld:     return "ABBRUCH: kein Feld gesehen";
    case Hominggrund::abgebrochen:   return "ABBRUCH: von aussen gestoppt";
  }
  return "unbekannt";
}

// ===========================================================================
// Messen
// ===========================================================================

void Hall::beginn() {
  pinMode(g::PIN_HALL_D0, INPUT);  // GPIO34 hat keinen internen Pull-up,
                                   // der sitzt extern
  analogReadResolution(12);
  // 12 dB deckt bis rund 3,1 V ab; die Ruhelinie bei 1,65 V liegt bequem in
  // der Mitte.
  analogSetPinAttenuation(g::PIN_HALL_A0, ADC_11db);

  ruhe_v_ = g::HALL_RUHE_V;
  spitzen_ruecksetzen();
  letzter_zustand_ = magnet_erkannt();
}

bool Hall::magnet_erkannt() const {
  const int roh = digitalRead(g::PIN_HALL_D0);
  return g::D0_AKTIV_HIGH ? (roh == HIGH) : (roh == LOW);
}

float Hall::spannung() const {
  return analogReadMilliVolts(g::PIN_HALL_A0) / 1000.0f;
}

float Hall::spannung_gemittelt(uint8_t proben) const {
  if (proben == 0)
    return spannung();
  uint32_t summe = 0;
  for (uint8_t i = 0; i < proben; i++)
    summe += analogReadMilliVolts(g::PIN_HALL_A0);
  return (summe / static_cast<float>(proben)) / 1000.0f;
}

float Hall::spannung_robust() const {
  // Median aus drei Mittelwerten. Drei genuegen: der Median von drei Werten
  // verwirft den einen Ausreisser, und mehr kostet nur Zeit in der
  // Hauptschleife (16 Proben sind schon rund 1,6 ms je Mittelwert).
  float p[TICK_MEDIAN];
  for (uint8_t i = 0; i < TICK_MEDIAN; i++)
    p[i] = spannung_gemittelt(TICK_PROBEN);

  // Sortiernetz fuer drei Werte - kuerzer als jede Schleife.
  if (p[0] > p[1]) std::swap(p[0], p[1]);
  if (p[1] > p[2]) std::swap(p[1], p[2]);
  if (p[0] > p[1]) std::swap(p[0], p[1]);
  return p[1];
}

void Hall::spitzen_ruecksetzen() {
  max_v_ = 0.0f;
  min_v_ = 3.3f;
}

void Hall::zaehler_ruecksetzen() {
  // GETRENNT VON spitzen_ruecksetzen(), und das ist der Punkt: jenes ruft die
  // REFERENZFAHRT selbst, unter anderem mitten drin vor der Feinanfahrt. Wer
  // dort die Fensterbreite mitloescht, veraendert die Messung. Diese Funktion
  // ist ausschliesslich fuer das Kommando 'c' da.
  //
  // Warum es sie ueberhaupt gibt: bis 24.08.2026 blieben Flanken und
  // Fensterbreite nach 'c' stehen, waehrend die Spitzenwerte bei null
  // anfingen. Beim Messen der Grenzsuche stand deshalb "Flanken 1,
  // Fenster 400" da, obwohl gerade zurueckgesetzt worden war.
  flanken_ = 0;
  fenster_schritte_ = 0;
}

bool Hall::feld_erkannt() const {
  // Die Groesse, die die Grobsuche braucht: liegt ein Feld an, unabhaengig
  // von jedem Komparator und jedem Poti. Bezug ist die zuletzt gemessene
  // Ruhelinie - beim ersten Lauf der Startwert aus geraet.h, danach der
  // gemessene Wert.
  //
  // FRISCH GEMESSEN, nicht der gefilterte Wert aus tick(). Der wird nur alle
  // 50 ms erneuert, und bei 800 Schritten/s in der Grobsuche sind das 40
  // Schritte Unsicherheit - gemessen am 22.08.2026 als 52 Schritte Streuung
  // im Nachlauf, gegenueber 1 Schritt bei der D0-Flanke. Der Nachlauf geht in
  // den Offset ein; 52 Schritte sind 5,85 Grad und damit 12 Minuten
  // Anzeigefehler.
  //
  // Acht Proben kosten rund 0,8 ms - bei 800 Schritten/s also 0,6 Schritte.
  // Das Rauschen liegt damit weit unter der Schwelle von 87 mV.
  return fabsf(spannung_gemittelt(8) - ruhe_v_) > g::HALL_GROB_SCHWELLE_V;
}

void Hall::tick(int32_t position) {
  // Woraus die Flanke gebildet wird:
  //   - mit Modul: D0, entprellt (der Komparator hat keine Hysterese)
  //   - ohne Modul: die Feldschwelle auf A0
  //
  // Die Fensterbreite bleibt in beiden Faellen eine messbare Groesse; sie
  // bezieht sich dann eben auf die Softwareschwelle statt auf das Poti - und
  // ist damit reproduzierbar, was sie vorher nicht war.
  const bool roh = g::D0_VORHANDEN ? magnet_erkannt() : feld_erkannt();
  const uint32_t t = millis();
  if (roh != roh_letzter_) {
    roh_letzter_ = roh;
    roh_seit_ = t;
  }
  const bool jetzt =
      (t - roh_seit_ >= D0_ENTPRELL_MS) ? roh : letzter_zustand_;

  // Der ANALOGE Wert dagegen getaktet, gemittelt und gegen Ausreisser
  // gesichert, siehe die Konstanten oben.
  const uint32_t nun = millis();
  if (nun - letzte_analogmessung_ >= TICK_TAKT_MS) {
    letzte_analogmessung_ = nun;
    const float v = spannung_robust();
    gefiltert_v_ = v;
    if (v > max_v_)
      max_v_ = v;
    if (v < min_v_)
      min_v_ = v;
  }

  if (jetzt && !letzter_zustand_) {
    // Eintritt in das Sensorfenster. Der Zaehlerstand in diesem Augenblick
    // ist die Groesse, aus der Fensterbreite und Offset entstehen.
    pos_flanke_ = position;
    flanken_++;
    gesehen_ = true;
  } else if (!jetzt && letzter_zustand_) {
    // Austritt. Die Breite ist die Differenz - in SCHRITTEN, nicht in
    // Millisekunden: der Pruefstand misst ms und muss umrechnen, was nur bei
    // bekannter Drehzahl geht. Schritte sind die Groesse, die die
    // Referenzfahrt wirklich braucht.
    const int32_t breite = position - pos_flanke_;
    if (breite > 0)
      fenster_schritte_ = breite;
  }
  letzter_zustand_ = jetzt;
}

// ===========================================================================
// Referenzfahrt
// ===========================================================================

bool Hall::warte_bis_ziel(Antrieb &antrieb, uint32_t timeout_ms, Pumpe pumpe) {
  const uint32_t ab = millis();
  while (!antrieb.ziel_erreicht()) {
    tick(antrieb.position());
    if (pumpe != nullptr && !pumpe()) {
      antrieb.stopp_mit_rampe();
      return false;
    }
    if (millis() - ab > timeout_ms)
      return false;
    delay(2);
  }
  return true;
}

Homingergebnis Hall::referenzfahrt(Antrieb &antrieb, int32_t offset, Pumpe pumpe) {
  Homingergebnis e;
  // Ab hier laeuft eine Fahrt: bricht sie an einer unerwarteten Stelle ab,
  // soll NICHT "noch nicht gelaufen" dastehen.
  e.grund = Hominggrund::abgebrochen;

  gesehen_ = false;
  spitzen_ruecksetzen();

  // ---------------------------------------------------------------------
  // 1. Grobsuche: vorwaerts, bis D0 anspricht
  // ---------------------------------------------------------------------
  antrieb.tempo(800);
  antrieb.fahre_weiter(g::REV + GROB_RESERVE);

  const uint32_t grob_ab = millis();
  while (!gesehen_ && !antrieb.ziel_erreicht()) {
    tick(antrieb.position());
    // Zusaetzlich zur Flanke aus tick(): die Feldschwelle auf A0. Damit endet
    // die Grobsuche auch dann, wenn gar kein D0 verdrahtet ist.
    if (!g::D0_VORHANDEN && feld_erkannt() && !gesehen_) {
      gesehen_ = true;
      pos_flanke_ = antrieb.position();
      flanken_++;
    }
    if (pumpe != nullptr && !pumpe()) {
      antrieb.stopp_mit_rampe();
      e.grund = Hominggrund::abgebrochen;
      return e;
    }
    if (millis() - grob_ab > GROB_TIMEOUT_MS)
      break;
    delay(2);
  }

  // Die Suchfahrt beenden, sobald der Magnet da ist - sonst laeuft sie ihre
  // ganze Umdrehung aus, obwohl das Ziel gefunden ist.
  antrieb.stopp_mit_rampe();
  while (antrieb.laeuft()) {
    tick(antrieb.position());
    delay(2);
  }

  e.pos_flanke = pos_flanke_;

  if (!gesehen_) {
    e.grund = Hominggrund::kein_magnet;
    return e;
  }

  // ---------------------------------------------------------------------
  // 2. Rueckzug
  //
  // Er ist die EINZIGE Rueckwaertsbewegung des ganzen Geraets. Die Referenz
  // entsteht dadurch wieder auf einer Vorwaertsfahrt und ist damit spielfrei -
  // wie jede spaetere Zielanfahrt. Das Riemenspiel liegt so immer auf
  // derselben Seite.
  // ---------------------------------------------------------------------
  antrieb.fahre_zu(pos_flanke_ - g::HALL_RUECKZUG_SCHRITTE);
  if (!warte_bis_ziel(antrieb, RUECKZUG_TIMEOUT_MS, pumpe)) {
    e.grund = Hominggrund::abgebrochen;
    return e;
  }

  // ---------------------------------------------------------------------
  // 3. Ruhelinie messen - ERST JETZT, NACH dem Rueckzug
  //
  // Vorher steht der Magnet womoeglich noch im Feld, und dann ist die
  // Nulllinie falsch - und mit ihr der ganze Schwerpunkt und der Offset, um
  // den es hier geht.
  // ---------------------------------------------------------------------
  ruhe_v_ = spannung_robust();
  e.ruhe_v = ruhe_v_;

  // ---------------------------------------------------------------------
  // 4. Feinanfahrt: EINE durchgehende langsame Fahrt, dabei messen
  // ---------------------------------------------------------------------
  size_t punkte = 0;
  spitzen_ruecksetzen();

  const int32_t fein_start = antrieb.position();
  antrieb.tempo(FEIN_TEMPO);
  antrieb.fahre_zu(fein_start + FEIN_WEG);

  const uint32_t fein_ab = millis();
  uint32_t letzte_messung = 0;
  while (!antrieb.ziel_erreicht()) {
    const uint32_t jetzt = millis();
    if (jetzt - letzte_messung >= FEIN_MESSTAKT_MS) {
      letzte_messung = jetzt;
      // Gemittelt: der Schwerpunkt gewichtet mit der Auslenkung, und ein
      // Ausreisser zieht ihn an eine Stelle, an der kein Magnet ist.
      const float v = spannung_gemittelt(TICK_PROBEN);
      if (punkte < FEIN_MAX_PUNKTE) {
        fein_pos[punkte] = antrieb.position();
        fein_wert[punkte] = v;
        punkte++;
      }
      if (v > max_v_)
        max_v_ = v;
      if (v < min_v_)
        min_v_ = v;
    }
    tick(antrieb.position());
    if (pumpe != nullptr && !pumpe()) {
      antrieb.stopp_mit_rampe();
      antrieb.tempo(800);
      e.grund = Hominggrund::abgebrochen;
      return e;
    }
    if (jetzt - fein_ab > FEIN_TIMEOUT_MS)
      break;
    delay(1);
  }

  antrieb.tempo(800);

  const int32_t gefahren = antrieb.position() - fein_start;
  e.fenster = fenster_schritte_;

  // Die Flankenlage aus der LANGSAMEN Fahrt, nicht aus der Grobsuche: dort
  // wird nur alle 2 ms geprueft, bei 800 Schritten/s also alle 1,6 Schritte.
  // Hier ist sie auf einen Schritt genau, und nur so ist der gemeldete
  // Versatz (Schwerpunkt minus Flanke) eine brauchbare Zahl.
  e.pos_flanke = pos_flanke_;

  // ---------------------------------------------------------------------
  // 5. Plausibilitaetspruefung
  //
  // Am 22.08.2026 um 01:54 meldete die Referenzfahrt des Pruefstands "OK",
  // obwohl Schwerpunkt und Flanke beide auf 0 standen: der Positionszaehler
  // hatte sich waehrend der ganzen Fahrt nicht bewegt. Ein Ergebnis aus
  // lauter Nullen ist kein Ergebnis. Eine Referenzfahrt, die bei stehendem
  // Zaehler "OK" meldet, ist schlimmer als eine, die abbricht - die Uhr geht
  // dann falsch und behauptet, sie sei kalibriert.
  // ---------------------------------------------------------------------
  if (gefahren < FEIN_MINDESTWEG) {
    e.grund = Hominggrund::zaehler_steht;
    return e;
  }

  // Gipfelhoehe suchen, daraus die Schwelle als ANTEIL - erst jetzt ist sie
  // bekannt. Danach rechnet die host-getestete Funktion.
  float gipfel = 0.0f;
  for (size_t i = 0; i < punkte; i++) {
    const float ab = fabsf(fein_wert[i] - ruhe_v_);
    if (ab > gipfel)
      gipfel = ab;
  }
  e.spanne_v = gipfel;  // die ECHTE Aussteuerung gegen die Ruhelinie

  float mitte = 0.0f;
  const float schwelle = gipfel * g::HALL_SCHWELLE_ANTEIL;
  // SYMMETRISCH um den Gipfel, nicht ueber alle Punkte - siehe
  // SCHWERPUNKT_HALBBREITE. Die Funktion lehnt ausserdem ab, wenn das Fenster
  // einseitig nicht abgedeckt ist; dann ist das Ergebnis nicht
  // vertrauenswuerdig, und ein nicht vertrauenswuerdiger Offset ist schlimmer
  // als keiner.
  if (punkte == 0 ||
      !schwerpunkt_symmetrisch(fein_pos, fein_wert, punkte, ruhe_v_, schwelle,
                               SCHWERPUNKT_HALBBREITE, &mitte)) {
    e.grund = Hominggrund::kein_feld;
    return e;
  }
  e.punkte = punkte;

  e.schwerpunkt = mitte;

  // Der Schwerpunkt liegt im ueberstrichenen Bogen, der Zeiger steht aber am
  // ENDE des Wegs. Die Differenz ist der Nachlauf und wird herausgerechnet -
  // damit spielt es keine Rolle, wie weit nach dem Gipfel noch gefahren wurde.
  e.nachlauf = antrieb.position() - static_cast<int32_t>(lroundf(mitte));
  e.zaehler = normiere(offset + e.nachlauf, g::REV);

  antrieb.zaehler_setzen(e.zaehler);
  e.grund = Hominggrund::ok;
  return e;
}

}  // namespace starwatch
