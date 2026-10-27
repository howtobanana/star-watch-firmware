#include "i2cbus.h"

#include <Arduino.h>
#include <Wire.h>
#include <geraet.h>

namespace starwatch {
namespace i2cbus {

namespace g = geraet;

namespace {
/// Gueltiger Adressbereich fuer 7-Bit-I2C. 0x00 bis 0x07 und 0x78 bis 0x7F
/// sind reserviert (General Call, 10-Bit-Adressierung und anderes); ein
/// Suchlauf darueber loest bei manchen Bausteinen Sonderfunktionen aus.
constexpr uint8_t ADR_ERSTE = 0x08;
constexpr uint8_t ADR_LETZTE = 0x77;
}  // namespace

bool beginn() {
  // Der Rueckgabewert von Wire.begin() sagt nur, ob der Treiber die Pins
  // belegen konnte - NICHT, ob ein Baustein antwortet. Dafuer ist der
  // Suchlauf da.
  if (!Wire.begin(g::PIN_SDA, g::PIN_SCL, g::I2C_HZ))
    return false;

  // Ein haengender Baustein kann SDA festhalten; dann steht der Bus, und
  // jeder Zugriff laeuft in den Zeitablauf. Eine kurze Frist ist besser als
  // eine Hauptschleife, die eine Sekunde je Versuch verliert.
  Wire.setTimeOut(50);
  return true;
}

bool pruefen(bool *sda_hoch, bool *scl_hoch, bool *sda_nach_takten) {
  Wire.end();
  pinMode(g::PIN_SDA, INPUT);
  pinMode(g::PIN_SCL, INPUT);
  delayMicroseconds(50);
  const bool sda = digitalRead(g::PIN_SDA) == HIGH;
  const bool scl = digitalRead(g::PIN_SCL) == HIGH;
  if (sda_hoch != nullptr) *sda_hoch = sda;
  if (scl_hoch != nullptr) *scl_hoch = scl;
  // Neun Takte: ein Baustein, der mitten in einem Byte haengt, schiebt sein
  // restliches Bit heraus und gibt SDA frei. Open-Drain nachgebildet - SCL
  // wird nur auf LOW gezogen, HIGH macht der Pull-up.
  if (scl && !sda) {
    for (int i = 0; i < 9; i++) {
      pinMode(g::PIN_SCL, OUTPUT);
      digitalWrite(g::PIN_SCL, LOW);
      delayMicroseconds(10);
      pinMode(g::PIN_SCL, INPUT);
      delayMicroseconds(10);
    }
  }
  if (sda_nach_takten != nullptr) *sda_nach_takten = digitalRead(g::PIN_SDA) == HIGH;
  return beginn();
}

bool vorhanden(uint8_t adresse) {
  Wire.beginTransmission(adresse);
  return Wire.endTransmission() == 0;
}

size_t absuchen(uint8_t *gefunden, size_t platz) {
  size_t n = 0;
  for (uint8_t a = ADR_ERSTE; a <= ADR_LETZTE; a++) {
    if (!vorhanden(a))
      continue;
    if (n < platz)
      gefunden[n] = a;
    n++;
  }
  return n;
}

const char *name(uint8_t adresse) {
  // Die drei Steckplaetze, ausgelesen aus dem Brett:
  // J5 = TMAG5273 (Hall), J7 = SCD41 (CO2), J8 = VEML7700 (Licht).
  if (adresse == g::TMAG_ADRESSE) return "TMAG5273 (Hall, J5)";
  if (adresse == g::LUX_ADRESSE) return "VEML7700 (Licht, J8)";
  if (adresse == 0x62) return "SCD41 (CO2, J7)";
  return nullptr;
}

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

bool byte_schreiben(uint8_t adresse, uint8_t reg, uint8_t wert) {
  Wire.beginTransmission(adresse);
  Wire.write(reg);
  Wire.write(wert);
  return Wire.endTransmission() == 0;
}

bool lesen(uint8_t adresse, uint8_t reg, uint8_t *puffer, size_t anzahl) {
  Wire.beginTransmission(adresse);
  Wire.write(reg);
  // false: KEIN Stopp vor dem Lesen. Der Baustein muss zwischen dem
  // Registerzeiger und den Daten am Bus bleiben - mit einem Stopp dazwischen
  // faengt mancher von vorn an und liefert Register 0.
  if (Wire.endTransmission(false) != 0)
    return false;

  const size_t bekommen = Wire.requestFrom(adresse, static_cast<uint8_t>(anzahl));
  if (bekommen != anzahl)
    return false;

  for (size_t i = 0; i < anzahl; i++)
    puffer[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool byte_lesen(uint8_t adresse, uint8_t reg, uint8_t *wert) {
  return lesen(adresse, reg, wert, 1);
}

bool wort_le_schreiben(uint8_t adresse, uint8_t reg, uint16_t wert) {
  Wire.beginTransmission(adresse);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(wert & 0xFF));
  Wire.write(static_cast<uint8_t>(wert >> 8));
  return Wire.endTransmission() == 0;
}

bool wort_le_lesen(uint8_t adresse, uint8_t reg, uint16_t *wert) {
  uint8_t b[2];
  if (!lesen(adresse, reg, b, 2))
    return false;
  *wert = static_cast<uint16_t>(b[0]) |
          (static_cast<uint16_t>(b[1]) << 8);
  return true;
}

}  // namespace i2cbus
}  // namespace starwatch
