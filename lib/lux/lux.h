// ---------------------------------------------------------------------------
// Star Watch - lib/lux: der Umgebungslichtsensor an J8.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Den VEML7700 lesen und EINE Zahl liefern: die Beleuchtungsstaerke in Lux.
// Was die Uhr damit macht - dimmen, nachts abschalten - steht in lib/zeiger
// (licht_anteil, Nachtschalter) und wird in main.cpp angewandt. Dieser
// Treiber kennt die Klinge nicht.
//
// ===========================================================================
// DER SENSOR IST OPTIONAL
// ===========================================================================
// J8 ist ein Steckplatz, kein Bauteil. Steckt dort nichts, meldet `beginn()`
// das einmal und die Uhr laeuft ohne ihn weiter - mit der von Hand
// eingestellten Helligkeit und ohne Nachtabschaltung. Jede Funktion hier
// muss den Fall "nicht da" vertragen, ohne die Hauptschleife aufzuhalten.
//
// ===========================================================================
// WARUM DIE EMPFINDLICHKEIT SELBST NACHGEFUEHRT WIRD
// ===========================================================================
// Der VEML7700 hat 16 Bit, aber sein Messbereich haengt an zwei
// Einstellungen: Verstaerkung (x1/8 bis x2) und Integrationszeit (25 bis
// 800 ms). Fest eingestellt deckt er entweder das dunkle Zimmer ODER den
// Sonnentag ab - nicht beides:
//
//   x2 und 800 ms  ->  0,0042 lx je Zaehlschritt, aber bei 275 lx am Anschlag
//   x1/8 und 25 ms ->  bis 140 000 lx, aber 2,15 lx je Zaehlschritt
//
// Diese Uhr braucht beides. Unten am Ende der Skala entscheidet sich die
// Nachtabschaltung (0,5 lx - bei 2,15 lx Aufloesung waere das ein einziger
// Zaehlschritt), oben soll das Tageslicht nicht in die Saettigung laufen.
// Also wird die Einstellung nachgefuehrt, eine Stufe je Messtakt.
//
// Zahlen und Bitlagen: Vishay 84286 Rev. 1.8 (28.11.2024), Tabelle 1
// "Configuration register #0" und die Tabelle "Refresh time, IDD and
// resolution relation".
//
// ===========================================================================
// WAS DIESER TREIBER NICHT TUT
// ===========================================================================
// Er rechnet die Nichtlinearitaet bei hohen Beleuchtungsstaerken NICHT
// heraus. Vishay gibt dafuer im Anwendungsbericht ein Korrekturpolynom an;
// es wirkt oberhalb von rund 1000 lx und laesst den Sensor dort bis zu
// 45 % zu wenig anzeigen. Fuer diese Uhr ist das ohne Bedeutung: geregelt
// wird zwischen 1 und 300 lx, und alles darueber heisst ohnehin nur "hell,
// volle Helligkeit". Ein Polynom, das nirgends gebraucht wird, waere eine
// Zahlenreihe ohne Probe.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace starwatch {

/// Warum der Sensor nicht bereit ist.
enum class Luxgrund {
  ok,
  nicht_gefunden,   ///< keine Antwort unter 0x10 - Modul steckt nicht
  falsche_kennung,  ///< dort antwortet etwas, aber kein VEML7700
  schreiben_fehlt,  ///< Konfiguration liess sich nicht schreiben
  lesefehler,       ///< Bus antwortet nicht mehr
};

const char *luxgrund_text(Luxgrund g);

class Lux {
 public:
  /// Sensor suchen, Kennung pruefen, einschalten. Der I2C-Bus muss laufen.
  /// false heisst: kein Modul an J8. Das ist KEIN Fehler der Uhr.
  bool beginn();

  bool bereit() const { return bereit_; }
  Luxgrund grund() const { return grund_; }

  /// Aus der Hauptschleife aufrufen. Misst hoechstens alle LUX_TAKT_MS und
  /// fuehrt dabei die Empfindlichkeit nach.
  void tick();

  /// Beleuchtungsstaerke in Lux. Solange noch nichts gemessen wurde: 0.
  float lux() const { return lux_; }

  /// Hat es ueberhaupt schon eine gueltige Messung gegeben? Wichtig fuer die
  /// Regelung: 0 lx heisst "stockdunkel" und wuerde die Klinge abschalten -
  /// ein noch nicht gemessener Wert darf das nicht ausloesen.
  bool gemessen() const { return gemessen_; }

  // --- Fuer Anzeigen und fuer das Einmessen -------------------------------

  uint16_t rohwert() const { return roh_; }
  float verstaerkung() const;              ///< 0,125 / 0,25 / 1 / 2
  uint16_t integrationszeit_ms() const;
  float aufloesung() const;                ///< Lux je Zaehlschritt
  uint8_t stufe() const { return stufe_; }  ///< Platz auf der Empfindlichkeitsleiter

 private:
  bool stufe_schreiben(uint8_t stufe);
  void nachfuehren();

  bool bereit_ = false;
  bool gemessen_ = false;
  Luxgrund grund_ = Luxgrund::nicht_gefunden;

  uint16_t roh_ = 0;
  float lux_ = 0.0f;
  uint8_t stufe_ = 0;

  uint32_t letzte_messung_ = 0;
  uint32_t gueltig_ab_ = 0;  ///< vor diesem Zeitpunkt ist der Wert noch alt
};

}  // namespace starwatch
