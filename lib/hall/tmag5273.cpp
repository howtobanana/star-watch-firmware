#include "tmag5273.h"

#include <Arduino.h>
#include <geraet.h>
#include <i2cbus.h>

namespace starwatch {

namespace g = geraet;

namespace {

// ===========================================================================
// Register. TI SLYS045C Tabelle 8-1 "TMAG5273 Registers".
// ===========================================================================
constexpr uint8_t REG_DEVICE_CONFIG_1 = 0x00;
constexpr uint8_t REG_DEVICE_CONFIG_2 = 0x01;
constexpr uint8_t REG_SENSOR_CONFIG_1 = 0x02;
constexpr uint8_t REG_SENSOR_CONFIG_2 = 0x03;
constexpr uint8_t REG_DEVICE_ID = 0x0D;
constexpr uint8_t REG_MANUFACTURER_ID_LSB = 0x0E;
constexpr uint8_t REG_X_MSB_RESULT = 0x12;

/// Herstellerkennung. Tabelle 8-17 und 8-18: LSB = 49h, MSB = 54h.
/// Das ist "TI" in ASCII, rueckwaerts gelesen - und der einzige Weg, einen
/// falsch gesteckten Baustein von einem TMAG5273 zu unterscheiden, bevor
/// Zahlen herauskommen, die plausibel aussehen und es nicht sind.
constexpr uint8_t HERSTELLER_LSB = 0x49;
constexpr uint8_t HERSTELLER_MSB = 0x54;

/// DEVICE_ID Bit 1:0 (Tabelle 8-16).
constexpr uint8_t VER_40_80_MT = 0x1;    ///< TMAG5273A1 und Geschwister
constexpr uint8_t VER_133_266_MT = 0x2;  ///< TMAG5273A2 und Geschwister

/// SENSOR_CONFIG_1 Bit 7:4 (Tabelle 8-5): 7h = X, Y und Z eingeschaltet.
/// Der Reset-Wert ist 0h - ALLE Kanaele AUS. Ohne dieses Register bleibt der
/// Sensor also stumm und liefert Nullen, die wie ein fehlendes Feld aussehen.
constexpr uint8_t MAG_CH_EN_XYZ = 0x7;

/// DEVICE_CONFIG_2 Bit 1:0 (Tabelle 8-4): 2h = Continuous measure mode.
constexpr uint8_t BETRIEB_DAUERMESSUNG = 0x2;

/// DEVICE_CONFIG_2 Bit 4 (Tabelle 8-4): 1h = low-noise mode.
///
/// Der andere Wert waere "low active current". Diese Uhr haengt an einem
/// 12-V-Netzteil, ihr Schrittmotor zieht 0,4 A - ein paar Milliampere am
/// Hall-Sensor sind hier ohne Bedeutung, ein ruhigeres Signal in der
/// Feinanfahrt dagegen nicht.
constexpr uint8_t RAUSCHARM = 1;

/// Umrechnung eines Ergebnisregisters in Millitesla.
///
/// Datenblatt Gleichung 10: B = (Zweierkomplement / 2^16) * 2 * BR, also
/// int16 / 32768 * BR. BR ist der Messbereich der Achse in mT.
constexpr float LSB_JE_MT = 32768.0f;

float zu_mt(int16_t roh) {
  return (static_cast<float>(roh) / LSB_JE_MT) * g::TMAG_BEREICH_MT;
}

}  // namespace

const char *tmaggrund_text(Tmaggrund grund) {
  switch (grund) {
    case Tmaggrund::ok:                  return "OK";
    case Tmaggrund::nicht_gefunden:      return "kein TMAG5273 am Bus";
    case Tmaggrund::falscher_hersteller: return "Herstellerkennung passt nicht";
    case Tmaggrund::falscher_bereich:    return "Variante passt nicht zu TMAG_BEREICH_MT";
    case Tmaggrund::schreiben_fehlt:     return "Konfiguration nicht schreibbar";
    case Tmaggrund::lesefehler:          return "Bus antwortet nicht";
  }
  return "unbekannt";
}

bool Tmag5273::beginn() {
  bereit_ = false;
  adresse_ = 0;

  // 1. Finden. Zuerst dort, wo er laut Datenblatt sitzt.
  if (i2cbus::vorhanden(g::TMAG_ADRESSE)) {
    adresse_ = g::TMAG_ADRESSE;
  } else {
    // Sonst den Bus absuchen. Die Adresse ist umstellbar (Tabelle 8-15), und
    // wer sie einmal umgestellt hat, sucht sonst am falschen Ende.
    uint8_t gefunden[8];
    const size_t n = i2cbus::absuchen(gefunden, sizeof(gefunden));
    for (size_t i = 0; i < n && i < sizeof(gefunden); i++) {
      uint8_t b[2];
      if (!i2cbus::lesen(gefunden[i], REG_MANUFACTURER_ID_LSB, b, 2))
        continue;
      if (b[0] == HERSTELLER_LSB && b[1] == HERSTELLER_MSB) {
        adresse_ = gefunden[i];
        break;
      }
    }
  }

  if (adresse_ == 0) {
    grund_ = Tmaggrund::nicht_gefunden;
    return false;
  }

  // 2. Kennung pruefen, BEVOR irgendetwas geschrieben wird. Unter 0x35 kann
  // auch etwas anderes antworten; hineinzuschreiben waere dann ein Eingriff
  // in ein fremdes Bauteil.
  uint8_t hersteller[2];
  if (!i2cbus::lesen(adresse_, REG_MANUFACTURER_ID_LSB, hersteller, 2)) {
    grund_ = Tmaggrund::lesefehler;
    return false;
  }
  if (hersteller[0] != HERSTELLER_LSB || hersteller[1] != HERSTELLER_MSB) {
    grund_ = Tmaggrund::falscher_hersteller;
    return false;
  }

  uint8_t id = 0;
  if (!i2cbus::byte_lesen(adresse_, REG_DEVICE_ID, &id)) {
    grund_ = Tmaggrund::lesefehler;
    return false;
  }
  version_ = id & 0x03;

  // 3. Passt die Variante zu dem Messbereich, mit dem geraet.h rechnet?
  //
  // DAS IST DER WICHTIGSTE PUNKT DIESER FUNKTION. Eine A1 liefert bei
  // demselben Feld dieselben Rohwerte wie eine A2 - nur bedeuten sie ein
  // Drittel. Ohne diese Pruefung rechnete die Firmware jede Feldstaerke um
  // den Faktor 3,3 falsch, die Aussteuerung laege scheinbar im Fenster, und
  // die Referenzfahrt meldete "OK".
  const bool passt =
      (version_ == VER_40_80_MT &&
       (g::TMAG_BEREICH_MT == 40.0f || g::TMAG_BEREICH_MT == 80.0f)) ||
      (version_ == VER_133_266_MT &&
       (g::TMAG_BEREICH_MT == 133.0f || g::TMAG_BEREICH_MT == 266.0f));
  if (!passt) {
    grund_ = Tmaggrund::falscher_bereich;
    return false;
  }

  if (!einrichten()) {
    grund_ = Tmaggrund::schreiben_fehlt;
    return false;
  }

  grund_ = Tmaggrund::ok;
  bereit_ = true;
  return true;
}

bool Tmag5273::einrichten() {
  // DEVICE_CONFIG_1 (Tabelle 8-3): CRC aus, keine Magnettemperaturkorrektur,
  // Mittelung aus geraet.h, Standard-Lesebefehl.
  //
  // MAG_TEMPCO bleibt auf 0, obwohl der Magnet ein NdFeB ist und das
  // Datenblatt dafuer 1h anbietet: die Korrektur zoege die ANZEIGE der
  // Feldstaerke gerade, aber die Referenzfahrt sucht einen GIPFEL. Wo der
  // liegt, aendert eine gleichmaessige Verstaerkung ueber die ganze Kurve
  // nicht - und was nichts bewirkt, gehoert nicht eingeschaltet.
  const uint8_t cfg1 = static_cast<uint8_t>((g::TMAG_CONV_AVG & 0x07) << 2);
  if (!i2cbus::byte_schreiben(adresse_, REG_DEVICE_CONFIG_1, cfg1))
    return false;

  // SENSOR_CONFIG_2 (Tabelle 8-6): kein Winkel, keine Schwellen,
  // X_Y_RANGE und Z_RANGE beide auf den KLEINEREN Bereich der Variante.
  //
  // Kleiner heisst hier feiner: bei der A2 sind das +/-133 statt +/-266 mT,
  // also 4,06 statt 8,12 uT je Zaehlschritt. Der groessere Bereich brauchte
  // einen Magneten, der den kleineren uebersteuert - und den wuerde
  // HALL_SPANNE_MAX_MT ohnehin als zu nah melden.
  const uint8_t grosser_bereich =
      (g::TMAG_BEREICH_MT == 80.0f || g::TMAG_BEREICH_MT == 266.0f) ? 1 : 0;
  const uint8_t cfg_sensor2 =
      static_cast<uint8_t>((grosser_bereich << 1) | grosser_bereich);
  if (!i2cbus::byte_schreiben(adresse_, REG_SENSOR_CONFIG_2, cfg_sensor2))
    return false;

  // SENSOR_CONFIG_1 (Tabelle 8-5): alle drei Achsen einschalten.
  const uint8_t cfg_sensor1 = static_cast<uint8_t>(MAG_CH_EN_XYZ << 4);
  if (!i2cbus::byte_schreiben(adresse_, REG_SENSOR_CONFIG_1, cfg_sensor1))
    return false;

  // DEVICE_CONFIG_2 (Tabelle 8-4) ZULETZT: erst jetzt faengt der Sensor an
  // zu wandeln, und zwar mit allem, was oben schon steht. Umgekehrt liefe
  // die erste Wandlung noch mit den Reset-Werten - also ohne Kanaele.
  const uint8_t cfg2 =
      static_cast<uint8_t>((RAUSCHARM << 4) | BETRIEB_DAUERMESSUNG);
  if (!i2cbus::byte_schreiben(adresse_, REG_DEVICE_CONFIG_2, cfg2))
    return false;

  return true;
}

bool Tmag5273::achsen_mt(float *bx, float *by, float *bz) {
  if (adresse_ == 0)
    return false;

  // Sechs Register am Stueck: X_MSB, X_LSB, Y_MSB, Y_LSB, Z_MSB, Z_LSB
  // (Tabelle 8-1, Offsets 12h bis 17h). Der Baustein zaehlt den
  // Registerzeiger von selbst weiter.
  uint8_t b[6];
  if (!i2cbus::lesen(adresse_, REG_X_MSB_RESULT, b, 6)) {
    bereit_ = false;
    grund_ = Tmaggrund::lesefehler;
    return false;
  }

  const int16_t x = static_cast<int16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
  const int16_t y = static_cast<int16_t>((static_cast<uint16_t>(b[2]) << 8) | b[3]);
  const int16_t z = static_cast<int16_t>((static_cast<uint16_t>(b[4]) << 8) | b[5]);

  if (bx != nullptr) *bx = zu_mt(x);
  if (by != nullptr) *by = zu_mt(y);
  if (bz != nullptr) *bz = zu_mt(z);
  return true;
}

float Tmag5273::betrag_mt() {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  if (!achsen_mt(&x, &y, &z))
    return 0.0f;
  return sqrtf(x * x + y * y + z * z);
}

}  // namespace starwatch
