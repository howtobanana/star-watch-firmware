#include "antrieb.h"

#include <Arduino.h>
#include <geraet.h>
#include <zeiger.h>

namespace starwatch {

namespace g = geraet;

namespace {
// WELCHER UART. Der C3 hat nur ZWEI - Serial2 gibt es dort nicht. Serial1 legt die GPIO-Matrix
// auf IO20/IO21; dass das zugleich die Vorgabepins von UART0 sind, stoert
// nicht, weil `Serial` auf dem C3 das USB-CDC ist und UART0 unbenutzt bleibt.
HardwareSerial &tmc_seriell = Serial1;

// Statisch, nicht auf dem Heap: TMC2209Stepper haelt einen Zeiger auf den
// Stream, und der muss die Lebensdauer des Objekts ueberdauern.
TMC2209Stepper tmc_objekt(&tmc_seriell, g::R_SENSE, g::TMC_ADDR);
}  // namespace

const char *treiberzustand_text(Treiberzustand z) {
  switch (z) {
    case Treiberzustand::schweigt:         return "antwortet nicht";
    case Treiberzustand::ok:               return "ok";
    case Treiberzustand::war_spannungslos: return "war spannungslos - Register neu gesetzt";
    case Treiberzustand::zu_warm:          return "zu warm (Vorwarnung)";
    case Treiberzustand::uebertemperatur:  return "ABSCHALTUNG wegen Temperatur";
    case Treiberzustand::wicklung_offen:   return "Wicklung offen - Stecker?";
    case Treiberzustand::kurzschluss:      return "KURZSCHLUSS am Motorausgang";
    case Treiberzustand::fehler:           return "Treiberfehler";
  }
  return "unbekannt";
}

// ===========================================================================
// Aufbau
// ===========================================================================

bool Antrieb::beginn() {
  tmc_ = &tmc_objekt;

  tmc_seriell.begin(g::TMC_BAUD, SERIAL_8N1, g::PIN_UART_RX, g::PIN_UART_TX);

  // Register ZUERST, Endstufe danach. Siehe Kopf von antrieb.h.
  register_setzen(static_cast<uint16_t>(g::MIKROSCHRITTE));
  const bool antwortet = treiber_antwortet();

  engine_.init();
  motor_ = engine_.stepperConnectToPin(g::PIN_STEP);
  if (motor_ == nullptr)
    return false;  // GPIO nicht als Impulsausgang nutzbar

  // Zweiter Parameter: siehe DREHRICHTUNG in geraet.h. Ohne ihn dreht der
  // Zeiger gegen den Uhrzeigersinn, und das Blatt ist gespiegelt.
  motor_->setDirectionPin(g::PIN_DIR, g::DIR_HIGH_ZAEHLT_AUFWAERTS);
  motor_->setEnablePin(g::PIN_EN, true);  // true = LOW aktiviert (ENN)
  motor_->setAutoEnable(false);           // wir schalten selbst, kein Freilauf

  // Startwerte aus geraet.h - dort steht auch, warum Tempo, Rampe und
  // Chopper nur GEMEINSAM etwas bringen.
  rampe(g::BESCHLEUNIGUNG, g::BESCHLEUNIGUNG);
  tempo(g::TEMPO_FAHRT);

  endstufe(true);
  return antwortet;
}

void Antrieb::register_setzen(uint16_t mikroschritte) {
  if (tmc_ == nullptr)
    return;

  tmc_->begin();
  tmc_->I_scale_analog(false);   // Strom aus dem Register, NICHT vom Trimmpoti
  tmc_->pdn_disable(true);       // PDN ist UART, nicht Standby
  tmc_->mstep_reg_select(true);  // Mikroschritte aus MRES, nicht von MS1/MS2
  tmc_->toff(g::TOFF);
  tmc_->blank_time(24);
  tmc_->intpol(true);            // MicroPlyer
  tmc_->microsteps(mikroschritte);
  tmc_->rms_current(g::STROM_LAUF,
                    static_cast<float>(g::STROM_HALT) / g::STROM_LAUF);
  halt_ma_ = g::STROM_HALT;
  tmc_->iholddelay(6);
  tmc_->TPOWERDOWN(20);
  tmc_->en_spreadCycle(false);   // false = stealthChop ... im Stand und langsam
  tmc_->pwm_autoscale(true);     // stealthChop braucht die Regelung

  // ... ABER OBERHALB VON TPWMTHRS SPREADCYCLE. Ohne diese Zeile blieb der
  // Treiber immer in stealthChop (TPWMTHRS = 0 heisst "nie umschalten"), und
  // stealthChop verliert mit dem Tempo sein Drehmoment: ueber etwa 1 U/s
  // stand der Motor, waehrend der Impulszaehler ungeruehrt weiterzaehlte.
  // Genau diese Kombination - Zaehler laeuft, Welle nicht - ist die
  // gefaehrlichste, die dieser Aufbau kennt.
  //
  // Leise bleibt die Uhr trotzdem: der Uhrbetrieb fuehrt einen Mikroschritt
  // alle 13,5 s, also weit unter der Schwelle. Laut wird nur, was schnell
  // ist - Anfahrten, Timerlaeufe, Referenzfahrt.
  tmc_->TPWMTHRS(g::TPWMTHRS_WERT);
  tmc_->freewheel(0);            // Stillstand normal, NIE Freilauf
  delay(20);
}

uint8_t Antrieb::ic_version() {
  if (tmc_ == nullptr)
    return 0;
  return tmc_->version();
}

// ===========================================================================
// Endstufe
// ===========================================================================

void Antrieb::endstufe(bool ein) {
  if (motor_ == nullptr)
    return;
  if (ein)
    motor_->enableOutputs();
  else
    motor_->disableOutputs();
  if (endstufe_an_ && !ein)
    freilaeufe_++;
  endstufe_an_ = ein;
}

// ===========================================================================
// Fahren
// ===========================================================================

void Antrieb::tempo(uint32_t schritte_je_s) {
  if (motor_ == nullptr || schritte_je_s == 0)
    return;
  motor_->setSpeedInHz(schritte_je_s);
  tempo_ = schritte_je_s;
}

void Antrieb::rampe(uint32_t beschleunigung, uint32_t bremsung) {
  if (motor_ == nullptr)
    return;
  motor_->setAcceleration(beschleunigung);
  beschleunigung_ = beschleunigung;
  // FastAccelStepper kennt keine getrennte Bremsrampe; setAcceleration gilt
  // fuer beide Richtungen. Der Parameter steht in der Schnittstelle, weil der
  // Pruefstand ihn kennt und die Absicht erhalten bleiben soll.
  (void)bremsung;
}

bool Antrieb::tempo_mhz(uint32_t milli_hz) {
  if (motor_ == nullptr || milli_hz == 0)
    return false;
  if (motor_->setSpeedInMilliHz(milli_hz) != 0)
    return false;
  // Aufgerundet, damit tempo() nie 0 meldet - darauf teilt der Pruefstand.
  tempo_ = (milli_hz + 999u) / 1000u;
  // setSpeed allein wirkt erst mit dem naechsten moveTo. Ohne diese Zeile
  // liefe eine Fahrt mit dem alten Tempo weiter.
  if (motor_->isRunning())
    motor_->applySpeedAcceleration();
  return true;
}

void Antrieb::fahre_zu(int32_t position) {
  if (motor_ == nullptr)
    return;
  // Eine vorige WEICHE Fahrt hat Beschleunigung und Einsatz veraendert - fuer
  // diese Fahrt gilt wieder genau die eingestellte Rampe.
  motor_->setAcceleration(beschleunigung_);
  motor_->setLinearAcceleration(0);
  ziel_ = position;
  stopp_angefordert_ = false;
  motor_->moveTo(position);
}

void Antrieb::fahre_weiter(int32_t schritte) {
  if (motor_ == nullptr || schritte == 0)
    return;
  fahre_zu(ziel() + schritte);
}

void Antrieb::fahre_weich_zu(int32_t position) {
  if (motor_ == nullptr)
    return;
  const int32_t d = position - motor_->getCurrentPosition();
  const uint32_t weg = static_cast<uint32_t>(d < 0 ? -d : d);
  uint32_t a = weiche_beschleunigung(weg, beschleunigung_, g::WEICH_MIN_MS);
  // FAEHRT ER SCHON, muss er vor dem Ziel zum Stehen kommen: v^2/(2*weg),
  // mit einem Fuenftel Reserve. Sonst schoesse er mit der kleinen Rampe
  // eines kurzen Sprungs ueber das Ziel hinaus und kaeme zurueck - etwa
  // wenn am Ende einer schnellen Timer-Gleitfahrt die letzten Schritte
  // weich nachgesetzt werden.
  if (motor_->isRunning() && weg > 0) {
    const double v = static_cast<double>(motor_->getCurrentSpeedInMilliHz()) / 1000.0;
    const double brems = v * v / (2.0 * static_cast<double>(weg)) * 1.2 + 1.0;
    const uint32_t a_stopp =
        brems >= static_cast<double>(beschleunigung_) ? beschleunigung_
                                                      : static_cast<uint32_t>(brems);
    if (a_stopp > a)
      a = a_stopp;
  }
  motor_->setAcceleration(a);
  motor_->setLinearAcceleration(weicher_einsatz(weg, g::WEICH_EINSATZ_MAX));
  ziel_ = position;
  stopp_angefordert_ = false;
  motor_->moveTo(position);
}

void Antrieb::fahre_weich_weiter(int32_t schritte) {
  if (motor_ == nullptr || schritte == 0)
    return;
  fahre_weich_zu(ziel() + schritte);
}

int32_t Antrieb::position() const {
  if (motor_ == nullptr)
    return 0;
  return motor_->getCurrentPosition();
}

int32_t Antrieb::ziel() const {
  if (motor_ == nullptr)
    return 0;

  // Das Ziel wird SELBST mitgefuehrt und nicht aus dem Zustand des Steppers
  // abgeleitet. Der naheliegende Weg - "laeuft er nicht, ist die Istposition
  // das Ziel" - hat ein Rennen: zwischen dem Fahrbefehl und dem Anlaufen der
  // Rampe steht der Motor noch, und dann meldete ziel() die alte Position.
  // Der Uhrbetrieb rechnet mit diesem Wert und haette den Fahrbefehl damit
  // stillschweigend verworfen.
  //
  // Einzige Ausnahme ist der abgebrochene Lauf: nach stopp_mit_rampe() bleibt
  // der Zeiger irgendwo unterwegs stehen, und DANN ist die Istposition das
  // Ziel. Sonst wuerde die naechste Stellfahrt gegen ein Ziel rechnen, das
  // nie erreicht wurde.
  if (stopp_angefordert_ && !motor_->isRunning()) {
    ziel_ = motor_->getCurrentPosition();
    stopp_angefordert_ = false;
  }
  return ziel_;
}

bool Antrieb::laeuft() const {
  if (motor_ == nullptr)
    return false;
  return motor_->isRunning();
}

bool Antrieb::ziel_erreicht() const {
  if (motor_ == nullptr)
    return true;
  // Nicht nur "laeuft nicht": zwischen Fahrbefehl und Anlaufen der Rampe
  // laeuft er auch nicht, und dann waere jede Warteschleife sofort fertig.
  return !motor_->isRunning() && motor_->getCurrentPosition() == ziel();
}

void Antrieb::stopp_mit_rampe() {
  if (motor_ == nullptr)
    return;
  stopp_angefordert_ = true;
  // Mit der EINGESTELLTEN Rampe bremsen: nach einem kleinen weichen Sprung
  // stuende hier sonst eine Beschleunigung von ein paar hundert Schritten/s^2,
  // und eine schnelle Fahrt rollte sekundenlang aus.
  motor_->setAcceleration(beschleunigung_);
  motor_->setLinearAcceleration(0);
  if (motor_->isRunning())
    motor_->applySpeedAcceleration();
  motor_->stopMove();
}

bool Antrieb::zaehler_setzen(int32_t position) {
  if (motor_ == nullptr)
    return false;
  if (motor_->isRunning())
    return false;  // waehrend der Fahrt zaehlt die Hardware weiter
  motor_->setCurrentPosition(position);
  // Das Ziel mitziehen, sonst rechnet die naechste Stellfahrt gegen einen
  // Wert aus der Zeit vor dem Setzen.
  ziel_ = position;
  stopp_angefordert_ = false;
  motor_->moveTo(position);
  return true;
}

// ===========================================================================
// Diagnose
// ===========================================================================

uint16_t Antrieb::ma_aus_cs(uint8_t cs) {
  if (tmc_ == nullptr)
    return 0;
  const float vfs = tmc_->vsense() ? 0.180f : 0.325f;
  return static_cast<uint16_t>(
      lroundf((cs + 1) / 32.0f * vfs / (g::R_SENSE + 0.02f) * 0.70711f * 1000.0f));
}

void Antrieb::haltestrom(uint16_t ma) {
  if (ma < g::HALTESTROM_MIN)
    ma = g::HALTESTROM_MIN;
  if (ma > g::STROM_LAUF)
    ma = g::STROM_LAUF;
  halt_ma_ = ma;
  if (tmc_ == nullptr)
    return;
  // IHOLD ist ein Anteil von IRUN (32 Stufen, an C4E8 rund 15 mA je Stufe).
  tmc_->rms_current(g::STROM_LAUF, static_cast<float>(ma) / g::STROM_LAUF);
}

uint16_t Antrieb::strom_ma() {
  if (tmc_ == nullptr)
    return 0;
  return ma_aus_cs(tmc_->cs_actual());
}

uint16_t Antrieb::last() {
  if (tmc_ == nullptr)
    return 0;
  return tmc_->SG_RESULT();
}

uint32_t Antrieb::drv_status() {
  if (tmc_ == nullptr)
    return 0;
  return tmc_->DRV_STATUS();
}

uint16_t Antrieb::mikroschritte() {
  if (tmc_ == nullptr)
    return 0;
  return tmc_->microsteps();
}

uint16_t Antrieb::mscnt() {
  if (tmc_ == nullptr)
    return 0;
  return tmc_->MSCNT();
}

void Antrieb::spreadcycle(bool ein) {
  if (tmc_ == nullptr)
    return;
  tmc_->en_spreadCycle(ein);
}

Treiberzustand Antrieb::zustand() {
  if (tmc_ == nullptr || !treiber_antwortet())
    return Treiberzustand::schweigt;

  // GSTAT.reset ist ein Latch - ein Spannungseinbruch zwischen zwei Abfragen
  // geht damit nicht verloren. ABER: beim TMC2209 loescht LESEN das Flag
  // NICHT, es wird durch Zurueckschreiben einer 1 geloescht.
  //
  // AM GERAET GEFUNDEN (22.08.2026): ohne das Zurueckschreiben meldete jedes
  // 'd' bis in Ewigkeit "war spannungslos" - nach dem ersten Einschalten ist
  // das Flag naemlich immer gesetzt. Eine Meldung, die immer kommt, ist keine
  // Meldung mehr, und die echte Spannungsunterbrechung waere darin untergegangen.
  if (tmc_->reset()) {
    // Nach einem Reset stehen die Register auf WERKSEINSTELLUNG - also auch
    // I_scale_analog=1, und dann kommt der Strom wieder vom Trimmpoti (ab Werk
    // rund 0,9 A statt 0,4 A). Das darf nicht bis zum naechsten Fahrbefehl
    // warten, deshalb wird hier sofort nachgezogen. Frueher stand hier nur
    // eine Meldung, die das behauptete, ohne es zu tun.
    register_setzen(static_cast<uint16_t>(g::MIKROSCHRITTE));
    tmc_->GSTAT(0b111);  // alle drei Flags loeschen (reset, drv_err, uv_cp)
    return Treiberzustand::war_spannungslos;
  }

  const uint32_t s = tmc_->DRV_STATUS();
  // Bitlagen aus dem Datenblatt DRV_STATUS:
  //   0 otpw (Vorwarnung), 1 ot (Abschaltung), 2 s2ga, 3 s2gb,
  //   4 s2vsa, 5 s2vsb, 6 ola, 7 olb
  if (s & (1u << 1))
    return Treiberzustand::uebertemperatur;
  if (s & ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 5)))
    return Treiberzustand::kurzschluss;
  if (s & ((1u << 6) | (1u << 7)))
    return Treiberzustand::wicklung_offen;
  if (s & (1u << 0))
    return Treiberzustand::zu_warm;
  if (tmc_->drv_err())
    return Treiberzustand::fehler;
  return Treiberzustand::ok;
}

}  // namespace starwatch
