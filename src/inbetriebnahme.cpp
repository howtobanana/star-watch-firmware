// ---------------------------------------------------------------------------
// Star Watch - Inbetriebnahme-Sketch. Beantwortet Abschnitt 14 des Briefings.
// Messgeraet, kein Produkt: serielles Menue ueber USB, 115200 Baud. Kein WLAN,
// kein Webserver, kein Homing, keine Zeitrechnung. Ausgaben ohne Umlaute.
// ERSTER LAUF OHNE RIEMEN UND OHNE KLINGE. Rampen trotzdem immer fahren.
// VOR DEM EINSCHALTEN: Trimmpoti des TMC2209 auf Vref 0,55 V. Solange die
// Registerschreibzugriffe nicht ankommen, bestimmt allein das Poti den Strom,
// und ab Werk steht es auf 1,2 V = rund 0,9 A statt der geplanten 0,4 A.
// Schritte ueber FastAccelStepper (STEP/DIR in Hardware), UART nur fuer die
// Register - kein VACTUAL. Der Treiber ist bestromt (Haltestrom, nie 0).
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <TMCStepper.h>
#include <FastAccelStepper.h>

#include <geraet.h>

// --- Pins, Treiber- und Antriebsdaten -------------------------------------
// SEIT AP 2.1 (22.08.2026) KOMMEN SIE AUS lib/geraet/geraet.h und stehen
// nicht mehr in dieser Datei. Der Grund ist ein Fehler, den genau diese
// Doppelfuehrung erzeugt hat: der Sketch trug bis heute TMC_ADDR = 0x03 und
// D0_AKTIV = LOW, beides ist am 21./22.08.2026 am Geraet widerlegt worden -
// die Adresse ist 0x00 (MS1/MS2 unbeschaltet, interne Pull-downs), und D0
// liegt bei erkanntem Magnet HIGH. Wer mit den alten Werten gemessen haette,
// waere in Menue 1 an einem schweigenden Treiber gescheitert und haette in
// Menue 4 das Sensorfenster als "alles ausser dem Magneten" gesehen.
//
// UART ist EIN Draht: TX ueber 1 kOhm, RX direkt, beide auf DASSELBE PDN-Pad
// (Pin 4, das vierte von EN). Pin 5 ist ein Alternativpad und haengt in der
// Luft. Am verbauten Modul - einer V2.0, nicht der BTT V1.2 - ist das
// unbestaetigt; die Angaben zu den PDN-Pads stammen aus dem V1.2-Handbuch.
namespace g = starwatch::geraet;

constexpr uint8_t PIN_UART_TX = g::PIN_UART_TX;
constexpr uint8_t PIN_UART_RX = g::PIN_UART_RX;
constexpr uint8_t PIN_INDEX   = g::PIN_INDEX;
constexpr uint8_t PIN_STEP    = g::PIN_STEP;
constexpr uint8_t PIN_DIR     = g::PIN_DIR;
constexpr uint8_t PIN_EN      = g::PIN_EN;
constexpr uint8_t PIN_HALL_D0 = g::PIN_HALL_D0;
constexpr uint8_t PIN_HALL_A0 = g::PIN_HALL_A0;

// GEDREHT AM 21.08.2026 AM GERAET, gegen die Theorie. Vorher stand hier LOW,
// mit der Begruendung "das Modul zieht D0 beim Ansprechen nach LOW". Am
// verbauten KY-024 ist es umgekehrt; im Pruefstand steht deshalb
// `inverted: false`. Massgeblich ist das Bauteil, nicht das Datenblatt.
constexpr int D0_AKTIV = g::D0_AKTIV_HIGH ? HIGH : LOW;

constexpr float    R_SENSE      = g::R_SENSE;
constexpr uint8_t  TMC_ADDR     = g::TMC_ADDR;
constexpr uint32_t TMC_BAUD     = g::TMC_BAUD;
constexpr uint16_t STROM_LAUF   = g::STROM_LAUF;
constexpr uint16_t STROM_HALT   = g::STROM_HALT;
constexpr uint8_t  TOFF         = g::TOFF;
constexpr uint16_t VOLLSCHRITTE = (uint16_t)g::VOLLSCHRITTE;

TMC2209Stepper tmc(&Serial2, R_SENSE, TMC_ADDR);
FastAccelStepperEngine engine = FastAccelStepperEngine();
FastAccelStepper *motor = nullptr;
uint16_t mikroschritte = (uint16_t)g::MIKROSCHRITTE;  // gilt erst, wenn Menue 2 lief
bool     konfiguriert  = false;
volatile uint32_t index_zaehler = 0;

// `index_zaehler++` auf einem volatile ist seit C++20 abgekuendigt, und
// Arduino-Core 3.3.11 uebersetzt mit gnu++2b - unter Core 2.0.17 (gnu++11)
// hat es nie gewarnt. `x = x + 1` ist NICHT abgekuendigt und hier
// buchstaeblich derselbe Maschinencode: der Zaehler wird nur in dieser ISR
// geschrieben, ein zweiter Schreiber gibt es nicht.
void IRAM_ATTR index_isr() { index_zaehler = index_zaehler + 1; }
long umdrehung() { return (long)VOLLSCHRITTE * mikroschritte; }

// --- Konsole und kleine Helfer --------------------------------------------
// Eine Zeile blockierend lesen. CR oder LF beendet, Reste werden verworfen.
String zeile(const char *frage) {
  Serial.print(frage);
  while (Serial.available()) Serial.read();
  String s;
  for (;;) {
    while (!Serial.available()) delay(2);
    const char c = Serial.read();
    if (c == '\r' || c == '\n') break; else s += c;
  }
  Serial.println();
  s.trim();
  return s;
}
long  zahl(const char *f, long v)       { String s = zeile(f); return s.length() ? s.toInt()   : v; }
float kommazahl(const char *f, float v) { String s = zeile(f); return s.length() ? s.toFloat() : v; }

// Soll/Ist mit Toleranz. tol = 0 heisst exakte Gleichheit.
void pruef(const char *name, long soll, long ist, long tol = 0) {
  Serial.printf("  %-26s soll %6ld   ist %6ld   %s\n", name, soll, ist,
                labs(soll - ist) <= tol ? "OK" : "FEHLER");
}

// TMC2209: I_rms = (CS+1)/32 * Vfs/(Rsense+0,02) / sqrt(2). vsense waehlt Vfs
// und wird von TMCStepper selbst gesetzt, also aus dem Chip lesen.
uint16_t ma_aus_cs(uint8_t cs) {
  const float vfs = tmc.vsense() ? 0.180f : 0.325f;
  return (uint16_t)lroundf((cs + 1) / 32.0f * vfs / (R_SENSE + 0.02f) * 0.70711f * 1000.0f);
}

// Rampe setzen. Briefing 4: Anlauf auf 1 U/s in rund 0,2 s. Nie hart anfahren.
void tempo(uint32_t hz) {
  motor->setSpeedInHz(hz);
  motor->setAcceleration(max<uint32_t>(200, hz * 5));
}
void warnung() {
  if (!konfiguriert) Serial.println("HINWEIS: Menue 2 lief nicht. Mikroschritte und Strom unbestaetigt.");
}

// --- 1: TMC2209 ueber UART ansprechen -------------------------------------
void m1_uart() {
  const uint8_t v = tmc.version();
  Serial.printf("IC-Version: 0x%02X (erwartet 0x21)\n", v);
  if (v == 0x21) {
    Serial.printf("OK - TMC2209 antwortet. IFCNT=%d, DRV_STATUS=0x%08X\n", tmc.IFCNT(), tmc.DRV_STATUS());
    return;
  }
  if (v == 0x20) Serial.println("0x20 ist ein TMC2208, kein 2209.");
  Serial.println("Keine gueltige Antwort. Ursachen, in dieser Reihenfolge:\n"
                 " 1 VM fehlt. Ohne Motorspannung schweigt der Treiber, auch wenn\n"
                 "   VDD anliegt. Das ist die haeufigste Ursache.\n"
                 " 2 TX und RX liegen auf ZWEI verschiedenen PDN-Pads. Sie gehoeren\n"
                 "   auf DASSELBE: Pin 4, das vierte von EN. Pin 5 ist ab Werk nicht\n"
                 "   mit dem Chip verbunden - RX haengt dort in der Luft.\n"
                 " 3 Der Widerstand am Modul sitzt doch am Pin 5. Dann den ganzen\n"
                 "   Knoten auf Pin 5 umstecken, eine Ader.\n"
                 " 4 1-kOhm-Widerstand in Reihe TX (GPIO17) -> Knoten fehlt.\n"
                 " 5 Vertauscht: der Widerstand gehoert in die TX-Ader, RX geht direkt.\n"
                 "Einen Jumper PDN->VDD gibt es an diesem Modul NICHT - nicht danach suchen.");
}

// Der Registerblock. Laeuft ZWEIMAL: blind in setup(), bevor die Endstufe
// eingeschaltet wird, und noch einmal in Menue 2 mit Rueckvergleich. Der Lauf in
// setup() ist der wichtigere - ohne ihn haelt der Motor ab der ersten Sekunde
// mit dem Strom, den das Trimmpoti vorgibt (ab Werk 0,9 A).
void register_setzen(uint16_t ms) {
  tmc.begin();
  tmc.I_scale_analog(false);   // Strom aus dem Register, nicht vom VREF-Pin
  tmc.pdn_disable(true);       // PDN ist UART, nicht Standby
  tmc.mstep_reg_select(true);  // Mikroschritte aus MRES, nicht von MS1/MS2
  tmc.toff(TOFF);
  tmc.blank_time(24);
  tmc.intpol(true);            // MicroPlyer
  tmc.microsteps(ms);
  tmc.rms_current(STROM_LAUF, (float)STROM_HALT / STROM_LAUF);
  tmc.iholddelay(6);
  tmc.TPOWERDOWN(20);
  tmc.en_spreadCycle(false);   // false = stealthChop
  tmc.pwm_autoscale(true);     // stealthChop braucht die Regelung
  tmc.freewheel(0);            // Stillstand normal, kein Freilauf
  delay(20);
}

// --- 2: Register setzen und zurueckvergleichen ----------------------------
void m2_register() {
  const long ms = zahl("Mikroschritte 16/32/64/128/256 [16]: ", 16);
  if (ms != 16 && ms != 32 && ms != 64 && ms != 128 && ms != 256) { Serial.println("Ungueltig."); return; }
  const uint8_t ifcnt_vor = tmc.IFCNT();
  register_setzen((uint16_t)ms);
  Serial.println("Zurueckgelesen aus CHOPCONF / GCONF / PWMCONF:");
  pruef("Mikroschritte", ms, tmc.microsteps());
  pruef("toff", TOFF, tmc.toff());
  pruef("Interpolation", 1, tmc.intpol());
  pruef("spreadCycle (0=stealth)", 0, tmc.en_spreadCycle());
  pruef("pwm_autoscale", 1, tmc.pwm_autoscale());
  pruef("Freilauf (0=normal)", 0, tmc.freewheel());
  Serial.printf("  GCONF: pdn_disable=%d mstep_reg_select=%d I_scale_analog=%d (soll 1 1 0)\n",
                tmc.pdn_disable(), tmc.mstep_reg_select(), tmc.I_scale_analog());
  // IHOLD_IRUN ist im TMC2209 write-only. Der Getter liefert das Schatten-
  // register der Bibliothek, nicht den Chip. Beweis, dass die Schreibzugriffe
  // ankamen, ist deshalb IFCNT - dieser Zaehler sitzt im Chip.
  Serial.println("Strom aus dem Schattenregister gerechnet (IHOLD_IRUN ist write-only):");
  pruef("Laufstrom mA", STROM_LAUF, ma_aus_cs(tmc.irun()), 56);   // 56 mA = ein CS-Schritt
  pruef("Haltestrom mA", STROM_HALT, ma_aus_cs(tmc.ihold()), 56);
  const uint8_t d = (uint8_t)(tmc.IFCNT() - ifcnt_vor);
  Serial.printf("  IFCNT +%d (>=12 erwartet, 0 heisst kein Schreibzugriff kam an)\n", d);
  mikroschritte = tmc.microsteps();
  konfiguriert  = (tmc.microsteps() == ms) && (d > 0);
  Serial.printf("Umdrehung = %ld Mikroschritte, 1 U/s = %ld Schritte/s = 200 Vollschritte/s\n",
                umdrehung(), umdrehung());
}

// --- 3: N Schritte fahren, INDEX gegen die Kommandos ----------------------
void m3_schritte() {
  warnung();
  const long n  = zahl("Anzahl Schritte [eine Umdrehung]: ", umdrehung());
  const long hz = zahl("Rate in Schritten/s [1000]: ", 1000);
  const bool rueck = zeile("Richtung v=vorwaerts r=rueckwaerts [v]: ").startsWith("r");
  if (n <= 0 || hz <= 0) { Serial.println("Ungueltig."); return; }
  index_zaehler = 0;
  tempo(hz);
  motor->move(rueck ? -n : n, true);   // blockierend, mit Rampe
  delay(50);                           // letzte Impulse einsammeln
  const uint32_t gez = index_zaehler;
  Serial.printf("kommandiert %ld, INDEX-Impulse %u, Position %d\n", n, gez, motor->getCurrentPosition());
  if (gez == 0) { Serial.println("Kein INDEX: GPIO35 pruefen, oder INDEX gibt Overtemp aus."); return; }
  Serial.printf("Schritte je Impuls = %.3f\n", (double)n / gez);
  Serial.printf("Erwartet: ein Impuls je elektrischer Umdrehung = 4 Vollschritte, also %d bei\n"
                "1/%d. Der Wert selbst ist Nebensache - er muss ueber alle Laeufe KONSTANT\n"
                "sein. Wandert er, verschluckt der Treiber Schritte.\n",
                4 * mikroschritte, mikroschritte);
}

// --- 4: Hall-Sweep, eine ganze Umdrehung, CSV auf die Konsole -------------
void m4_hall() {
  warnung();
  const long n  = umdrehung();
  const long hz = zahl("Rate fuer den Sweep in Schritten/s [200]: ", 200);
  if (hz <= 0) { Serial.println("Ungueltig."); return; }
  tempo(hz);
  // Briefing 9: steht der Magnet schon im Fenster, erst herausfahren. Danach
  // ist der Anfang des Sweeps garantiert Ruhefeld - das ist die Nulllinie.
  Serial.println("Fahre aus dem Sensorfenster heraus ...");
  for (long i = 0; i < n && digitalRead(PIN_HALL_D0) == D0_AKTIV; i++) motor->move(1, true);
  motor->move(4L * mikroschritte, true);   // Sicherheitsabstand
  // Fuer die Auswertung eine Umdrehung Millivolt im RAM, falls sie hineinpasst.
  uint16_t *puffer = (n <= 8192) ? (uint16_t *)malloc(n * sizeof(uint16_t)) : nullptr;
  long f_erste = -1, f_letzte = -1, fenster = 0;
  Serial.println("schritt,volt,d0");
  for (long i = 0; i < n; i++) {
    delay(2);                        // Sensor und Komparator beruhigen
    uint32_t mv = 0;
    for (uint8_t k = 0; k < 4; k++) mv += analogReadMilliVolts(PIN_HALL_A0);
    mv /= 4;
    const int d0 = digitalRead(PIN_HALL_D0);
    Serial.printf("%ld,%.4f,%d\n", i, mv / 1000.0, d0);
    if (puffer) puffer[i] = (uint16_t)mv;
    if (d0 == D0_AKTIV) { if (f_erste < 0) f_erste = i; f_letzte = i; fenster++; }
    motor->move(1, true);
  }
  Serial.println("--- Auswertung ---");
  if (fenster == 0)
    Serial.println("D0 sprach nie an: Magnet zu weit weg, Poti zu unempfindlich, oder\n"
                   "D0_AKTIV im Sketch hat die falsche Polaritaet.");
  else
    Serial.printf("D0-Fenster: %ld Schritte, von %ld bis %ld, Mitte %.1f. Der Rueckzugsweg\n"
                  "beim Homing muss groesser sein als diese Breite.\n",
                  fenster, f_erste, f_letzte, (f_erste + f_letzte) / 2.0);
  if (!puffer) { Serial.println("Umdrehung zu gross fuer die RAM-Auswertung - Sweep mit\n"
                 "1/16 oder 1/32 fahren, das genuegt fuer Fenster und Schwerpunkt."); return; }
  // Ruhespannung aus allem AUSSERHALB des Fensters. Nicht mit 1,65 V annehmen.
  const long a = max(0L, f_erste - 50), b = min(n - 1, f_letzte + 50);
  uint32_t summe = 0; long anz = 0;
  for (long i = 0; i < n; i++) if (f_erste < 0 || i < a || i > b) { summe += puffer[i]; anz++; }
  const long ruhe = anz ? (long)(summe / anz) : puffer[0];
  // Mit dem BETRAG der Auslenkung rechnen - dann darf der Magnet falsch herum
  // sitzen, der SS49E ist linear und bipolar.
  long gipfel = 0, gipfel_i = 0;
  for (long i = 0; i < n; i++) { const long ab = labs((long)puffer[i] - ruhe);
                                 if (ab > gipfel) { gipfel = ab; gipfel_i = i; } }
  // Schwerpunkt der Feldkurve, 20 Prozent Schwelle gegen das ADC-Rauschen.
  double zaehler = 0, gewicht = 0;
  for (long i = 0; i < n; i++) { const long w = labs((long)puffer[i] - ruhe) - gipfel / 5;
                                 if (w > 0) { zaehler += (double)i * w; gewicht += w; } }
  free(puffer);
  Serial.printf("Ruhespannung %.4f V, Gipfelhoehe %.4f V bei Schritt %ld\n",
                ruhe / 1000.0, gipfel / 1000.0, gipfel_i);
  if (gipfel < 250)      Serial.println("Gipfel <0,25 V: Magnetabstand verringern, der Index waere unsicher.");
  else if (gipfel > 550) Serial.println("Gipfel >0,55 V: Abstand vergroessern, sonst klippt die Kurve.");
  else                   Serial.println("Aussteuerung im Fenster 0,25 bis 0,55 V - passt.");
  if (gewicht > 0) {
    const double sp = zaehler / gewicht;
    Serial.printf("Schwerpunkt der Feldkurve bei Schritt %.2f\n", sp);
    if (f_erste >= 0)
      Serial.printf("Versatz Fenstermitte -> Schwerpunkt = %.2f Schritte. Das ist der Wert aus\n"
                    "Briefing 11.1, den der Driftmonitor spaeter braucht.\n",
                    sp - (f_erste + f_letzte) / 2.0);
  }
}

// --- 5: Dauerlauf fuer den Resonanztest -----------------------------------
void m5_dauerlauf() {
  warnung();
  const float ups = kommazahl("Rate in Umdrehungen/s [1.0]: ", 1.0f);
  const long  hz  = lroundf(ups * umdrehung());
  if (ups <= 0 || hz <= 0) { Serial.println("Ungueltig."); return; }
  Serial.printf("%.2f U/s = %ld Schritte/s = %.0f Vollschritte/s. Die Resonanz liegt bei\n"
                "rund 162 Hz Vollschritt und wird beim Anfahren durchfahren. Ruckelt der\n"
                "Umlauf: 0,8 U/s versuchen oder die Rampe steiler machen.\n",
                ups, hz, ups * VOLLSCHRITTE);
  index_zaehler = 0;
  tempo(hz);
  motor->runForward();
  Serial.println("Laeuft. Enter beendet - mit Rampe, nie hart.");
  const uint32_t t0 = millis();
  while (!Serial.available()) {
    delay(1000);
    Serial.printf("  t=%lu s  pos=%d  INDEX=%u\n", (millis() - t0) / 1000,
                  motor->getCurrentPosition(), index_zaehler);
  }
  motor->stopMove();                     // bremst die Rampe herunter
  while (motor->isRunning()) delay(10);
  while (Serial.available()) Serial.read();
  Serial.printf("Gestoppt. Position %d, INDEX %u, Schritte je Impuls %.3f\n",
                motor->getCurrentPosition(), index_zaehler,
                index_zaehler ? (double)motor->getCurrentPosition() / index_zaehler : 0.0);
}

// --- Menue ----------------------------------------------------------------
void menue() {
  Serial.println("\n=== Star Watch Inbetriebnahme - ohne Riemen, ohne Klinge ===\n"
                 " 1  TMC2209 ueber UART ansprechen, IC-Version lesen\n"
                 " 2  Register setzen und zurueckvergleichen (zuerst laufen lassen)\n"
                 " 3  N Schritte fahren, INDEX gegen kommandierte Schritte\n"
                 " 4  Hall-Sweep ueber eine Umdrehung, CSV schritt,volt,d0\n"
                 " 5  Dauerlauf fuer den Resonanztest um 1 U/s");
  Serial.printf("Stand: 1/%d Mikroschritte, %ld Schritte je Umdrehung\n", mikroschritte, umdrehung());
}

void setup() {
  Serial.begin(115200);
  delay(400);
  pinMode(PIN_HALL_D0, INPUT);   // GPIO34 hat keinen internen Pull-up
  pinMode(PIN_INDEX, INPUT);
  analogReadResolution(12);
  attachInterrupt(PIN_INDEX, index_isr, RISING);
  Serial2.begin(TMC_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  // Register VOR der Bestromung setzen. Sonst gilt bis Menue 2 der Werkszustand:
  // I_scale_analog=1, IRUN am Anschlag, Strom nach Trimmpoti.
  register_setzen(mikroschritte);
  if (tmc.version() != 0x21)
    Serial.println("WARNUNG: TMC2209 antwortet nicht (Menue 1). Die Register sind\n"
                   "nicht angekommen - der Strom kommt jetzt allein vom Trimmpoti.\n"
                   "Steht es auf Werkseinstellung, sind das rund 0,9 A statt 0,4 A.\n"
                   "Erst die Verdrahtung klaeren, dann laufen lassen.");
  engine.init();
  motor = engine.stepperConnectToPin(PIN_STEP);
  if (!motor) { Serial.println("FastAccelStepper: GPIO25 nicht nutzbar."); while (true) delay(1000); }
  motor->setDirectionPin(PIN_DIR, g::DIR_HIGH_ZAEHLT_AUFWAERTS);
  motor->setEnablePin(PIN_EN, true);   // true = LOW aktiviert (ENN)
  motor->setAutoEnable(false);
  motor->enableOutputs();              // Haltestrom bleibt an, kein Freilauf
  tempo(1000);
  menue();
}

void loop() {
  const String w = zeile("\nAuswahl 1-5 (m = Menue): ");
  if      (w == "1") m1_uart();
  else if (w == "2") m2_register();
  else if (w == "3") m3_schritte();
  else if (w == "4") m4_hall();
  else if (w == "5") m5_dauerlauf();
  else menue();
}
