// ---------------------------------------------------------------------------
// Star Watch - lib/hall: Sensor und Referenzfahrt. AP 2.3.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Den TMAG5273 lesen und daraus EINEN Wert machen: die Position, an der der
// Magnet dem Sensor gegenuebersteht.
//
// ===========================================================================
// WAS SICH GEGENUEBER DEN ALPHAS GEAENDERT HAT - UND WAS NICHT
// ===========================================================================
// GEAENDERT hat sich, WOHER die Feldzahl kommt: nicht mehr aus
// analogReadMilliVolts() am A0 einer KY-024, sondern aus drei Registern des
// TMAG5273 ueber I2C. Und die Einheit: Volt sind Millitesla geworden.
//
// NICHT geaendert hat sich der Algorithmus. Die Referenzfahrt bleibt
// zweistufig - Grobsuche, Rueckzug, eine durchgehende langsame Fahrt - und
// holt ihre Genauigkeit weiter aus dem SCHWERPUNKT ueber die Feldkurve. Das
// war schon vor dem Sensorwechsel so: seit dem 22.08.2026 haengt die
// Grobsuche nicht mehr am Komparator D0, sondern an einer Schwelle in
// Software. Genau deshalb kostet der Wechsel auf einen Sensor ohne D0 nichts.
//
// ===========================================================================
// WARUM SCHWERPUNKT UND NICHT DIE FLANKE
// ===========================================================================
// Eine Schwelle - egal ob im Poti eines Komparators oder in einer Konstante -
// haengt an Versorgung, Temperatur und Magnetabstand. Das Maximum der
// Feldkurve liegt dort, wo der Magnet dem Sensor gegenuebersteht, und ist
// von jeder Schwelle unabhaengig.
//
// Gerechnet wird mit dem BETRAG des Feldvektors. Der ist immer positiv, und
// damit ist es gleichgueltig, welche Polflaeche vorbeizieht und wie das
// Modul in seiner Aufnahme verdreht steckt. Beim SS49E galt das nur fuer die
// Polung, nicht fuer die Lage.
//
// Die Rechnung steht in lib/zeiger (Schwerpunktsammler), host-getestet.
// Diese Bibliothek liefert ihr nur die Messpunkte.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#include <antrieb.h>

#include "tmag5273.h"

namespace starwatch {

/// Warum eine Referenzfahrt so ausgegangen ist.
enum class Hominggrund {
  nicht_gelaufen,     ///< Startwert - es hat noch keine Fahrt gegeben
  ok,
  kein_sensor,        ///< der TMAG5273 antwortet nicht
  kein_magnet,        ///< in einer ganzen Umdrehung keine Feldschwelle erreicht
  zaehler_steht,      ///< der Weg wurde nicht zurueckgelegt
  kein_feld,          ///< kein Messpunkt ueber der Rauschschwelle
  abgebrochen,        ///< von aussen gestoppt
};

const char *hominggrund_text(Hominggrund g);

/// Ergebnis einer Referenzfahrt.
struct Homingergebnis {
  Hominggrund grund = Hominggrund::nicht_gelaufen;
  bool ok() const { return grund == Hominggrund::ok; }

  float schwerpunkt = 0.0f;   ///< Position des Feldmaximums, in Schritten
  int32_t pos_flanke = 0;     ///< Zaehlerstand beim Ueberschreiten der Schwelle
  int32_t nachlauf = 0;       ///< Weg vom Maximum bis zum Stillstand
  int32_t zaehler = 0;        ///< was der Zaehler danach zeigt
  float ruhe_mt = 0.0f;       ///< gemessene Nulllinie im freien Feld
  float spanne_mt = 0.0f;     ///< groesste Auslenkung gegen die Nulllinie
  int32_t fenster = 0;        ///< Feldbreite ueber der Grobschwelle, aus der Feinfahrt
  int32_t fein_von = 0;       ///< Beginn der Feinfahrt, relativ zur Mitte (negativ)
  int32_t fein_bis = 0;       ///< Ende der Feinfahrt (Stillstand), relativ zur Mitte
  size_t punkte = 0;          ///< Messpunkte der Feinanfahrt
};

class Hall {
 public:
  /// Wird waehrend der Wartezeiten der Referenzfahrt aufgerufen. Damit bleibt
  /// die Konsole bedienbar, obwohl der Ablauf sequenziell geschrieben ist.
  /// Gibt der Rueckgabewert false, wird die Fahrt abgebrochen.
  using Pumpe = bool (*)();

  /// Sensor suchen und einrichten. Der I2C-Bus muss schon laufen.
  /// Gibt false zurueck, wenn kein TMAG5273 antwortet - dann kann die Uhr
  /// NICHT referenzieren, und der Aufrufer MUSS das melden.
  bool beginn();

  bool bereit() const { return sensor_.bereit(); }
  Tmaggrund sensorgrund() const { return sensor_.grund(); }
  uint8_t sensoradresse() const { return sensor_.adresse(); }

  /// Die drei Achsen einzeln, in Millitesla. Nur fuer Anzeigen und fuer das
  /// Einrichten der Mechanik - die Referenzfahrt benutzt sie nicht.
  bool achsen_mt(float *bx, float *by, float *bz) {
    return sensor_.achsen_mt(bx, by, bz);
  }

  /// Ist der Magnet gerade vor dem Sensor? Aus dem zuletzt GEFILTERTEN Wert,
  /// ohne neue Messung - fuer Anzeigen, die im Sekundentakt abgefragt werden
  /// und den Bus nicht belasten sollen.
  bool magnet_erkannt() const;

  /// Der entprellte Zustand, wie ihn die Flankenerkennung sieht.
  bool magnet_stabil() const { return letzter_zustand_; }

  /// Liegt ein Feld an? FRISCH gemessen. Das ist die Groesse, mit der die
  /// Grobsuche arbeitet.
  bool feld_erkannt();

  /// Betrag des Feldvektors in Millitesla, eine Messung.
  float feld_mt();

  /// Mehrfach gemessen und gemittelt.
  ///
  /// EIN WERT IST DER NORMALFALL, und das ist der Unterschied zum SS49E: der
  /// TMAG5273 mittelt in Hardware ueber 32 Wandlungen (CONV_AVG in
  /// geraet.h). Die Mittelung, die der ESP32-ADC in Software brauchte, ist
  /// damit schon geschehen, bevor der Wert auf den Bus kommt.
  float feld_gemittelt(uint8_t proben = 1);

  /// Frueher: Median aus drei Mittelwerten gegen die Ausreisser des
  /// ESP32-ADC. Diese Ausreisser gibt es nicht mehr - der Wert kommt als
  /// Zahl ueber den Bus, nicht aus einer Wandlung im ESP32.
  ///
  /// Die Funktion BLEIBT, weil die Referenzfahrt und die Oberflaeche sie
  /// rufen, und sie mittelt weiterhin ueber ein paar Werte: gegen das
  /// Rauschen des Sensors selbst, nicht gegen Ausreisser.
  float feld_robust();

  /// Der letzte gefilterte Wert, ohne neu zu messen.
  float feld_gefiltert() const { return gefiltert_mt_; }

  /// Spitzenwerte seit dem letzten Ruecksetzen - damit findet man beim Drehen
  /// von Hand den Ort des Maximums, ohne auf einen Zahlenwert zu starren.
  void spitzen_ruecksetzen();

  /// Flankenzahl und gemessene Fensterbreite zuruecksetzen.
  ///
  /// NICHT in spitzen_ruecksetzen() hineinlegen: das ruft die Referenzfahrt
  /// selbst, auch mitten drin. Nur fuer das Kommando 'c'.
  void zaehler_ruecksetzen();

  float spanne_mt() const { return max_mt_ - min_mt_; }
  float max_mt() const { return max_mt_; }
  float min_mt() const { return min_mt_; }

  /// Aus der Hauptschleife aufrufen: fuehrt die Spitzenwerte und erkennt die
  /// Schwellenuebergaenge. `position` ist der Zaehlerstand des Antriebs.
  void tick(int32_t position);

  /// Zaehler der erkannten Uebergaenge und die zuletzt gemessene
  /// Fensterbreite.
  uint32_t flanken() const { return flanken_; }
  int32_t fenster_schritte() const { return fenster_schritte_; }

  /// Die letzte gemessene Ruhelinie. Beim TMAG5273 ist das der Rest, den der
  /// Sensor im freien Feld sieht - Erdfeld und Nullpunktfehler, zusammen
  /// einige Hundertstel Millitesla. Startwert aus geraet.h, ersetzt bei jeder
  /// Referenzfahrt durch eine Messung.
  float ruhe_mt() const { return ruhe_mt_; }

  // -------------------------------------------------------------------------
  // Die Referenzfahrt
  // -------------------------------------------------------------------------

  /// Zweistufig: Grobsuche bis zur Feldschwelle, Rueckzug, dann eine
  /// DURCHGEHENDE langsame Fahrt ueber das Fenster, dabei messen.
  ///
  /// `offset` ist der eingemessene Winkel zwischen Magnet und Klinge - jede
  /// gebaute Uhr hat ihren eigenen, er entsteht erst bei der Montage.
  ///
  /// Diese Funktion FAEHRT und kehrt erst zurueck, wenn sie fertig ist. Der
  /// Motor laeuft dabei in Hardware, das Warten kostet also keine
  /// Gleichmaessigkeit; `pumpe` haelt waehrenddessen die Konsole am Leben.
  Homingergebnis referenzfahrt(Antrieb &antrieb, int32_t offset, Pumpe pumpe);

  /// Tempo der Grobsuche und der Feinfahrt in Schritten/s - zur Laufzeit
  /// einstellbar, damit sich am Geraet MESSEN laesst, wie schnell die
  /// Referenzfahrt sein darf, ohne ungenau zu werden (27.09.2026). Nicht
  /// gespeichert: nach dem Neustart gelten die Vorgaben.
  ///
  /// WARUM DAS JETZT GEHT: die 25 Schritte/s der Feinfahrt stammen vom
  /// KY-024 am ADC - bei 50 streute der Schwerpunkt dort um 13 Schritte. Der
  /// TMAG5273 wandelt digital und mittelt selbst; ueber 90 % der rund 60 s
  /// einer Referenzfahrt steckten in dieser einen Stufe.
  void tempi(uint32_t fein, uint32_t grob);
  uint32_t fein_tempo() const { return fein_tempo_; }
  uint32_t grob_tempo() const { return grob_tempo_; }

 private:
  bool warte_bis_ziel(Antrieb &antrieb, uint32_t timeout_ms, Pumpe pumpe);

  Tmag5273 sensor_;

  float ruhe_mt_ = 0.0f;
  float max_mt_ = 0.0f;
  float min_mt_ = 0.0f;
  uint32_t flanken_ = 0;
  bool letzter_zustand_ = false;
  bool roh_letzter_ = false;
  uint32_t roh_seit_ = 0;
  uint32_t letzte_messung_ = 0;
  float gefiltert_mt_ = 0.0f;
  int32_t pos_flanke_ = 0;
  int32_t fenster_schritte_ = 0;
  bool fenster_aus_fein_ = false;  ///< Breite stammt aus der Referenzfahrt
  bool gesehen_ = false;
  // GEMESSEN 27.09.2026 an C4E8: 200/1600 braucht 6,6..7,1 s statt 44 s und
  // streut wie 25 (+-2..3 Schritte); 800 streute um 10 Schritte.
  uint32_t fein_tempo_ = 200;
  uint32_t grob_tempo_ = 1600;
};

}  // namespace starwatch
