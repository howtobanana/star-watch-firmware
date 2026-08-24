// ---------------------------------------------------------------------------
// Star Watch - lib/hall: Sensor und Referenzfahrt. AP 2.3.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Den KY-024 lesen (D0 digital, A0 analog) und daraus EINEN Wert machen: die
// Position, an der der Magnet dem Sensor gegenuebersteht.
//
// ===========================================================================
// WARUM SCHWERPUNKT UND NICHT DIE FLANKE
// ===========================================================================
// Die Flanke des Komparators haengt am Poti der KY-024, an der
// Versorgungsspannung und an der Temperatur. Das Maximum der Feldkurve liegt
// dort, wo der Magnet dem Sensor gegenuebersteht, und ist von jeder Schwelle
// unabhaengig. Gerechnet wird mit dem BETRAG der Auslenkung: der SS49E ist
// linear und bipolar, der Magnet darf also falsch herum eingeklebt sein.
//
// Die Rechnung steht in lib/zeiger (Schwerpunktsammler), host-getestet.
// Diese Bibliothek liefert ihr nur die Messpunkte.
//
// Ein Nebeneffekt, der wichtig ist: weil die Genauigkeit aus der Feldkurve
// kommt und nicht aus der Flanke, ist es gleichgueltig, dass die steigende
// D0-Flanke durch den 100-nF-Kondensator um rund 1 ms verschleppt ist. Die
// Flanke dient nur der Grobsuche.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#include <antrieb.h>

namespace starwatch {

/// Warum eine Referenzfahrt so ausgegangen ist.
enum class Hominggrund {
  nicht_gelaufen,     ///< Startwert - es hat noch keine Fahrt gegeben
  ok,
  kein_magnet,        ///< in einer ganzen Umdrehung keine D0-Flanke
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
  int32_t pos_flanke = 0;     ///< Zaehlerstand im Augenblick der D0-Flanke
  int32_t nachlauf = 0;       ///< Weg vom Maximum bis zum Stillstand
  int32_t zaehler = 0;        ///< was der Zaehler danach zeigt
  float ruhe_v = 0.0f;        ///< gemessene Nulllinie
  float spanne_v = 0.0f;      ///< groesste Auslenkung gegen die Nulllinie
  int32_t fenster = 0;        ///< Breite des D0-Fensters in Schritten, 0 = unbekannt
  size_t punkte = 0;          ///< Messpunkte der Feinanfahrt
};

class Hall {
 public:
  /// Wird waehrend der Wartezeiten der Referenzfahrt aufgerufen. Damit bleibt
  /// die Konsole bedienbar, obwohl der Ablauf sequenziell geschrieben ist.
  /// Gibt der Rueckgabewert false, wird die Fahrt abgebrochen.
  using Pumpe = bool (*)();

  void beginn();

  /// Ist der Magnet gerade vor dem Sensor? ROHWERT, nicht entprellt -
  /// fuer Anzeigen. Die Flankenerkennung in tick() entprellt selbst.
  /// Polaritaet aus geraet.h, am Bauteil gemessen.
  bool magnet_erkannt() const;

  /// Der entprellte Zustand, wie ihn die Flankenerkennung sieht.
  bool magnet_stabil() const { return letzter_zustand_; }

  /// Liegt ein Feld an? Aus A0 gegen die Ruhelinie, OHNE Komparator und ohne
  /// Poti. Das ist die Groesse, mit der die Grobsuche arbeitet - und der
  /// Grund, warum ein Sensor ohne D0 (nackter SS49E, SS495A, DRV5055)
  /// genuegt.
  bool feld_erkannt() const;

  /// Rohspannung des SS49E in Volt.
  float spannung() const;

  /// Mehrfach gemessen und gemittelt.
  float spannung_gemittelt(uint8_t proben = 8) const;

  /// Median aus mehreren Mittelwerten - das ist der Wert, den man ANZEIGEN
  /// und fuer Spitzenwerte benutzen soll. Der ESP32-ADC liefert einzelne
  /// Ausreisser, und in einem Extremwert wie min/max bleibt so ein Ausreisser
  /// fuer immer stehen. Kostet rund 5 ms.
  float spannung_robust() const;

  /// Der letzte gefilterte Wert, ohne neu zu messen. Fuer Anzeigen, die oft
  /// abgefragt werden - etwa die Weboberflaeche im Sekundentakt.
  float spannung_gefiltert() const { return gefiltert_v_; }

  /// Spitzenwerte seit dem letzten Ruecksetzen - damit findet man beim Drehen
  /// von Hand den Ort des Maximums, ohne auf einen Zahlenwert zu starren.
  void spitzen_ruecksetzen();

  /// Flankenzahl und gemessene Fensterbreite zuruecksetzen.
  ///
  /// NICHT in spitzen_ruecksetzen() hineinlegen: das ruft die Referenzfahrt
  /// selbst, auch mitten drin. Nur fuer das Kommando 'c'.
  void zaehler_ruecksetzen();
  float spanne_v() const { return max_v_ - min_v_; }
  float max_v() const { return max_v_; }
  float min_v() const { return min_v_; }

  /// Aus der Hauptschleife aufrufen: fuehrt die Spitzenwerte und erkennt die
  /// D0-Flanken. `position` ist der Zaehlerstand des Antriebs.
  void tick(int32_t position);

  /// Zaehler der erkannten Flanken und die zuletzt gemessene Fensterbreite.
  uint32_t flanken() const { return flanken_; }
  int32_t fenster_schritte() const { return fenster_schritte_; }

  /// Die letzte gemessene Ruhelinie. Startwert aus geraet.h, ersetzt bei
  /// jeder Referenzfahrt durch eine Messung.
  float ruhe_v() const { return ruhe_v_; }

  // -------------------------------------------------------------------------
  // Die Referenzfahrt
  // -------------------------------------------------------------------------

  /// Zweistufig: Grobsuche bis zur D0-Flanke, Rueckzug, dann eine
  /// DURCHGEHENDE langsame Fahrt ueber das Fenster, dabei messen.
  ///
  /// `offset` ist der eingemessene Winkel zwischen Magnet und Klinge - jede
  /// gebaute Uhr hat ihren eigenen, er entsteht erst bei der Montage.
  ///
  /// Diese Funktion FAEHRT und kehrt erst zurueck, wenn sie fertig ist. Der
  /// Motor laeuft dabei in Hardware, das Warten kostet also keine
  /// Gleichmaessigkeit; `pumpe` haelt waehrenddessen die Konsole am Leben.
  Homingergebnis referenzfahrt(Antrieb &antrieb, int32_t offset, Pumpe pumpe);

 private:
  bool warte_bis_ziel(Antrieb &antrieb, uint32_t timeout_ms, Pumpe pumpe);

  float ruhe_v_ = 0.0f;
  float max_v_ = 0.0f;
  float min_v_ = 3.3f;
  uint32_t flanken_ = 0;
  bool letzter_zustand_ = false;
  bool roh_letzter_ = false;
  uint32_t roh_seit_ = 0;
  uint32_t letzte_analogmessung_ = 0;
  float gefiltert_v_ = 0.0f;
  int32_t pos_flanke_ = 0;
  int32_t fenster_schritte_ = 0;
  bool gesehen_ = false;
};

}  // namespace starwatch
