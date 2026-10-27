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
#include <vector>

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
/// Warum nicht eine Schwelle: die haengt an Versorgung, Temperatur und
/// Magnetabstand - frueher zusaetzlich am Poti eines Komparators. Das
/// Maximum der Feldkurve liegt dagegen dort, wo der Magnet dem Sensor
/// gegenuebersteht, und ist von jeder Schwelle unabhaengig.
///
/// Gerechnet wird mit dem BETRAG der Auslenkung gegen `ruhe`. Der Magnet
/// darf damit falsch herum sitzen: der TMAG5273 liefert
/// ohnehin den Betrag des Feldvektors, auf den Alphas war es die vorzeichenbehaftete
/// Auslenkung eines linearen, bipolaren SS49E. In beiden Faellen ist das
/// Vorzeichen gleichgueltig, und genau das prueft einer der Host-Tests. Messpunkte
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

/// Wann die Feinfahrt der Referenzfahrt aufhoeren darf (27.09.2026).
///
/// Bis dahin fuhr sie eine FESTE Strecke hinter die Flanke der Grobsuche -
/// ausgelegt fuer die 429..567 Schritte breite Kurve des KY-024. Die Kurve des
/// TMAG5273 an C4E8 ist 842 Schritte breit; die Fahrt endete 227 Schritte
/// hinter der Mitte und hatte fuer die Rechnung (150 Schritte je Seite) noch
/// 77 Schritte Reserve. Ein etwas staerkerer Magnet, und die Uhr bleibt
/// unkalibriert.
///
/// Jetzt entscheidet die Kurve selbst: fertig ist, wer den Gipfel gesehen
/// hat, mindestens `nach_gipfel` Schritte dahinter steht UND das Feld wieder
/// unter `anteil` des Gipfels gefallen ist. Die erste Bedingung deckt den
/// schmalen Magneten, die zweite den breiten. `mindestgipfel` verhindert, dass
/// Rauschen im freien Feld als Gipfel gilt.
class Feinfahrtende {
 public:
  Feinfahrtende(float ruhe, float anteil, float mindestgipfel, int32_t nach_gipfel)
      : ruhe_(ruhe), anteil_(anteil), mindestgipfel_(mindestgipfel),
        nach_gipfel_(nach_gipfel) {}

  /// Einen Messpunkt aufnehmen. true heisst: jetzt anhalten.
  bool fertig(int32_t position, float messwert);

  float gipfel() const { return gipfel_; }
  int32_t gipfel_position() const { return gipfel_pos_; }

 private:
  float ruhe_;
  float anteil_;
  float mindestgipfel_;
  int32_t nach_gipfel_;
  float gipfel_ = 0.0f;
  int32_t gipfel_pos_ = 0;
};

/// Breite des Bereichs, in dem der Betrag gegen `ruhe` ueber `schwelle` lag -
/// aus den Punkten der Feinfahrt, also auf den Schritt genau. Die Breite, die
/// tick() nebenbei misst, haengt an dessen 50-ms-Takt und kam an C4E8 einmal
/// als 842, einmal als 1 Schritt heraus. 0, wenn kein Punkt darueber lag.
int32_t feldbreite(const int32_t *positionen, const float *messwerte, size_t anzahl,
                   float ruhe, float schwelle);

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
// Homing-Verlauf (05.10.2026)
// ===========================================================================
//
// ANLASS: C4E8 stand am 05.10.2026 stundenlang auf 18 Uhr. Aus- und
// Einschalten half nicht, erst "Hier ist zwoelf". Das Log war ein Ringpuffer
// im RAM und reichte nur bis kurz nach dem Neustart - was davor geschah, war
// nicht mehr zu belegen. Verdacht: der Haltestrom (seit 01.10.2026 150 statt
// 300 mA, getestet nur 75 s in einer Stellung).
//
// Jede Referenzfahrt misst, wo der Magnet IM ALTEN ZAEHLER lag. Galt vorher
// eine Referenz und war die Endstufe nie aus, ist die Abweichung genau das,
// was der Motor seit der letzten Referenz verloren hat. Dieser Wert wird
// jetzt bei JEDER Fahrt festgehalten (auch beim taeglichen Homing um 4 Uhr)
// und ueberlebt den Neustart.

/// Was eine Abweichung bei der Referenzfahrt bedeutet.
///
/// EIN SCHRITTMOTOR VERLIERT GANZE ELEKTRISCHE PERIODEN: rutscht der Laeufer
/// unter Last durch, rastet er erst in der naechsten gleichwertigen Lage
/// wieder ein - 4 Vollschritte weiter, bei 1/16 also 64 Zaehlschritte. Eine
/// Abweichung nahe einem Vielfachen davon ist die Handschrift eines
/// Schrittverlusts; eine krumme Zahl spricht fuer etwas anderes (Riemen,
/// Magnet, Mechanik).
struct Verlustbefund {
  bool verlust = false;     ///< groesser als die Streuung der Referenzfahrt
  int32_t perioden = 0;     ///< auf ganze Perioden gerundet, mit Vorzeichen
  int32_t rest = 0;         ///< abweichung - perioden * periode
  bool periodisch = false;  ///< verlust UND |rest| <= toleranz
};

/// `periode` = 4 * Mikroschritte, `toleranz` = Streuung der Referenzfahrt.
Verlustbefund schrittverlust(int32_t abweichung, int32_t periode, int32_t toleranz);

/// Wodurch ein Eintrag im Verlauf entstand.
enum class Hominganlass : uint8_t {
  start = 0,      ///< Referenzfahrt beim Einschalten
  hand = 1,       ///< 'h' (Konsole, Weboberflaeche)
  alles_ein = 2,  ///< 'q 1'
  taeglich = 3,   ///< die taegliche Fahrt
  nachholen = 4,  ///< Wiederholung nach einem Fehlschlag
  offset = 5,     ///< keine Fahrt: der Hall-Offset wurde geaendert
};
const char *hominganlass_text(Hominganlass a);

/// Ein Eintrag. Feste Breiten, weil er als Blob im NVS liegt.
struct Homingeintrag {
  uint32_t zeit = 0;          ///< Unix-Zeit; 0 = die Uhr kannte die Zeit nicht
  uint32_t laufzeit_s = 0;    ///< Sekunden seit dem Start
  uint8_t anlass = 0;         ///< Hominganlass
  uint8_t grund = 0;          ///< Hominggrund aus lib/hall, als Zahl
  uint8_t vergleichbar = 0;   ///< 1: galt vorher eine Referenz, ohne Freilauf dazwischen
  uint8_t frei = 0;
  int16_t abweichung = 0;     ///< Schritte; bei `offset`: neuer minus alter Offset
  uint16_t haltestrom_ma = 0;
  int16_t offset = 0;         ///< Hall-Offset NACH dem Eintrag
  int16_t feldbreite = 0;     ///< Schritte
  uint16_t ruhe_ut = 0;       ///< Ruhelinie in Mikrotesla
  uint16_t spanne_ut = 0;     ///< Spanne in Mikrotesla
};

/// Ringpuffer der letzten Eintraege, als ein Block speicherbar.
class Homingverlauf {
 public:
  static constexpr size_t N = 24;
  static constexpr uint8_t VERSION = 1;

  void hinzu(const Homingeintrag &e);
  size_t anzahl() const { return d_.anzahl; }
  /// i = 0 ist der NEUESTE Eintrag. Ausserhalb: ein leerer.
  const Homingeintrag &neuer(size_t i) const;
  void leeren() { d_ = Daten(); }

  /// Fuer den Speicher: Zeiger und Groesse des Abbilds.
  const void *abbild() const { return &d_; }
  static constexpr size_t abbild_groesse() { return sizeof(Daten); }
  /// Abbild uebernehmen. Falsche Groesse, Version oder Zaehler: false, und
  /// der Verlauf bleibt leer - ein halber Verlauf aus einer alten Fassung
  /// waere eine Falschaussage.
  bool laden(const void *daten, size_t groesse);

 private:
  struct Daten {
    uint8_t version = VERSION;
    uint8_t anzahl = 0;
    uint8_t naechster = 0;
    uint8_t frei = 0;
    Homingeintrag e[N];
  };
  Daten d_;
};

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

/// Ziel im SEKUNDENTAKT: dieselbe Umdrehung wie `timer_ziel`, aber die Zeit
/// wird auf die angefangene Sekunde abgerundet. Der Zeiger steht damit eine
/// Sekunde still und springt dann um den Anteil dieser Sekunde - wie ein
/// Quarzwerk. Die letzte Sekunde endet genau auf `start + rev`, weil die
/// Timerdauer immer ganze Sekunden hat.
int32_t timer_sprungziel(int32_t start, uint32_t verstrichen_ms, uint32_t dauer_ms,
                         int32_t rev);

/// Tempo der GLEITENDEN Timerfahrt in Milli-Schritten je Sekunde (mHz).
///
/// Nennwert ist genau eine Umdrehung in `dauer_ms` - der Zeiger faehrt in
/// EINEM Zug mit gleichmaessigem Schrittabstand, statt alle paar Millisekunden
/// ein neues Ziel zu bekommen. Das war bis 27.09.2026 der Weg, und jedes
/// dieser Ziele war eine eigene Fahrt mit voller Rampe: beim 5-min-Timer ein
/// Ruck von 15 ms und 235 ms Stillstand, viermal je Sekunde.
///
/// `rueckstand` (Soll minus Ist, Schritte) wird ueber `ausgleich_ms`
/// abgebaut. Ein Schritt in jede Richtung ist Rundung von `timer_ziel` und
/// aendert nichts - sonst wackelte das Tempo bei langen Timern bei jedem
/// Nachfuehren, weil die Istposition dort 13,5 s lang auf einem Schritt steht.
///
/// Begrenzt auf halben Nennwert (nie rueckwaerts, nie stehen) und
/// `tempo_max` (Schritte/s). 0 bei `rev` <= 0 oder `dauer_ms` 0.
uint32_t timer_gleittempo_mhz(int32_t rev, uint32_t dauer_ms, int32_t rueckstand,
                              uint32_t ausgleich_ms, uint32_t tempo_max);

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

// ---------------------------------------------------------------------------
// Weiche Fahrt (28.09.2026) - der Zeiger soll gleiten, nicht zucken
// ---------------------------------------------------------------------------

/// Beschleunigung fuer eine WEICHE Fahrt ueber `weg` Schritte: nie mehr als
/// `a_max` (der Anlauf-Regler), und bei kurzen Wegen so klein, dass die Fahrt
/// mindestens `min_ms` dauert. Dreiecksprofil: t = 2*sqrt(weg/a), also
/// a = 4*weg/t^2. Ein Minutensprung (4 Schritte) dauerte mit voller Rampe
/// 18 ms - ein Zucken. Mindestens 1.
uint32_t weiche_beschleunigung(uint32_t weg, uint32_t a_max, uint32_t min_ms);

/// Ueber so viele Schritte waechst die Beschleunigung von null auf ihren
/// Wert (FastAccelStepper::setLinearAcceleration): ein Viertel des Wegs,
/// hoechstens `max_schritte`. Ohne das setzt die volle Beschleunigung
/// schlagartig ein - das Tempo steigt zwar gleichmaessig, aber der Ruck am
/// Anfang ist das, was das Auge als "hart" sieht.
uint32_t weicher_einsatz(uint32_t weg, uint32_t max_schritte);

// ---------------------------------------------------------------------------
// Zuendung der Klinge (28.09.2026)
// ---------------------------------------------------------------------------

/// Ein Augenblick der Zuendung: Helligkeit als ANTEIL des Zielwerts (1,0 =
/// eingestellte Helligkeit, darueber das kurze Aufleuchten) und die Farbe.
struct Zuendbild {
  float anteil;
  uint32_t farbe;  ///< 0xRRGGBB
};

/// Gesamtdauer der Zuendung in ms. Danach gilt der eingestellte Wert.
constexpr uint32_t ZUEND_DAUER_MS = 2800;

/// Die Zuendung zum Zeitpunkt `t_ms` - wie ein Lichtschwert:
///
///   0,00 s  kurzes, warmes Vorglimmen
///   0,18 s  zuegiges Ausfahren bis 1,25 des Zielwerts, HEISS (zum Weissen
///           verschoben)
///   0,65 s  unruhiges Plasma: 4-6 deutliche Flackerer (60-120 ms, 25-55 %),
///           nach hinten flacher - aber NIE zurueck ins Dunkle
///   1,70 s  ruhiges Einschwingen, gelegentlich ein kleiner Nachzucker; die
///           Hitze klingt ab, die Farbe gleitet von `alt` nach `ziel`
///   2,80 s  genau Zielwert und Zielfarbe
///
/// HITZE heisst: die Farbe ist zu `warm` hin verschoben. Rot kommt dadurch
/// erst als Orangerot und saettigt sich dann - die Farbe ist "noch nicht
/// ganz da". `alt` == `ziel` beim Einschalten, beim Farbwechsel die vorige.
/// `zufall` streut die Zuendversuche, damit keine Zuendung der anderen
/// gleicht; bei gleichem `zufall` ist das Ergebnis gleich (Host-Tests).
/// Die waermere Variante einer Farbe (01.10.2026): Farbton um 35 % des
/// Abstands Richtung Rot (Gelb -> Orange, Gruen -> Gelbgruen, Blau ->
/// Violett, Rot bleibt Rot), Weiss und Pastell ueber den Weissabgleich
/// (weniger Blau). Saettigung und Helligkeit bleiben. `staerke` 0..1.
/// Ersetzt das Mischen mit Warmweiss: dabei lief jeder Farbwechsel ueber Weiss.
uint32_t waermer(uint32_t farbe, float staerke);

Zuendbild zuend_bild(uint32_t t_ms, uint32_t alt, uint32_t ziel,
                     uint32_t zufall);

/// Gesamtdauer des Einfahrens in ms.
constexpr uint32_t LOESCH_DAUER_MS = 1900;

/// Das Gegenstueck: die Klinge faehrt EIN (28.09.2026). Langsames Aufwallen
/// (bis 1,08, dabei heisser), ruhiges Ausblenden, weiches Nachglimmen - ohne
/// Funken oder Blitze. Danach dunkel. Anteil relativ zum eingestellten Wert.
Zuendbild loesch_bild(uint32_t t_ms, uint32_t farbe, uint32_t zufall);

/// Dauer der schlichten Ueberblendung, wenn der Zuendeffekt abgeschaltet ist.
constexpr uint32_t BLENDE_DAUER_MS = 500;

/// Schlichte Ueberblendung ohne Effekt: Helligkeitsanteil von `von` nach
/// `nach`, Farbe von `alt` nach `ziel`, weich (smoothstep). Einschalten
/// 0 -> 1, Ausschalten 1 -> 0, Farbwechsel 1 -> 1.
Zuendbild blende_bild(uint32_t t_ms, float von, float nach, uint32_t alt, uint32_t ziel);

// ---------------------------------------------------------------------------
// UEBERGANG UND PULS (0.20.0, 30.09.2026)
// ---------------------------------------------------------------------------
// Home Assistant schickt "transition" in Sekunden und erwartet, dass die
// Klinge vom GERADE GEZEIGTEN Wert aus gleitet - in beide Richtungen, auch
// mitten in einem laufenden Uebergang. Bis 0.19.8 wurde das Feld nicht
// gelesen: Abdunkeln sprang, und jeder Farbwechsel loeste die Zuendung aus,
// die in ZUEND_WARM (Warmweiss) beginnt.

/// Zwei Farben je Kanal linear mischen, x in 0..1.
uint32_t farbe_mischen(uint32_t a, uint32_t b, float x);

/// Linearer Uebergang von `von` nach `nach` nach `t_ms` von `dauer_ms`.
/// Dauer 0 oder vorbei: `nach`.
float uebergang_wert(float von, float nach, uint32_t t_ms, uint32_t dauer_ms);

constexpr uint32_t PULS_PERIODE_MIN_MS = 400;
constexpr uint32_t PULS_PERIODE_MAX_MS = 4000;
constexpr uint32_t PULS_PERIODE_VORGABE_MS = 2000;
constexpr float PULS_MIN_UNTEN = 0.01f;   ///< nie 0 - der Puls schaltet nie aus
constexpr float PULS_MIN_OBEN = 0.50f;
constexpr float PULS_MIN_VORGABE = 0.08f;

/// Hoehe des Pulses als Anteil des Maximums: Sinus zwischen `min_anteil`
/// (Phase 0) und 1 (Phase 0,5). Eine Periode = einmal auf und ab.
float puls_anteil(float phase, float min_anteil);

/// Die Phase auf der STEIGENDEN Haelfte, bei der der Puls `anteil` zeigt -
/// damit ein Puls dort beginnt, wo die Klinge gerade steht.
float puls_phase_fuer(float anteil, float min_anteil);

/// Phase um `dt_ms` weiterdrehen. Die Periode darf sich zwischen zwei
/// Aufrufen aendern, ohne dass die Hoehe springt.
float puls_phase_weiter(float phase, uint32_t dt_ms, uint32_t periode_ms);

uint32_t puls_periode_begrenzt(uint32_t ms);
float puls_min_begrenzt(float anteil);

/// Ein Befehl an die Klinge, wie ihn Home Assistant schickt (JSON-Schema):
///   {"state":"ON","brightness":80,"color":{"r":255,"g":0,"b":0},
///    "transition":2.5,"effect":"Puls","flash":"short"}
/// Jedes Feld ist optional. Kein vollstaendiger JSON-Leser - flaches JSON
/// mit einem Objekt "color", genau das, was HA schickt.
struct Lichtbefehl {
  int state = -1;             ///< -1 fehlt, 0 OFF, 1 ON
  int hell = -1;              ///< 0..100 (brightness_scale 100), -1 fehlt
  bool farbe_da = false;
  uint32_t farbe = 0;
  int32_t uebergang_ms = -1;  ///< -1 fehlt; begrenzt auf 0..3600 s
  int effekt = -1;            ///< -1 fehlt, 0 "none", 1 "Puls", 2 unbekannt
  int blitze = 0;             ///< flash: short 1, long 3
};
bool lichtbefehl_lesen(const char *json, Lichtbefehl *b);

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

/// Eine GEMERKTE Manifest-Adresse auf die heutigen Namen umstellen
/// (28.09.2026). Die Manifeste hiessen bis 0.18.0 anders, und ein gemerkter
/// Wert gewinnt gegen jede neue Vorgabe - ohne Umstellung fragte die Uhr fuer
/// immer eine Datei, die nicht mehr gepflegt wird.
///
/// Umgestellt wird NUR eine Adresse im Ordner `ordner`, die keiner der
/// beiden heutigen ist: steht "beta" darin, auf `beta`, sonst auf `stabil`.
/// Alles andere - auch eine selbst gesetzte fremde URL - bleibt, wie es ist.
/// @return true, wenn umgestellt wurde.
bool update_url_umstellen(std::string *url, const char *ordner,
                          const char *stabil, const char *beta);

/// Hoechstzahl der ZEICHEN eines Uhrnamens - nicht Bytes: "Kueche" mit ue
/// als Umlaut sind sechs Zeichen, auch wenn es sieben Bytes sind.
constexpr size_t ALIAS_MAX_ZEICHEN = 24;

/// Uhrnamen pruefen und saeubern (27.09.2026).
///
/// Der Name steht in der Weboberflaeche, im Browsertab und in Home Assistant
/// als Geraetename - dort landet er in JSON. Deshalb gueltiges UTF-8, keine
/// Steuerzeichen und keins von  " \ < >  - die einzigen Zeichen, die in
/// JSON oder HTML eine Bedeutung haben. Leerzeichen am Rand fallen weg,
/// innen bleiben sie. Leer ist GUELTIG und heisst "kein Name".
///
/// @return false, wenn der Name so nicht geht; `sauber` bleibt dann unberuehrt.
bool alias_pruefen(const std::string &roh, std::string *sauber);

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
// Farbe (drei LED-Stufen R, G, B)
// ===========================================================================

/// Farbe aus Text, als 0xRRGGBB. Nimmt "#ff8800", "ff8800", "255 136 0" und
/// die Namen rot, gruen, blau, gelb, violett, weiss, orange, tuerkis (auch
/// mit Umlaut, Gross/klein egal). "weiss" ist das Weiss DIESER Uhr
/// (`weiss`, 'lw'), denn die LEDs streuen von Uhr zu Uhr (27.10.2026).
///
/// Gibt false zurueck und laesst `farbe` UNBERUEHRT, wenn der Text keine
/// Farbe ist. Ein Vertipper soll nichts veraendern - eine halb gelesene
/// Farbe waere ein Kanal auf null und saehe aus wie ein kaputter Treiber.
bool farbe_lesen(const char *text, uint32_t *farbe, uint32_t weiss = 0xFFD764);

/// Tastverhaeltnis je Kanal aus dem Gesamtanteil (tastverhaeltnis()) und der
/// Farbe, begrenzt auf eine LEISTUNG. `aus[0]` rot, `aus[1]` gruen,
/// `aus[2]` blau, jeweils 0 .. 1. Gibt die geschaetzte Leistung in Watt
/// zurueck, NACH der Begrenzung.
///
/// `watt_voll[k]` ist die Leistung des Kanals bei 100 % Tastverhaeltnis.
/// Liegt die Summe ueber `max_w`, werden ALLE Kanaele um denselben Faktor
/// zurueckgenommen - der Farbton bleibt, nur die Helligkeit sinkt. Liegt sie
/// darunter, wird nichts veraendert: Rot allein bei 100 % bleibt 100 %.
///
/// LINEAR, ohne Gamma - aus demselben Grund wie bei tastverhaeltnis(): hier
/// wird der Treiber angesteuert, nicht das Auge bedient. Ein Farbwert von 128
/// sind 50 % Tastverhaeltnis und damit halber LED-Strom im Mittel.
float kanal_anteile(float anteil, uint32_t farbe, const float watt_voll[3],
                    float max_w, float aus[3]);

// ===========================================================================
// Lichtsignale: Meldungen (MQTT) und spaeter der Geraetezustand
// ===========================================================================

/// Wie ein Signal leuchtet.
enum class Muster : uint8_t {
  leuchten,   ///< ruhig
  pulsieren,  ///< langsam an- und abschwellend, 2,4 s je Zug
  blinken,    ///< 0,5 s an, 0,5 s aus
};

const char *muster_text(Muster m);

/// "leuchten", "pulsieren", "blinken". false bei allem anderen, `m` bleibt.
bool muster_lesen(const char *text, Muster *m);

/// Helligkeitsanteil 0..1 eines Musters, `seit_ms` nach dessen Beginn.
/// Pulsieren faengt OBEN an und faellt nie unter 15 % - eine Meldung, die
/// zwischendurch ganz dunkel wird, sieht aus, als sei sie vorbei.
float muster_anteil(Muster m, uint32_t seit_ms);

/// Eine Meldung, wie sie von aussen kommt (MQTT, Konsole, Oberflaeche).
struct Meldung {
  uint32_t farbe = 0xFFFFFF;
  Muster muster = Muster::leuchten;
  uint32_t dauer_s = 0;  ///< 0 = bis sie quittiert wird
  float hell = -1.0f;    ///< Prozent 1..100; < 0 = der Reglerwert
};

/// Hoechste Dauer einer Meldung: ein Tag. Laenger ist ein Vertipper.
constexpr uint32_t MELDUNG_DAUER_MAX_S = 86400;

/// Liest "<farbe> [muster] [dauer_s] [hell]", durch Leerzeichen getrennt.
/// Die Farbe ist ein Name oder Hex OHNE Leerzeichen. Die Reihenfolge der
/// drei optionalen Teile ist frei - ein Wort ist das Muster, die erste Zahl
/// die Dauer, die zweite die Helligkeit.
/// Gibt false zurueck und laesst `m` unberuehrt, wenn etwas nicht passt.
bool meldung_lesen(const char *text, Meldung *m);

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

// ===========================================================================
// Umgebungslicht: was die Uhr aus der Beleuchtungsstaerke macht
// ===========================================================================
//
// WARUM DIESE RECHNUNG HIER STEHT UND NICHT IN lib/lux: sie ist reine
// Arithmetik und ein Zustandsautomat - also genau die Art Teil, die sich
// auf dem Mac pruefen laesst und bei der ein Denkfehler am Geraet als
// "flackert manchmal" auftritt und wochenlang gesucht wird. lib/lux liest
// den Sensor, hier wird entschieden.

/// Helligkeitsfaktor aus der Beleuchtungsstaerke, 0 .. 1.
///
/// Liefert `anteil_min` bei `lux <= lux_dunkel`, 1,0 bei `lux >= lux_hell`
/// und dazwischen eine LOGARITHMISCHE Ueberblendung.
///
/// WARUM LOGARITHMISCH: das Auge empfindet Helligkeit als Verhaeltnis, nicht
/// als Differenz. Zwischen 1 lx und 300 lx liegt der ganze Weg von der
/// Nachttischlampe bis zum hellen Zimmer, aber linear ueberblendet liegt 50 lx
/// - ein normal beleuchtetes Wohnzimmer - schon bei 16 % der Skala. Der
/// ganze Abend spielte sich dann im untersten Sechstel ab.
///
/// Mit dem Logarithmus liegt derselbe Punkt bei 68 %: die Klinge wird
/// heller, wenn das Zimmer heller wird, und nicht erst kurz vor Tageslicht.
///
/// WARUM UEBERHAUPT HELLER BEI HELL: die Klinge leuchtet GEGEN das
/// Umgebungslicht an. Im dunklen Zimmer blendet voller Ausschlag, im hellen
/// Zimmer verschwindet ein Zehntel davon.
float licht_anteil(float lux, float lux_dunkel, float lux_hell,
                   float anteil_min);

/// Den Dimmfaktor WEICH auf sein Ziel nachfuehren (27.09.2026).
///
/// Der Sensor liefert alle 2 s einen Wert, und bis dahin sprang die Klinge
/// jedes Mal auf den neuen Faktor - sichtbare Stufen. Jetzt naehert sich der
/// Faktor dem Ziel exponentiell: nach `tau_ms` ist rund 63 % des Wegs
/// geschafft, nach dem Dreifachen 95 %. Grosse Spruenge laufen damit schnell
/// an und klingen sanft aus, kleine sieht man gar nicht.
///
/// Unter einem halben Promille Rest steht der Wert auf dem Ziel - sonst
/// schriebe der Aufrufer ewig winzige Aenderungen in den LED-Treiber.
/// `tau_ms` 0 heisst sofort. Werte ausserhalb 0..1 und NaN werden geklemmt.
float dimm_nachfuehren(float ist, float ziel, uint32_t dt_ms, uint32_t tau_ms);

/// Einstellungen der Lichtautomatik, wie sie der Benutzer setzt (27.09.2026).
/// Bis dahin standen alle Werte fest in geraet.h, und die Automatik lief nach
/// jedem Neustart wieder an - einstellbar nur ueber 'lu' an der Konsole.
struct Lichtautomatik {
  bool an = true;            ///< dimmen nach Umgebungslicht
  bool nacht = true;         ///< nachts ausschalten
  float anteil_min = 0.30f;  ///< kleinster Anteil des Reglerwerts, 0,05..1
  float lux_hell = 20.0f;    ///< ab hier voller Reglerwert, 10..2000 lx
  float nacht_aus = 0.2f;    ///< darunter aus, 0,1..20 lx
};

/// Werte in ihre Grenzen holen. Gegen Vertipper und gegen einen NVS-Inhalt
/// aus einer anderen Fassung - nie gegen den Benutzer: was innerhalb der
/// Grenzen liegt, bleibt unveraendert.
Lichtautomatik lichtautomatik_begrenzt(Lichtautomatik a, float lux_dunkel);

/// Einschaltschwelle der Nacht: gut ein Drittel ueber der Ausschaltschwelle
/// (Faktor 1,35 - aus unter 0,3 lx heisst an ueber 0,4 lx), aber hoechstens
/// die Haelfte von `lux_hell`. BIS 28.09.2026 WAR ES DAS SECHSFACHE (0,5 /
/// 3,0): bei gedaempftem Stimmungslicht zuendete die Klinge dann nie. Gegen
/// Rauschen an der Schwelle schuetzt beim Ausschalten die Frist und
/// abbruch_lux(), beim Einschalten der Abstand von einem Drittel.
float nacht_an_lux(const Lichtautomatik &a);

/// Die Nachtabschaltung: unter einer Schwelle geht die Klinge aus, ueber
/// einer zweiten wieder an.
///
/// ZWEI SCHWELLEN UND EINE FRIST, und beides aus demselben Grund: die
/// Beleuchtungsstaerke wandert in der Daemmerung langsam durch den
/// Schaltpunkt und rauscht dabei. Mit einer einzigen Schwelle und ohne Frist
/// schaltete die Klinge dann minutenlang im Sekundentakt.
///
///   - HYSTERESE gegen das Rauschen um den Schaltpunkt: ausgeschaltet wird
///     unter `aus_lux`, eingeschaltet erst wieder ueber `an_lux`.
///   - FRIST gegen kurze Ereignisse: der Schatten, der ueber den Sensor
///     faellt, und die Taschenlampe, die nachts durchs Zimmer geht. Beide
///     sollen die Klinge nicht umschalten.
///
/// Die Zeit kommt VON AUSSEN herein (`jetzt_ms`), damit hier keine
/// Zeitquelle steckt - nur so laeuft der Automat im Host-Test.
class Nachtschalter {
 public:
  /// `aus_lux` muss unter `an_lux` liegen; sonst gibt es keine Hysterese und
  /// der Automat verhaelt sich wie mit einer einzigen Schwelle.
  /// `frist_ms` gilt fuer das Ausschalten, `frist_an_ms` fuer das
  /// Einschalten - ohne Angabe dieselbe. Die Uhr schaltet SOFORT ein
  /// (28.09.2026): wer das Licht anmacht, will die Klinge sehen, nicht eine
  /// Minute warten. Das Ausschalten wartet, damit ein Schatten sie nicht
  /// loescht.
  Nachtschalter(float aus_lux, float an_lux, uint32_t frist_ms,
                uint32_t frist_an_ms = UINT32_MAX);

  /// Ab hier bricht ein laufender Countdown zum AUSschalten ab: ein Viertel
  /// des Wegs von der unteren zur oberen Schwelle. Direkt an der unteren
  /// Schwelle faengt er sonst bei jedem Messwert neu an, der knapp darueber
  /// liegt - am Geraet gesehen, das Zimmer stand bei 0,49 lx und die Klinge
  /// ging nie aus.
  float abbruch_lux() const {
    // Mindestens 20 % ueber der Ausschaltschwelle - bei engen Schwellen
    // (0,3 / 0,4) laege ein Viertel des Abstands sonst im Rauschen.
    const float viertel = 0.25f * (an_lux_ - aus_lux_);
    const float mindest = 0.20f * aus_lux_;
    return aus_lux_ + (viertel > mindest ? viertel : mindest);
  }

  /// Einen neuen Messwert hineingeben. Liefert zurueck, ob die Klinge AN
  /// sein soll.
  ///
  /// Der Startzustand ist AN: eine Uhr, die nach dem Einschalten erst einmal
  /// dunkel bleibt, sieht kaputt aus. Ist es wirklich Nacht, schaltet sie
  /// nach der Frist von selbst ab.
  bool aktualisiere(float lux, uint32_t jetzt_ms);

  bool an() const { return an_; }

  /// Schwellen zur Laufzeit aendern (Weboberflaeche, 27.09.2026). Der Zustand
  /// bleibt, eine laufende Frist wird verworfen - sie gehoerte zur alten
  /// Schwelle.
  void schwellen(float aus_lux, float an_lux) {
    aus_lux_ = aus_lux;
    an_lux_ = an_lux;
    wechsel_angemeldet_ = false;
  }
  float aus_lux() const { return aus_lux_; }
  float an_lux() const { return an_lux_; }

  /// Wie lange der Messwert schon jenseits der Schwelle liegt, in
  /// Millisekunden. 0 heisst: er liegt im Haltebereich, es laeuft keine
  /// Frist. Fuer die Anzeige - damit sichtbar ist, dass gleich geschaltet
  /// wird, statt dass es einfach geschieht.
  uint32_t frist_laeuft_ms(uint32_t jetzt_ms) const;

 private:
  float aus_lux_;
  float an_lux_;
  uint32_t frist_ms_;
  uint32_t frist_an_ms_;

  bool an_ = true;
  bool wechsel_angemeldet_ = false;  ///< Messwert liegt jenseits der Schwelle
  uint32_t wechsel_seit_ = 0;
};

// ===========================================================================
// Einrichtung per Bluetooth (23.10.2026)
// ===========================================================================
//
// Zweiter Weg neben dem offenen Zugangspunkt, mit derselben Freigabe: nur im
// Einrichtungszustand. Hier steht nur, was ohne Funk pruefbar ist.

/// Ein Netz aus dem Scan.
struct Funknetz {
  std::string ssid;
  int rssi = 0;
  bool offen = false;
};

/// Netzliste als JSON fuer das BLE-Merkmal: staerkstes zuerst, jede SSID
/// einmal, versteckte (leere) weg. Nie laenger als `max_bytes` - ein
/// BLE-Attribut fasst 512 Bytes; was nicht passt, faellt hinten weg.
std::string netze_json(std::vector<Funknetz> netze, size_t max_bytes);

/// Schreibauftrag der App: "SSID\nPasswort". false bei leerer SSID oder
/// mehr als 32 Bytes, und bei einem Passwort, das WPA nicht annimmt
/// (1..7 oder mehr als 64 Zeichen). Leeres Passwort = offenes Netz.
bool wlan_auftrag_lesen(const std::string &daten, std::string *ssid, std::string *pass);

}  // namespace starwatch
