// ---------------------------------------------------------------------------
// Star Watch - die Rechenteile der Firmware, ohne Hardware.
//
// Hier steht ausschliesslich Arithmetik: kein Arduino, kein ESP-IDF, keine
// Register, keine Zeitquelle. Der Grund steht im Briefing Abschnitt 8: genau
// diese Teile sind die kniffligen, und genau hier sassen beide Fehler, die in
// der ESPHome-Fassung gefunden wurden. Sie laufen deshalb als Host-Tests auf
// dem Mac (`pio test -e native`) und brauchen dafuer keinen ESP32.
//
// Einheiten und Vorzeichen, einmal festgelegt:
//   - Positionen sind MIKROSCHRITTE, nicht Grad. Nach aussen (API) werden
//     spaeter Grad gereicht, siehe App-und-HA-Architektur.md Abschnitt 4.
//   - 0 ist 12 Uhr. Wachsende Zahlen laufen IM UHRZEIGERSINN.
//   - `rev` ist die Schrittzahl einer vollen Zeigerumdrehung, also 12 Stunden.
//     Bei 1/16 Microstepping und Riemen 1:1 sind das 3200.
//   - `sek_je_schritt` ist 43200 / rev, bei 3200 also 13,5 s.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace starwatch {

/// Faltet eine beliebige Position in den Bereich 0 .. rev-1.
/// Auch fuer negative Werte - der eingebaute %-Operator liefert in C++ bei
/// negativem Zaehler ein negatives Ergebnis, das ist hier nie gewollt.
int32_t normiere(int32_t position, int32_t rev);

/// Sollposition aus der Tageszeit, absolut gerechnet (Architekturregel 8).
/// `sekunden` ist die Sekunde seit Mitternacht; alles ueber 12 h faellt weg,
/// das Blatt hat nur zwoelf Stunden.
///
/// Absolut heisst: das Ergebnis haengt NUR an der Zeit, nie am vorherigen
/// Ergebnis. Nach einem WLAN- oder NTP-Ausfall stellt sich die Uhr damit von
/// selbst wieder richtig, statt einen Fehler mitzuschleppen.
int32_t soll_position(int32_t sekunden, int32_t rev, float sek_je_schritt);

/// Weg von `ist` nach `ziel`, IMMER im Uhrzeigersinn, Ergebnis 0 .. rev-1.
///
/// Nie den kuerzeren Weg nehmen: das Riemenspiel betraegt bis 0,8 mm Losigkeit
/// und damit rund 7 Grad Umkehrspiel (Briefing Abschnitt 6). Solange der
/// Zeiger nur eine Richtung laeuft, ist das Spiel immer auf derselben Seite
/// und faellt gar nicht auf. Ein Richtungswechsel macht daraus einen
/// systematischen Anzeigefehler von rund 14 Minuten.
int32_t delta_vorwaerts(int32_t ist, int32_t ziel, int32_t rev);

/// Steht der Zeiger ein paar Schritte VOR der Zeit - etwa nach einer
/// Driftkorrektur -, waere `delta_vorwaerts` fast eine ganze Umdrehung. Dann
/// ist Stehenbleiben richtig, bis die Zeit aufgeholt hat.
///
/// Gibt true zurueck, wenn gewartet werden soll.
bool warten_statt_umrunden(int32_t delta, int32_t rev, int32_t toleranz);

/// Abweichung zwischen Istposition und Referenzlage, gefaltet auf
/// -rev/2 .. +rev/2. Positiv heisst: der Zeiger ist zu weit gelaufen.
///
/// Die Faltung ist der Punkt. Ohne sie meldet ein Zeiger, der einen Schritt
/// VOR der Referenz steht, eine Abweichung von fast einer ganzen Umdrehung.
int32_t drift(int32_t ist, int32_t referenz, int32_t rev);

/// Schwerpunkt der Hall-Feldkurve - das Verfahren, mit dem die Referenzfahrt
/// ihre Genauigkeit holt.
///
/// Warum nicht die Flanke des Komparators: die haengt am Poti der KY-024, an
/// der Versorgungsspannung und an der Temperatur. Das Maximum der Feldkurve
/// liegt dagegen dort, wo der Magnet dem Sensor gegenuebersteht, und ist von
/// jeder Schwelle unabhaengig.
///
/// Gerechnet wird mit dem BETRAG der Auslenkung gegen `ruhe`: der SS49E ist
/// linear und bipolar, der Magnet darf also falsch herum sitzen. Messpunkte
/// unterhalb von `schwelle` gehen nicht ein (Rauschgrenze), und die Schwelle
/// wird von den Gewichten abgezogen, damit der Rand nicht mitzieht.
///
/// Gibt false zurueck, wenn kein einziger Messpunkt ueber der Schwelle lag -
/// dann hat die Fahrt kein Feld gesehen und es gibt keine Referenz.
bool schwerpunkt(const int32_t *positionen, const float *messwerte, size_t anzahl,
                 float ruhe, float schwelle, float *ergebnis);

/// Was der Zeiger ANZEIGT, als "H:MM" auf einem 12-Stunden-Blatt.
/// Nicht was die Uhr weiss - beim Einmessen des Offsets ist genau diese Zahl
/// mit der Wanduhr zu vergleichen.
///
/// Null Uhr gibt es auf einem Zwoelferblatt nicht: aus 0 wird 12.
std::string zeigerzeit(int32_t position, int32_t rev, float sek_je_schritt);

// ===========================================================================
// Schwerpunkt WAEHREND der Fahrt (AP 2.3)
// ===========================================================================

/// Dasselbe Verfahren wie `schwerpunkt()`, aber ohne Array.
///
/// Die Referenzfahrt kann die Messpunkte nicht sammeln und hinterher rechnen:
/// sie faehrt EINE durchgehende langsame Fahrt und misst dabei. Wie viele
/// Punkte anfallen, weiss sie vorher nicht. Also wird laufend summiert.
///
/// Der Unterschied zur Array-Fassung ist die Schwelle: hier ist sie ein
/// fester Wert in Volt (Rauschgrenze), dort ein Anteil der bekannten
/// Gipfelhoehe. Waehrend der Fahrt ist der Gipfel noch nicht bekannt.
class Schwerpunktsammler {
 public:
  /// `ruhe` ist die gemessene Nulllinie, `schwelle` die Rauschgrenze in Volt.
  Schwerpunktsammler(float ruhe, float schwelle);

  /// Einen Messpunkt aufnehmen. Punkte unter der Schwelle gehen nicht ein.
  void nimm(int32_t position, float messwert);

  /// Anzahl der Punkte, die die Schwelle ueberschritten haben.
  size_t punkte() const { return punkte_; }

  /// Ergebnis. false heisst: kein Punkt ueber der Schwelle - die Fahrt hat
  /// kein Feld gesehen, und dann gibt es keine Referenz. NICHT stattdessen
  /// eine 0 zurueckgeben: eine 0 ist eine gueltige Position.
  bool ergebnis(float *mitte) const;

 private:
  float ruhe_;
  float schwelle_;
  double summe_gewicht_ = 0.0;
  double summe_gewicht_mal_position_ = 0.0;
  size_t punkte_ = 0;
};

/// Schwerpunkt ueber ein SYMMETRISCHES Fenster um den Gipfel.
///
/// WARUM DAS NOETIG IST - am Aufbau gemessen, 22.08.2026:
///
/// `schwerpunkt()` gewichtet ALLE Punkte oberhalb der Schwelle. Erfasst die
/// Fahrt links vom Gipfel mehr Punkte als rechts, wandert das Ergebnis nach
/// links - und wie weit sie links reicht, haengt davon ab, wo die Grobsuche
/// stehengeblieben ist. Drei Referenzfahrten ergaben dieselben ZAEHLERWERTE
/// (362, 363, 367), aber die physische Lage streute um rund 23 Schritte:
/// gemessen an der steilen Flanke der Feldkurve als -104, -74, -103 mV bei
/// 1,3 mV je Schritt. 23 Schritte sind 2,6 Grad und damit 5 Minuten
/// Anzeigefehler.
///
/// Diese Fassung sucht zuerst den Gipfel und rechnet dann nur ueber
/// [gipfel - halbbreite, gipfel + halbbreite]. Damit ist das Ergebnis
/// unabhaengig davon, wie weit die Fahrt nach links und rechts reichte -
/// solange sie das Fenster ueberhaupt abdeckt.
///
/// `halbbreite` in Schritten, sinnvoll etwa die halbe Fensterbreite.
/// Gibt false zurueck, wenn kein Punkt ueber der Schwelle lag oder wenn das
/// Fenster einseitig nicht abgedeckt ist - dann ist das Ergebnis nicht
/// vertrauenswuerdig und darf nicht gemeldet werden.
bool schwerpunkt_symmetrisch(const int32_t *positionen, const float *messwerte,
                             size_t anzahl, float ruhe, float schwelle,
                             int32_t halbbreite, float *ergebnis);

/// Neuer Hall-Offset beim Einmessen ("hier ist zwoelf Uhr").
///
/// Der Zeiger ist von Hand so gestellt, dass er auf zwoelf zeigt. Diese
/// physische Lage soll kuenftig als Zaehlerstand 0 gelesen werden.
///
/// HERGELEITET, nicht geraten - hier steckt zweimal ein Vorzeichen und einmal
/// ein Summand zu viel drin, wenn man es hinschreibt:
///
///   Die Referenzfahrt setzt den Zaehler auf (offset + nachlauf); sie faehrt
///   ueber den Schwerpunkt hinaus, und der Nachlauf ist diese Strecke.
///   Wird danach um d verstellt, liest der Zaehler z = offset + nachlauf + d.
///   Gefordert ist, dass die naechste Fahrt bei DERSELBEN physischen Lage
///   einen Wert setzt, der d Schritte vor zwoelf bedeutet:
///
///       offset_neu + nachlauf = -d
///   <=> offset_neu = -d - nachlauf = offset_alt - z
///
/// Der Nachlauf kuerzt sich heraus. Das ist nicht nur kuerzer, sondern
/// robuster: das Einmessen haengt dann nicht davon ab, dass eine
/// Referenzfahrt ihren Nachlauf richtig gemeldet hat.
int32_t offset_einmessen(int32_t offset_alt, int32_t position, int32_t rev);

// ===========================================================================
// Timer (AP 2.6)
// ===========================================================================

/// Zielposition des Timers zu einem Zeitpunkt seines Ablaufs.
///
/// Der Zeiger macht aus der Laufzeit GENAU EINE Umdrehung: er startet auf
/// zwoelf und kommt dort wieder an, wenn die Zeit um ist. Damit ist der Timer
/// ohne Skala ablesbar - halb herum heisst halbe Zeit.
///
/// `verstrichen_ms` ueber `dauer_ms` hinaus wird auf eine ganze Umdrehung
/// begrenzt; der Timer laeuft nie weiter als einmal herum.
int32_t timer_ziel(int32_t start, uint32_t verstrichen_ms, uint32_t dauer_ms,
                   int32_t rev);

/// Restzeit als "H:MM:SS". Laeuft kein Timer, kommt "--:--:--".
std::string timer_restzeit(uint32_t verstrichen_ms, uint32_t dauer_ms);

/// Fahrzeit fuer eine Strecke, aus Rampe und Hoechsttempo gerechnet.
///
/// Zwei Faelle, und welcher gilt, entscheidet die Rampe:
///
///   Dreieck   Die Strecke ist zu kurz, um `tempo_max` zu erreichen - es wird
///             nur beschleunigt und gebremst. Spitzentempo sqrt(weg*a),
///             Dauer 2*sqrt(weg/a).
///   Trapez    `tempo_max` wird erreicht und ein Stueck gehalten.
///             Dauer 2*v/a + (weg - v^2/a)/v.
///
/// Das ist dieselbe Rechnung, die FastAccelStepper in Hardware fuehrt - hier
/// steht sie, um VORHER sagen zu koennen, wie lange etwas dauert und ob es
/// ueberhaupt geht. Host-getestet, weil sie sonst nirgends pruefbar waere.
uint32_t fahrzeit_ms(int32_t weg, uint32_t beschleunigung, uint32_t tempo_max);

/// Zwei Versionen nach semver vergleichen. Ergebnis wie `strcmp`:
/// <0 wenn a aelter, 0 bei gleich, >0 wenn a neuer.
///
/// WARUM DAS HIER STEHT und nicht bei der Aktualisierung: es ist eine reine
/// Rechnung, und nur hier erreichen sie die Host-Tests. Ein falscher
/// Versionsvergleich waere besonders unangenehm - er entscheidet, ob ein
/// Geraet ein Update annimmt oder ein Downgrade zulaesst.
///
/// Verglichen werden bis zu drei Zahlen, ZAHLENMAESSIG und nicht als Text:
/// "0.10.0" ist neuer als "0.9.0", obwohl es alphabetisch davor kaeme. Fehlende
/// Stellen gelten als 0, also ist "1.2" gleich "1.2.0". Anhaengsel wie
/// "-test" werden abgeschnitten und nicht bewertet.
int semver_vergleich(const char *a, const char *b);

/// Kuerzeste Zeit, in der EINE Umdrehung ueberhaupt zu fahren ist - aus
/// Rampe und Hoechsttempo gerechnet.
///
/// WARUM DAS GEBRAUCHT WIRD: 't 1' nahm die Firmware widerspruchslos an und
/// brauchte dann ein Vielfaches davon. Der Zeiger kann eine Umdrehung nicht
/// beliebig schnell drehen, und ein Timer, der seine eigene Zusage nicht
/// halten kann, ist schlimmer als einer, der sie abweist.
///
/// Zwei Faelle, und welcher gilt, entscheidet die Rampe:
///
///   Dreieck   Die Strecke ist zu kurz, um `tempo_max` zu erreichen - es wird
///             nur beschleunigt und gebremst. Spitzentempo sqrt(rev*a),
///             Dauer 2*sqrt(rev/a).
///   Trapez    `tempo_max` wird erreicht und ein Stueck gehalten.
///             Dauer 2*v/a + (rev - v^2/a)/v.
uint32_t timer_mindestdauer_ms(int32_t rev, uint32_t beschleunigung,
                               uint32_t tempo_max);

// ===========================================================================
// Helligkeit (AP 2.5)
// ===========================================================================

/// Prozentwert des Reglers auf ein Tastverhaeltnis 0,0 .. 1,0.
///
/// `min_prozent` ist die gemessene Untergrenze, ab der die LED ueberhaupt
/// glimmt (AP 0.5). Unterhalb davon gibt der Treiber nichts Sichtbares aus,
/// deshalb wird der Bereich darueber gespreizt: 1 % am Regler ist genau die
/// Glimmschwelle, 100 % ist voll. NICHT linear von 0 weg - sonst ist das
/// erste Drittel des Reglers wirkungslos.
///
/// GENAU 0 % ist immer wirklich aus, nicht die Glimmschwelle.
///
/// Keine Gammakorrektur. Der Pruefstand setzt `gamma_correct: 1.0` mit der
/// Begruendung, dass man sonst ESPHome misst statt den Treiber - dasselbe
/// gilt hier: eine Wahrnehmungskurve gehoert in die Oberflaeche, nicht in
/// die Ansteuerung.
float tastverhaeltnis(float prozent, float min_prozent);

// ===========================================================================
// Wer darf den Zeiger bewegen (AP 1.5 - Kollisionsregel)
// ===========================================================================

/// Wer gerade die Hoheit ueber den Zeiger hat.
enum class Fuehrung {
  niemand,  ///< der Zeiger steht, niemand stellt
  homing,   ///< Referenzfahrt - hat Vorrang vor allem
  hand,     ///< Fahrbefehl von aussen (Konsole, spaeter API)
  timer,    ///< Timerlauf
  uhr,      ///< Uhrbetrieb
};

/// Die Kollisionsregel: **letzte Aktion gewinnt, keine Warteschlange** -
/// mit EINER Ausnahme, und die ist am Aufbau gelernt.
///
/// Die Referenzfahrt hat Vorrang vor allem anderen. Sie sucht eine absolute
/// Lage, und wer ihr dabei in den Zaehler schreibt, macht das Ergebnis
/// wertlos. Im Pruefstand steht die Warnung als Hinweistext an der
/// Oberflaeche ("solange LAEUFT GERADE zeigt, ueberschreibt die Suchfahrt
/// jeden Zaehler") - hier ist sie eine Regel im Code.
///
/// Darunter gilt die Reihenfolge Hand, Timer, Uhr. Ein Timerlauf schaltet
/// den Uhrbetrieb aus, weil er den Zeiger als Anzeige braucht; das tut der
/// Pruefstand genauso (`timer_lauf` setzt den Zeitmodus auf "Aus").
Fuehrung fuehrung_bestimmen(bool homing_laeuft, bool hand_faehrt,
                            bool timer_laeuft, bool uhr_an);

}  // namespace starwatch
