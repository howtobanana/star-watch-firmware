#include "lux.h"

#include <Arduino.h>
#include <geraet.h>
#include <i2cbus.h>

namespace starwatch {

namespace g = geraet;

namespace {

// ===========================================================================
// Register. Vishay 84286 Rev. 1.8, "Command register format".
// ===========================================================================
constexpr uint8_t REG_ALS_CONF_0 = 0x00;
constexpr uint8_t REG_POWER_SAVING = 0x03;
constexpr uint8_t REG_ALS = 0x04;
constexpr uint8_t REG_ID = 0x07;

/// Tabelle 8 "Device ID #7": die unteren acht Bit sind die feste
/// Bauteilkennung 0x81. Die oberen acht haengen an der Adressvariante und
/// werden bewusst NICHT geprueft - sie unterscheiden nur 0x10 von 0x48.
constexpr uint8_t ID_VEML7700 = 0x81;

/// ALS_CONF_0, Tabelle 1. Bitlagen, nicht Werte.
constexpr uint8_t BIT_ALS_SD = 0;       ///< 1 = abgeschaltet
constexpr uint8_t BIT_ALS_INT_EN = 1;
constexpr uint8_t BIT_ALS_IT = 6;       ///< 9:6
constexpr uint8_t BIT_ALS_GAIN = 11;    ///< 12:11

/// Verstaerkungs-Schluessel (Tabelle 1, ALS_GAIN 12:11).
constexpr uint8_t GAIN_X1 = 0x0;
constexpr uint8_t GAIN_X2 = 0x1;
constexpr uint8_t GAIN_X1_8 = 0x2;
constexpr uint8_t GAIN_X1_4 = 0x3;

/// Integrationszeit-Schluessel (Tabelle 1, ALS_IT 9:6). NICHT fortlaufend -
/// 25 ms ist 1100b und 100 ms ist 0000b. Wer hier durchzaehlt, stellt etwas
/// anderes ein als er glaubt.
constexpr uint8_t IT_25 = 0xC;
constexpr uint8_t IT_50 = 0x8;
constexpr uint8_t IT_100 = 0x0;
constexpr uint8_t IT_200 = 0x1;
constexpr uint8_t IT_400 = 0x2;
constexpr uint8_t IT_800 = 0x3;

/// Eine Stufe der Empfindlichkeitsleiter.
struct Stufe {
  uint8_t gain_code;
  uint8_t it_code;
  float gain;        ///< Verstaerkungsfaktor als Zahl
  uint16_t it_ms;
};

/// Die Leiter, von UNEMPFINDLICH nach EMPFINDLICH.
///
/// Acht Stufen decken zusammen 0,0042 bis 2,15 lx je Zaehlschritt ab, also
/// den ganzen Weg vom Vollmond bis zum Sonnentag. Jede Stufe ist halb so
/// empfindlich wie die naechste - damit springt der Messwert beim Umschalten
/// um genau den Faktor zwei und nicht um einen krummen Betrag.
constexpr Stufe LEITER[] = {
    {GAIN_X1_8, IT_25,  0.125f,  25},   // bis rund 141 000 lx
    {GAIN_X1_8, IT_50,  0.125f,  50},
    {GAIN_X1_8, IT_100, 0.125f, 100},
    {GAIN_X1_4, IT_100, 0.25f,  100},
    {GAIN_X1,   IT_100, 1.0f,   100},
    {GAIN_X2,   IT_100, 2.0f,   100},
    {GAIN_X2,   IT_200, 2.0f,   200},
    {GAIN_X2,   IT_800, 2.0f,   800},   // 0,0042 lx je Schritt
};
constexpr uint8_t STUFEN = sizeof(LEITER) / sizeof(LEITER[0]);

/// Startstufe: die MITTE der Leiter, nicht ein Ende.
///
/// Von ganz unten braeuchte ein dunkles Zimmer sieben Messtakte, bis die
/// Nachfuehrung oben ankommt - bei 2 s Takt also vierzehn Sekunden, in denen
/// die Uhr nicht weiss, wie hell es ist. Aus der Mitte sind es hoechstens
/// vier in jede Richtung.
constexpr uint8_t START_STUFE = 4;

/// Wann nachgefuehrt wird.
///
/// Nicht erst bei 0 und 65535: ein Zaehlwert unter 1000 hat weniger als zehn
/// Bit Aufloesung, und ueber 30000 ist die naechste Helligkeitsaenderung
/// womoeglich schon im Anschlag. Die beiden Schwellen liegen um den Faktor
/// 30 auseinander, die Stufen aber nur um den Faktor 2 - damit kann die
/// Nachfuehrung nicht zwischen zwei Stufen hin und her pendeln.
constexpr uint16_t ROH_ZU_KLEIN = 1000;
constexpr uint16_t ROH_ZU_GROSS = 30000;

}  // namespace

const char *luxgrund_text(Luxgrund grund) {
  switch (grund) {
    case Luxgrund::ok:              return "OK";
    case Luxgrund::nicht_gefunden:  return "kein VEML7700 an J8";
    case Luxgrund::falsche_kennung: return "an 0x10 antwortet kein VEML7700";
    case Luxgrund::schreiben_fehlt: return "Konfiguration nicht schreibbar";
    case Luxgrund::lesefehler:      return "Bus antwortet nicht";
  }
  return "unbekannt";
}

float Lux::verstaerkung() const { return LEITER[stufe_].gain; }
uint16_t Lux::integrationszeit_ms() const { return LEITER[stufe_].it_ms; }

float Lux::aufloesung() const {
  // Vishay 84286: bei x2 und 800 ms sind es 0,0042 lx je Zaehlschritt. Die
  // Aufloesung verdoppelt sich mit jeder Halbierung von Zeit ODER
  // Verstaerkung - also ist sie umgekehrt proportional zu beidem.
  //
  // PROBE gegen das Datenblatt: x1/8 und 25 ms geben
  // 0,0042 * (800/25) * (2/0,125) = 2,1504 lx/Schritt, mal 65535 Schritte
  // = 140 923 lx. Das Datenblatt nennt 140 000 lx als groesste messbare
  // Beleuchtungsstaerke. Die Formel trifft also die Tabelle, sie ist nicht
  // daneben geraten. Dieselbe Probe steht als Host-Test in test_geraet.
  const Stufe &st = LEITER[stufe_];
  return g::LUX_AUFLOESUNG_BASIS * (800.0f / static_cast<float>(st.it_ms)) *
         (2.0f / st.gain);
}

bool Lux::beginn() {
  bereit_ = false;
  gemessen_ = false;

  if (!i2cbus::vorhanden(g::LUX_ADRESSE)) {
    grund_ = Luxgrund::nicht_gefunden;
    return false;
  }

  uint16_t id = 0;
  if (!i2cbus::wort_le_lesen(g::LUX_ADRESSE, REG_ID, &id)) {
    grund_ = Luxgrund::lesefehler;
    return false;
  }
  if ((id & 0x00FF) != ID_VEML7700) {
    grund_ = Luxgrund::falsche_kennung;
    return false;
  }

  // Stromsparbetrieb aus. Er verlaengert die Zeit zwischen zwei Messungen
  // auf bis zu 4,1 s (Tabelle "Refresh time") - das ist mehr als der
  // Messtakt dieser Firmware, und die Uhr laese dann immer denselben Wert
  // zweimal. Sparen waere hier ohnehin sinnlos: die Uhr haengt am Netzteil.
  if (!i2cbus::wort_le_schreiben(g::LUX_ADRESSE, REG_POWER_SAVING, 0x0000)) {
    grund_ = Luxgrund::schreiben_fehlt;
    return false;
  }

  if (!stufe_schreiben(START_STUFE)) {
    grund_ = Luxgrund::schreiben_fehlt;
    return false;
  }

  grund_ = Luxgrund::ok;
  bereit_ = true;
  return true;
}

bool Lux::stufe_schreiben(uint8_t stufe) {
  if (stufe >= STUFEN)
    return false;
  const Stufe &st = LEITER[stufe];

  // ALS_SD auf 0 heisst EINGESCHALTET (Tabelle 1). Das ist herum wie bei
  // einer Sicherung, nicht wie bei einem Schalter - wer hier eine 1
  // hineinschreibt, schaltet den Sensor ab und misst danach ewig denselben
  // Wert.
  const uint16_t cfg =
      static_cast<uint16_t>(static_cast<uint16_t>(st.gain_code) << BIT_ALS_GAIN) |
      static_cast<uint16_t>(static_cast<uint16_t>(st.it_code) << BIT_ALS_IT) |
      static_cast<uint16_t>(0u << BIT_ALS_INT_EN) |
      static_cast<uint16_t>(0u << BIT_ALS_SD);

  if (!i2cbus::wort_le_schreiben(g::LUX_ADRESSE, REG_ALS_CONF_0, cfg))
    return false;

  stufe_ = stufe;

  // NACH EINER UMSTELLUNG IST DER NAECHSTE WERT NOCH DER ALTE. Der Sensor
  // integriert gerade - das laufende Ergebnis stammt aus der Einstellung von
  // vorher und wuerde mit der neuen Aufloesung umgerechnet. Daraus entstuende
  // ein Sprung um den Faktor zwei, und die Nachfuehrung stellte daraufhin
  // wieder zurueck: ein Pendeln, das nur beim Messen mit der Hand vor dem
  // Sensor auffaellt.
  //
  // Zwei Integrationszeiten warten deckt den Fall sicher ab.
  gueltig_ab_ = millis() + 2u * static_cast<uint32_t>(st.it_ms);
  return true;
}

void Lux::nachfuehren() {
  if (roh_ > ROH_ZU_GROSS && stufe_ > 0) {
    stufe_schreiben(static_cast<uint8_t>(stufe_ - 1));
    return;
  }
  if (roh_ < ROH_ZU_KLEIN && stufe_ + 1 < STUFEN) {
    stufe_schreiben(static_cast<uint8_t>(stufe_ + 1));
    return;
  }
  // Sonst passt die Stufe. Am oberen und unteren Ende der Leiter bleibt sie
  // stehen, auch wenn der Zaehlwert ausserhalb liegt - mehr kann das
  // Bauteil nicht.
}

void Lux::tick() {
  if (!bereit_)
    return;

  const uint32_t jetzt = millis();
  if (jetzt - letzte_messung_ < g::LUX_TAKT_MS)
    return;
  letzte_messung_ = jetzt;

  uint16_t roh = 0;
  if (!i2cbus::wort_le_lesen(g::LUX_ADRESSE, REG_ALS, &roh)) {
    // Ein Busfehler heisst: Stecker, Ader oder Modul. Der Sensor gilt danach
    // als nicht bereit, und die Regelung faellt auf den von Hand
    // eingestellten Wert zurueck - lieber das als ein eingefrorener Lux-Wert,
    // nach dem die Klinge weiter geregelt wird.
    bereit_ = false;
    grund_ = Luxgrund::lesefehler;
    return;
  }

  roh_ = roh;

  // Noch in der Einschwingzeit nach einer Umstellung? Dann ist dieser
  // Zaehlwert noch mit der ALTEN Einstellung entstanden - und taugt weder
  // als Messergebnis noch als Grundlage fuer die naechste Nachfuehrung.
  //
  // Das zweite ist der Punkt, und er ist beim Durchdenken aufgefallen:
  // waere hier nur die Umrechnung uebersprungen, die Nachfuehrung aber
  // nicht, dann sieht sie nach einem Schritt nach unten immer noch den
  // alten, doppelt so grossen Zaehlwert - und geht gleich noch einen
  // Schritt weiter. Die Leiter liefe jedes Mal eine Stufe zu weit und
  // pendelte sich erst danach ein.
  if (static_cast<int32_t>(jetzt - gueltig_ab_) < 0)
    return;

  lux_ = static_cast<float>(roh_) * aufloesung();
  gemessen_ = true;

  nachfuehren();
}

}  // namespace starwatch
