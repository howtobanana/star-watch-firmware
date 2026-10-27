#include "hall.h"

#include <Arduino.h>
#include <geraet.h>
#include <zeiger.h>

#include <algorithm>  // std::max
#include <utility>    // std::swap

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
///
/// 27.09.2026: KEINE FESTE STRECKE MEHR. Die Kurve des TMAG5273 an C4E8 ist
/// 842 Schritte breit, die Fahrt endete 227 Schritte hinter der Mitte - fuer
/// die 150 Schritte je Seite der Rechnung blieben 77 Reserve. Die Fahrt endet
/// jetzt an der Kurve (zeiger.h, Feinfahrtende); dieser Wert ist nur noch die
/// Obergrenze, falls sie nie wieder unter die Schwelle faellt.
constexpr int32_t FEIN_NACH_FLANKE = 1400;

/// Wo die Feinfahrt BEGINNT: so weit vor der Flanke der Grobsuche. Bis dahin
/// faehrt der Zeiger im Grobtempo (27.09.2026). Frueher begann sie am Ende des
/// Rueckzugs, 650 Schritte vor der Flanke - bei 25 Schritten/s 26 s Anlauf
/// durch freies Feld. Die Flanke liegt bei rund 5 % der Gipfelhoehe, die
/// Rechnung beginnt bei 20 %; 250 Schritte davor ist Luft.
constexpr int32_t FEIN_VORLAUF = 250;
constexpr int32_t FEIN_WEG = FEIN_VORLAUF + FEIN_NACH_FLANKE;

/// Mindestabstand des Halts hinter dem Gipfel - SCHWERPUNKT_HALBBREITE plus
/// Reserve, damit die Rechnung beim SCHMALEN Magneten beide Seiten sieht.
constexpr int32_t FEIN_NACH_GIPFEL = 200;

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

/// Verzoegerung zwischen Feldwert und Zaehlerstand, in Mikrosekunden.
///
/// AM GERAET GEMESSEN, 27.09.2026 (C4E8, TMAG5273): im Wechsel 50 <-> 400
/// Schritte/s lag der Schwerpunkt bei 400 im Mittel 3,8 Schritte spaeter,
/// in der Reihe 25/100/200/400/800 ergab sich dieselbe Groessenordnung
/// (9..16 ms). 3,8 Schritte auf 350 Schritte/s Unterschied sind 11 ms.
/// Woher genau - Mittelung im Sensor, Positionsmeldung des Antriebs, Last-
/// winkel des Motors - ist offen; die Wirkung ist ein Versatz proportional
/// zum Tempo, und der wird hier abgezogen. Damit liefert jede Feinfahrt-
/// geschwindigkeit dieselbe Mitte, und ein bei 25 Schritten/s eingemessener
/// Offset bleibt gueltig (Rest 0,25 Schritte).
constexpr uint32_t FEIN_LATENZ_US = 11000;

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
///
/// 27.09.2026: 3400, damit die Obergrenze des Wegs (FEIN_WEG = 1650) bei zwei
/// Punkten je Schritt hineinpasst - der Messtakt richtet sich jetzt nach dem
/// Tempo (siehe Feinanfahrt). Im Normalfall faellt rund ein Drittel weniger an.
constexpr size_t FEIN_MAX_PUNKTE = 3400;
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
/// WAS JETZT GILT, STEHT AM ENDE DIESES BLOCKS. Der Absatz davor ist die
/// Vorgeschichte aus der Zeit der KY-024 und bleibt stehen, weil er den
/// Grund fuer den Takt erklaert - der Takt gilt weiter, nur aus einem
/// anderen Grund.
///
/// HISTORIE, KY-024 MIT SS49E, AM GERAET GEMESSEN (22.08.2026): ungetaktet und ohne Mittelung zeigte die
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
/// SEIT DEM TMAG5273 SIND DIE DREI ZAHLEN KLEIN GEWORDEN, und der Grund
/// steht im Absatz darueber: das ganze Aufgebot aus Takt, 16 Proben und
/// Median richtete sich gegen den ESP32-ADC. Der ist nicht mehr beteiligt.
/// Der TMAG5273 mittelt in Hardware ueber 32 Wandlungen und schickt das
/// Ergebnis als Zahl - Ausreisser einzelner Wandlungen kommen dort gar nicht
/// mehr an.
///
/// Was BLEIBT, ist der Takt: tick() laeuft in jedem Schleifendurchlauf, und
/// eine I2C-Uebertragung je Durchlauf waere reine Verschwendung. Bei 100 kHz
/// kostet ein Lesevorgang ueber sechs Register rund 0,9 ms.
constexpr uint32_t TICK_TAKT_MS = 50;
constexpr uint8_t TICK_PROBEN = 4;   // gegen das Rauschen des Sensors selbst
constexpr uint8_t TICK_MEDIAN = 1;   // Ausreisser gibt es nicht mehr

/// Entprellung des Schwellenuebergangs.
///
/// WARUM SIE BLEIBT, OBWOHL DER KOMPARATOR WEG IST: am 22.08.2026 zaehlte
/// die Firmware im Stillstand 16071 Flanken, weil der LM393 der KY-024 keine
/// Hysterese hat. Dieses Bauteil gibt es nicht mehr - die Schwelle steht
/// jetzt in Software und der Wert kommt als Zahl ueber den Bus.
///
/// Der GRUND fuer die Entprellung ist aber ein anderer und gilt weiter: eine
/// Softwareschwelle ohne Hysterese kippt genauso, wenn das Feld dicht daneben
/// steht. Und die Grobsuche der Referenzfahrt endet bei der ERSTEN Flanke -
/// sie naehme also eine Zufallsflanke und rechnete den Rueckzug von einer
/// Stelle aus, an der kein Magnet ist.
///
/// 10 ms sind bei 800 Schritten/s rund 8 Schritte - fuer die Grobsuche
/// unerheblich, weil die Genauigkeit ohnehin aus dem Schwerpunkt kommt.
constexpr uint32_t SCHWELLE_ENTPRELL_MS = 10;

constexpr uint32_t GROB_TIMEOUT_MS = 120000;
constexpr uint32_t RUECKZUG_TIMEOUT_MS = 30000;
constexpr uint32_t FEIN_TIMEOUT_MS = 90000;  // Weg ist gewachsen, siehe oben
}  // namespace

const char *hominggrund_text(Hominggrund grund) {
  switch (grund) {
    case Hominggrund::nicht_gelaufen: return "noch nicht gelaufen";
    case Hominggrund::ok:            return "OK";
    case Hominggrund::kein_sensor:   return "ABBRUCH: TMAG5273 antwortet nicht";
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

bool Hall::beginn() {
  // Der INT-Pin wird NICHT ausgewertet (siehe geraet.h). Er bekommt trotzdem
  // eine Richtung: ein Eingang, der nie gesetzt wird, bleibt sonst im
  // Zustand, den der Bootvorgang hinterlassen hat.
  pinMode(g::PIN_HALL_INT, INPUT);  // der Pull-up sitzt auf der Platine:
                                    // B R3 an GPIO34, F R13 an IO7

  ruhe_mt_ = g::HALL_RUHE_MT;
  spitzen_ruecksetzen();

  if (!sensor_.beginn()) {
    letzter_zustand_ = false;
    return false;
  }

  gefiltert_mt_ = feld_gemittelt(TICK_PROBEN);
  letzter_zustand_ = magnet_erkannt();
  return true;
}

bool Hall::magnet_erkannt() const {
  // OHNE NEUE MESSUNG. Frueher las diese Funktion den Komparatorausgang -
  // das war ein digitalRead und kostete nichts. Eine I2C-Uebertragung kostet
  // etwas, und die Weboberflaeche fragt im Sekundentakt.
  //
  // Wer den frischen Zustand braucht, nimmt feld_erkannt().
  return fabsf(gefiltert_mt_ - ruhe_mt_) > g::HALL_GROB_SCHWELLE_MT;
}

float Hall::feld_mt() {
  return sensor_.betrag_mt();
}

float Hall::feld_gemittelt(uint8_t proben) {
  if (proben <= 1)
    return feld_mt();
  float summe = 0.0f;
  for (uint8_t i = 0; i < proben; i++)
    summe += feld_mt();
  return summe / static_cast<float>(proben);
}

float Hall::feld_robust() {
  // Der Median aus drei Mittelwerten ist mit dem ESP32-ADC entfallen. Was
  // hier steht, ist ein Mittelwert - und die Funktion behaelt ihren Namen,
  // weil sechs Aufrufstellen ihn benutzen und "robust" weiterhin stimmt:
  // sie ist der Wert, den man ANZEIGEN und fuer Spitzenwerte nehmen soll.
  if (TICK_MEDIAN <= 1)
    return feld_gemittelt(TICK_PROBEN);

  float p[3];
  for (uint8_t i = 0; i < 3; i++)
    p[i] = feld_gemittelt(TICK_PROBEN);

  // Sortiernetz fuer drei Werte - kuerzer als jede Schleife.
  if (p[0] > p[1]) std::swap(p[0], p[1]);
  if (p[1] > p[2]) std::swap(p[1], p[2]);
  if (p[0] > p[1]) std::swap(p[0], p[1]);
  return p[1];
}

void Hall::spitzen_ruecksetzen() {
  max_mt_ = 0.0f;
  // Die Obergrenze ist der Messbereich des Sensors, nicht eine getippte
  // Zahl: darueber kann kein Messwert liegen, also ist sie der richtige
  // Startwert fuer eine Minimumsuche.
  min_mt_ = g::TMAG_BEREICH_MT;
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
  fenster_aus_fein_ = false;
}

bool Hall::feld_erkannt() {
  // Die Groesse, die die Grobsuche braucht: liegt ein Feld an?
  //
  // FRISCH GEMESSEN, nicht der gefilterte Wert aus tick(). Der wird nur alle
  // 50 ms erneuert, und bei 800 Schritten/s in der Grobsuche sind das 40
  // Schritte Unsicherheit - gemessen am 22.08.2026 als 52 Schritte Streuung
  // im Nachlauf, gegenueber 1 Schritt bei der D0-Flanke. Der Nachlauf geht in
  // den Offset ein; 52 Schritte sind 5,85 Grad und damit 12 Minuten
  // Anzeigefehler.
  //
  // EINE Messung, nicht vier. Der SS49E brauchte acht Proben gegen das
  // ADC-Rauschen und kam damit auf 0,8 ms; vier I2C-Zugriffe waeren 3,6 ms
  // und bei 800 Schritten/s knapp drei Schritte Weg WAEHREND der Messung -
  // also genau der Fehler, gegen den der ganze Absatz oben argumentiert.
  //
  // Eine einzelne Uebertragung kostet 0,9 ms und liefert trotzdem einen
  // Mittelwert: der TMAG5273 hat ueber 32 Wandlungen gemittelt, bevor der
  // Wert auf den Bus kam.
  return fabsf(feld_mt() - ruhe_mt_) > g::HALL_GROB_SCHWELLE_MT;
}

void Hall::tick(int32_t position) {
  // EINE Messung je Takt, und aus ihr kommt beides: der gefilterte Wert fuer
  // die Anzeigen UND die Flanke.
  //
  // Frueher waren das zwei Quellen - D0 in jedem Schleifendurchlauf, A0 alle
  // 50 ms. Das ging, weil beide nichts kosteten. Ueber I2C kostet jede
  // Messung eine Uebertragung, und zwei Quellen waeren zwei davon, die
  // dasselbe Feld messen.
  const uint32_t nun = millis();
  if (nun - letzte_messung_ >= TICK_TAKT_MS) {
    letzte_messung_ = nun;
    const float v = feld_robust();
    gefiltert_mt_ = v;
    if (v > max_mt_)
      max_mt_ = v;
    if (v < min_mt_)
      min_mt_ = v;
  }

  // Die Flanke aus dem gerade gefilterten Wert. magnet_erkannt() misst
  // bewusst nicht selbst - siehe dort.
  const bool roh = magnet_erkannt();
  const uint32_t t = nun;
  if (roh != roh_letzter_) {
    roh_letzter_ = roh;
    roh_seit_ = t;
  }
  const bool jetzt =
      (t - roh_seit_ >= SCHWELLE_ENTPRELL_MS) ? roh : letzter_zustand_;

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
    // Nicht ueber die Breite aus der Feinfahrt schreiben - die ist auf den
    // Schritt genau, diese haengt am 50-ms-Takt (C4E8: 842, dann 1 Schritt).
    const int32_t breite = position - pos_flanke_;
    if (breite > 0 && !fenster_aus_fein_)
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

void Hall::tempi(uint32_t fein, uint32_t grob) {
  // Grenzen gegen den Vertipper, nicht gegen die Physik: unter 10 dauert
  // die Feinfahrt ueber zwei Minuten, ueber 800 bleiben bei 3 ms Messtakt
  // weniger als ein halber Messpunkt je Schritt. Die Grobsuche nicht ueber
  // das Fahrtempo.
  if (fein >= 10 && fein <= 800)
    fein_tempo_ = fein;
  if (grob >= 200 && grob <= g::TEMPO_FAHRT)
    grob_tempo_ = grob;
}

Homingergebnis Hall::referenzfahrt(Antrieb &antrieb, int32_t offset, Pumpe pumpe) {
  Homingergebnis e;
  // Ab hier laeuft eine Fahrt: bricht sie an einer unerwarteten Stelle ab,
  // soll NICHT "noch nicht gelaufen" dastehen.
  e.grund = Hominggrund::abgebrochen;

  // OHNE SENSOR GAR NICHT ERST LOSFAHREN. Frueher war das nicht noetig: ein
  // fehlender KY-024 lieferte einen offenen Eingang, und die Fahrt endete
  // mit "kein Magnet". Ein fehlender TMAG5273 liefert 0,0 mT - und eine
  // Feldkurve aus lauter Nullen sieht genauso aus wie ein zu schwacher
  // Magnet. Eine Umdrehung Suchfahrt spart man sich damit.
  if (!sensor_.bereit()) {
    e.grund = Hominggrund::kein_sensor;
    return e;
  }

  // ERST STILLSTAND (23.10.2026). Die App schickt 'h' direkt nach dem
  // Steuern, und dann lief der Zeiger noch mit 2 U/s rueckwaerts: die
  // Grobsuche sah die Schwelle beim Zurueckrollen (falsche Flanke, Ruhelinie
  // im Feld - BE40: Ruhe 4,95 mT, Breite 1650, Abw +1087, trotzdem "OK")
  // oder brach mit "Zaehler bewegt sich nicht" ab. Danach ist ziel() die
  // Istposition, und die Suche geht eine volle Umdrehung ab hier.
  antrieb.stopp_mit_rampe();
  while (antrieb.laeuft()) {
    if (pumpe != nullptr && !pumpe()) {
      e.grund = Hominggrund::abgebrochen;
      return e;
    }
    delay(2);
  }

  gesehen_ = false;
  spitzen_ruecksetzen();

  // ---------------------------------------------------------------------
  // 1. Grobsuche: vorwaerts, bis die Feldschwelle erreicht ist
  // ---------------------------------------------------------------------
  antrieb.tempo(grob_tempo_);  // siehe tempi()
  antrieb.fahre_weiter(g::REV + GROB_RESERVE);

  // DIE SCHWELLE GILT GEGEN DAS KLEINSTE BISHER GESEHENE FELD, nicht gegen
  // eine Ruhelinie (27.09.2026). Die ist nach dem Einschalten noch nicht
  // gemessen (HALL_RUHE_MT = 0), und C4E8 sieht im freien Feld 0,54 mT -
  // die halbe Schwelle. Ein Motor, der naeher am Sensor sitzt, und die
  // Grobsuche stuende sofort still, mitten im freien Feld.
  //
  // Das laufende Minimum ist das Grundfeld, sobald der Zeiger einmal freies
  // Feld gesehen hat. Steht er beim Start im Magnetfeld, sinkt das Minimum
  // beim Herausfahren mit, und die Flanke kommt beim naechsten Vorbeifahren -
  // dafuer ist der Weg eine Umdrehung plus GROB_RESERVE lang.
  //
  // Und NICHT ueber gesehen_ aus tick(): das haengt an der absoluten Schwelle
  // und am 50-ms-Takt.
  // Die Flanke der Grobsuche LOKAL: tick() schreibt pos_flanke_ bei jedem
  // Eintritt in sein (absolutes, 50-ms-)Fenster neu - auch waehrend des
  // Bremsens, und dann rechneten Rueckzug und Vorlauf von dort.
  bool gefunden = false;
  int32_t flanke = 0;
  float grund = g::TMAG_BEREICH_MT;
  const uint32_t grob_ab = millis();
  while (!gefunden && !antrieb.ziel_erreicht()) {
    tick(antrieb.position());
    const float v = feld_mt();  // FRISCH, siehe feld_erkannt()
    if (v < grund)
      grund = v;
    if (v - grund > g::HALL_GROB_SCHWELLE_MT) {
      gefunden = true;
      gesehen_ = true;
      flanke = antrieb.position();
      pos_flanke_ = flanke;
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

  if (!gefunden) {
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
  antrieb.fahre_zu(flanke - g::HALL_RUECKZUG_SCHRITTE);
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
  ruhe_mt_ = feld_robust();
  e.ruhe_mt = ruhe_mt_;

  // ---------------------------------------------------------------------
  // 4. Feinanfahrt: EINE durchgehende langsame Fahrt, dabei messen
  // ---------------------------------------------------------------------
  size_t punkte = 0;
  spitzen_ruecksetzen();

  // Im Grobtempo bis kurz vor die Flanke - VORWAERTS, damit das Riemenspiel
  // auf derselben Seite liegt wie bei jeder Zielanfahrt.
  antrieb.fahre_zu(flanke - FEIN_VORLAUF);
  if (!warte_bis_ziel(antrieb, RUECKZUG_TIMEOUT_MS, pumpe)) {
    e.grund = Hominggrund::abgebrochen;
    return e;
  }

  const int32_t fein_start = antrieb.position();
  antrieb.tempo(fein_tempo_);
  antrieb.fahre_zu(fein_start + FEIN_WEG);
  Feinfahrtende ende(ruhe_mt_, g::HALL_SCHWELLE_ANTEIL, g::HALL_GROB_SCHWELLE_MT,
                     FEIN_NACH_GIPFEL);
  bool haelt = false;

  // BEIM ALTEN TEMPO BLEIBT ALLES WIE ES WAR - vier Lesungen alle 20 ms, damit
  // eine Messung mit 25 Schritten/s mit den frueheren vergleichbar ist.
  // SCHNELLER: eine Lesung alle 3 ms. Mehr bringt nichts: der TMAG5273
  // liefert bei 32-facher Mittelung rund alle 2,5 ms einen neuen Wert (geraet.h,
  // TMAG_CONV_AVG), vier Lesungen hintereinander sind viermal DERSELBE.
  // Darueber: ein Takt, der rund ZWEI Punkte je Schritt gibt, nicht unter
  // 3 ms - so passt der laengste Weg in den Puffer.
  const bool alt = fein_tempo_ <= FEIN_TEMPO;
  const uint32_t messtakt =
      alt ? FEIN_MESSTAKT_MS : std::max<uint32_t>(3, 500 / fein_tempo_);
  const uint8_t proben = alt ? TICK_PROBEN : 1;

  const uint32_t fein_ab = millis();
  uint32_t letzte_messung = 0;
  while (haelt ? antrieb.laeuft() : !antrieb.ziel_erreicht()) {
    const uint32_t jetzt = millis();
    if (jetzt - letzte_messung >= messtakt) {
      letzte_messung = jetzt;
      // Gemittelt: der Schwerpunkt gewichtet mit der Auslenkung, und ein
      // Ausreisser zieht ihn an eine Stelle, an der kein Magnet ist.
      // Die Position VOR und NACH der Uebertragung gemittelt: der Wert
      // gehoert in die Mitte, nicht ans Ende. Bei 25 Schritten/s gleichgueltig,
      // bei 400 ist eine Uebertragung mit vier Proben fast ein Schritt.
      const int32_t p0 = antrieb.position();
      const float v = feld_gemittelt(proben);
      const int32_t p = (p0 + antrieb.position()) / 2 -
                        static_cast<int32_t>(((uint64_t)fein_tempo_ * FEIN_LATENZ_US + 500000u) / 1000000u);
      if (punkte < FEIN_MAX_PUNKTE) {
        fein_pos[punkte] = p;
        fein_wert[punkte] = v;
        punkte++;
      }
      if (!haelt && ende.fertig(p, v)) {
        // Die Kurve ist durch. Mit Rampe anhalten und weitermessen, bis er
        // steht - die Punkte schaden nicht, und die Schleife endet mit dem Ziel.
        haelt = true;
        antrieb.stopp_mit_rampe();
      }
      if (v > max_mt_)
        max_mt_ = v;
      if (v < min_mt_)
        min_mt_ = v;
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
    const float ab = fabsf(fein_wert[i] - ruhe_mt_);
    if (ab > gipfel)
      gipfel = ab;
  }
  e.spanne_mt = gipfel;  // die ECHTE Aussteuerung gegen die Ruhelinie

  float mitte = 0.0f;
  const float schwelle = gipfel * g::HALL_SCHWELLE_ANTEIL;
  // SYMMETRISCH um den Gipfel, nicht ueber alle Punkte - siehe
  // SCHWERPUNKT_HALBBREITE. Die Funktion lehnt ausserdem ab, wenn das Fenster
  // einseitig nicht abgedeckt ist; dann ist das Ergebnis nicht
  // vertrauenswuerdig, und ein nicht vertrauenswuerdiger Offset ist schlimmer
  // als keiner.
  if (punkte == 0 ||
      !schwerpunkt_symmetrisch(fein_pos, fein_wert, punkte, ruhe_mt_, schwelle,
                               SCHWERPUNKT_HALBBREITE, &mitte)) {
    e.grund = Hominggrund::kein_feld;
    return e;
  }
  e.punkte = punkte;

  e.schwerpunkt = mitte;
  // Die Breite aus der Feinfahrt - auf den Schritt genau, gegen die gemessene
  // Ruhelinie. Sie ersetzt die aus tick(), damit auch die Anzeige sie zeigt.
  e.fenster = feldbreite(fein_pos, fein_wert, punkte, ruhe_mt_, g::HALL_GROB_SCHWELLE_MT);
  fenster_schritte_ = e.fenster;
  fenster_aus_fein_ = true;
  e.fein_von = fein_start - static_cast<int32_t>(lroundf(mitte));
  e.fein_bis = antrieb.position() - static_cast<int32_t>(lroundf(mitte));

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
