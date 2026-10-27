// ---------------------------------------------------------------------------
// Star Watch - TMAG5273: der 3D-Hall-Sensor an J5, ueber I2C.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Den Sensor einrichten und drei Feldstaerken in Millitesla liefern. Was
// daraus wird - Grobsuche, Schwerpunkt, Referenzlage - ist Sache von
// lib/hall. Dieser Treiber kennt die Uhr nicht.
//
// ===========================================================================
// JEDE REGISTERZAHL HAT IHRE STELLE IM DATENBLATT
// ===========================================================================
// Grundlage ist TI SLYS045C (Juni 2021, ueberarbeitet April 2026),
// Abschnitt 8.1 "TMAG5273 Registers". Hinter jeder Konstante steht die
// Tabellennummer. Ohne Beleg kommt hier keine Zahl herein: ein falsch
// geratenes Bit in SENSOR_CONFIG_2 aendert den Messbereich um den Faktor
// zwei, und die Firmware rechnete dann jede Feldstaerke doppelt so gross,
// ohne dass irgendetwas auffiele.
//
// ===========================================================================
// WARUM DER BETRAG UND NICHT EINE ACHSE
// ===========================================================================
// Der Magnet sitzt AUSSERMITTIG auf der Scheibe und zieht einmal je
// Umdrehung am Sensor vorbei. Ein Absolutwinkel ist daraus nicht zu holen -
// dafuer muesste ein diametral magnetisierter Magnet mittig ueber dem Sensor
// sitzen. Was bleibt, ist die Feldkurve beim Vorbeiziehen, und deren Gipfel
// liegt dort, wo der Magnet dem Sensor gegenuebersteht.
//
// Der BETRAG sqrt(Bx^2+By^2+Bz^2) ist dafuer die richtige Groesse, nicht
// eine einzelne Achse:
//   - Er haengt nicht daran, welche Polflaeche vorbeizieht. Eine einzelne
//     Achse wechselt das Vorzeichen, wenn der Magnet umgedreht eingeklebt
//     ist; der Betrag nicht.
//   - Er haengt nicht daran, wie das Modul in seiner Aufnahme verdreht steckt.
//     Eine einzelne Achse tut das sehr wohl, und die Aufnahme gibt die
//     Drehlage nicht vor.
//   - Seine Nulllinie ist das Erdfeld, rund 0,05 mT, und nicht die halbe
//     Versorgungsspannung.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace starwatch {

/// Warum der Sensor nicht bereit ist.
enum class Tmaggrund {
  ok,
  nicht_gefunden,     ///< keine Antwort unter der Adresse
  falscher_hersteller,///< MANUFACTURER_ID passt nicht - da haengt etwas anderes
  falscher_bereich,   ///< die Variante passt nicht zu TMAG_BEREICH_MT
  schreiben_fehlt,    ///< Konfiguration liess sich nicht schreiben
  lesefehler,         ///< Bus antwortet nicht mehr
};

const char *tmaggrund_text(Tmaggrund g);

class Tmag5273 {
 public:
  /// Sensor suchen, Kennung pruefen, einrichten. Der Bus muss schon laufen
  /// (i2cbus::beginn()).
  ///
  /// Sucht der Sensor nicht unter der erwarteten Adresse, wird der ganze Bus
  /// abgesucht: der TMAG5273 laesst sich auf eine andere Adresse umstellen,
  /// und eine Firmware, die daran scheitert, schickt den Suchenden zur
  /// Verdrahtung statt zum Register.
  bool beginn();

  bool bereit() const { return bereit_; }
  Tmaggrund grund() const { return grund_; }

  /// Unter welcher Adresse er tatsaechlich antwortet.
  uint8_t adresse() const { return adresse_; }

  /// Variante laut DEVICE_ID Bit 1:0 (Tabelle 8-16).
  /// 1 = +/-40 und +/-80 mT (A1), 2 = +/-133 und +/-266 mT (A2).
  uint8_t version() const { return version_; }

  /// EINE Uebertragung, sechs Register, drei Achsen in Millitesla.
  ///
  /// Am Stueck gelesen, und das ist kein Geschmack: drei einzelne Zugriffe
  /// traefen drei verschiedene Wandlungen. Waehrend der Feinanfahrt dreht
  /// sich der Zeiger dabei weiter, und der Betrag entstuende aus Werten, die
  /// nicht zusammengehoeren.
  bool achsen_mt(float *bx, float *by, float *bz);

  /// Betrag des Feldvektors in Millitesla. Bei einem Busfehler 0 - und
  /// `bereit()` steht danach auf false, damit ein stiller Dauerfehler nicht
  /// als "kein Magnet" durchgeht.
  float betrag_mt();

 private:
  bool einrichten();

  bool bereit_ = false;
  Tmaggrund grund_ = Tmaggrund::nicht_gefunden;
  uint8_t adresse_ = 0;
  uint8_t version_ = 0;
};

}  // namespace starwatch
