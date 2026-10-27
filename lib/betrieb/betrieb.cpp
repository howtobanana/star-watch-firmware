#include "betrieb.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <geraet.h>

namespace starwatch {

namespace g = geraet;

namespace {
/// Takt des Uhrbetriebs. Ein Mikroschritt faellt bei 1/16 fruehestens alle
/// 13,5 s an, 5 s genuegen also reichlich. Es kann sich kein Fehler
/// aufsummieren, weil jeder Durchlauf die Sollposition NEU aus der Uhrzeit
/// rechnet und nicht aus dem letzten Ergebnis.
constexpr uint32_t UHR_TAKT_MS = 5000;

/// Steht der Zeiger bis zu so vielen Schritten VOR der Zeit, wird gewartet,
/// statt eine ganze Umdrehung nachzufahren.
constexpr int32_t VORLAUF_TOLERANZ = 8;

/// Ab diesem Rueckstand holt die Uhr im Fahrtempo auf statt im leisen
/// Uhrtempo (27.09.2026). 27 Schritte sind 6 Minuten auf dem Blatt - mehr
/// als jeder regulaere Minutensprung (4,4 Schritte), weniger als jede
/// Rueckkehr nach Timer oder Handfahrt, die man sieht.
constexpr int32_t AUFHOLEN_AB = 27;

// TIMERLAUF (27.09.2026). Bis dahin bekam der Zeiger alle dauer/200 ms,
// hoechstens alle 250 ms, ein neues Ziel. Jedes davon war eine EIGENE Fahrt
// mit voller Rampe - beim 5-min-Timer 2,7 Schritte in 15 ms, dann 235 ms
// Stillstand. Das war das Ruckeln der Klinge, und mehr Schuebe machten es nur
// kleiner. Jetzt gleitet er in einem Zug (timer_gleiten) oder springt
// bewusst einmal je Sekunde (timer_springen).

/// So oft wird das Tempo der Gleitfahrt gegen die Uhr geprueft. Die Impulse
/// kommen aus demselben Quarz wie millis(); nachgefuehrt wird nur der
/// Anlauf und alles, was von aussen stoert.
constexpr uint32_t TIMER_NACHFUEHREN_MS = 1000;

/// Ueber diese Zeit wird ein Rueckstand der Gleitfahrt abgebaut.
constexpr uint32_t TIMER_AUSGLEICH_MS = 2000;

constexpr uint32_t TIMER_ANFAHRT_TIMEOUT_MS = 120000;
constexpr uint32_t TIMER_ENDFAHRT_TIMEOUT_MS = 60000;

// ===========================================================================
// NVS - AP 3.1: ZWEI BEREICHE, NICHT EINER
// ===========================================================================
// Umgesetzt am 23.08.2026 nach Umsetzungsplan AP 3.1.
//
//   FERTIGUNG   entsteht bei der Montage, ist fuer JEDE Uhr anders und kann
//               vom Kunden nicht wiederhergestellt werden. Wird NICHT
//               exportiert und ueberlebt den Werksreset.
//               -> Hall-Offset. Spaeter: Seriennummer, Geraetetoken,
//                  API-Schluessel, Kalibrierdatum.
//
//   BENUTZER    alles, was der Bediener einstellt. Wird exportiert und vom
//               Werksreset geloescht.
//               -> Fahrtempo, Rampe, Schwungkreise, Zeitmodus, Helligkeit.
//                  WLAN liegt in sw_netz (testweb) und wird mitgeloescht.
//
// WARUM DIE TRENNUNG VOR EXPORT UND RESET KOMMEN MUSS: wanderte der Offset in
// eine Exportdatei, wuerde ein Import von einer ANDEREN Uhr die Kalibrierung
// zerstoeren - und die Uhr ginge falsch, waehrend sie behauptet, kalibriert zu
// sein. Loeschte ihn ein Werksreset, waere eine verkaufte Uhr unbrauchbar, bis
// jemand sie neu einmisst, und das kann der Kunde nicht.
//
// DER FERTIGUNGSBEREICH HEISST WEITER "starwatch". Nicht aus Bequemlichkeit:
// dort liegt der eingemessene Offset schon. Ihn in einen neuen Bereich zu
// verschieben waere eine Migration des EINEN Werts, den niemand
// wiederherstellen kann - fuer nichts als einen schoeneren Namen.
const char *NVS_FERTIGUNG = "starwatch";
const char *NVS_BENUTZER = "sw_nutzer";

const char *K_OFFSET = "hall_offset";
const char *K_OTA_PW = "ota_pw";
const char *K_AP_PW = "ap_pw";
const char *K_TOKEN = "token";
const char *K_WEISS = "weiss";  // 0xRRGGBB, Weiss dieser Uhr
/// Zaehler der Stromzyklen. Im FERTIGUNGSbereich, damit ihn ein Werksreset
/// nicht selbst loescht - sonst zaehlte der Reset seinen eigenen Zaehler weg.
const char *K_ZYKLEN = "zyklen";
const char *K_TEMPO = "tempo";
const char *K_RAMPE = "rampe";
const char *K_KREISE = "kreise";
// Der Vorgabewert von rueckfall_frist_ms_ steht als blanke Zahl im Header,
// weil der Header geraet.h nicht einbindet. Hier faellt es auf, wenn sich die
// beiden auseinanderbewegen.
static_assert(g::RUECKFALL_MS == 10000,
              "rueckfall_frist_ms_ im Header nachziehen.");

static_assert(g::MQTT_PORT_VORGABE == 1883,
              "mqtt_port_ im Header nachziehen.");

const char *K_MODUS = "modus";
const char *K_EFF_START = "eff_start";
const char *K_EFF_STD = "eff_std";
const char *K_LICHT = "licht";
const char *K_LICHT_AN = "licht_an";
const char *K_FARBE = "farbe";  // 0xRRGGBB
const char *K_TZ = "tz";
// Lichtautomatik (27.09.2026). Kurz, weil NVS-Schluessel hoechstens 15
// Zeichen haben.
const char *K_LA_AN = "la_an";
const char *K_LA_NACHT = "la_nacht";
const char *K_LA_MIN = "la_min";
const char *K_LA_HELL = "la_hell";
const char *K_LA_AUS = "la_aus";
// Puls (0.20.0): Periode in ms, Minimum als Anteil des Maximums.
const char *K_PULS_PER = "p_per";
const char *K_PULS_MIN = "p_min";
// Haltestrom in mA (01.10.2026) - ohne Licht der groesste Verbraucher.
const char *K_HALT = "halt_ma";
const char *K_MQ_AN = "mq_an";
const char *K_MQ_HOST = "mq_host";
const char *K_MQ_PORT = "mq_port";
const char *K_MQ_USER = "mq_user";
const char *K_MQ_PW = "mq_pw";
const char *K_TIMER_ENDE = "t_ende";
const char *K_TIMER_LAUF = "t_lauf";
const char *K_ALIAS = "alias";

// Homing-Verlauf (05.10.2026). EIGENER BEREICH: kein Benutzerwert (nicht im
// Export, nicht im Werksreset) und kein Fertigungswert - ein Messprotokoll.
const char *NVS_VERLAUF = "sw_verlauf";
const char *K_VERLAUF = "homing";

Preferences nvs;      ///< Fertigung
Preferences nvs_ben;  ///< Benutzer
}  // namespace

namespace {
/// Zufaelliges Passwort aus einem Alphabet OHNE verwechselbare Zeichen.
///
/// Kein 0/O, kein 1/l/I: das Passwort steht auf einem Zettel und wird von
/// Hand abgetippt. Ein Alphabet mit 32 Zeichen und 12 Stellen sind 60 Bit -
/// fuer ein Geraet im Heimnetz reichlich, und noch abschreibbar.
std::string wuerfeln(size_t stellen) {
  static const char *ABC = "23456789abcdefghjkmnpqrstuvwxyz";
  const size_t n = strlen(ABC);
  std::string s;
  s.reserve(stellen);
  for (size_t i = 0; i < stellen; i++)
    s.push_back(ABC[esp_random() % n]);
  return s;
}
}  // namespace

const char *zeitmodus_text(Zeitmodus m) {
  switch (m) {
    case Zeitmodus::aus:           return "aus";
    case Zeitmodus::fliessend:     return "fliessend";
    case Zeitmodus::minutensprung: return "Minutensprung";
  }
  return "unbekannt";
}

const char *timerlauf_text(Timerlauf l) {
  switch (l) {
    case Timerlauf::gleitend:     return "gleitend";
    case Timerlauf::sekundentakt: return "Sekundentakt";
  }
  return "unbekannt";
}

void Betrieb::beginn(Antrieb *antrieb, Hall *hall, Licht *licht) {
  antrieb_ = antrieb;
  hall_ = hall;
  licht_ = licht;

  nvs.begin(NVS_FERTIGUNG, false);
  hall_offset_ = nvs.getInt(K_OFFSET, 0);
  weiss_ = nvs.getUInt(K_WEISS, g::LED_WEISS_VORGABE) & 0xFFFFFF;
  identitaet_laden();

  {
    Preferences p;
    if (p.begin(NVS_VERLAUF, true)) {
      const size_t n = p.getBytesLength(K_VERLAUF);
      if (n == Homingverlauf::abbild_groesse()) {
        uint8_t roh[Homingverlauf::abbild_groesse()];
        p.getBytes(K_VERLAUF, roh, n);
        verlauf_.laden(roh, n);
      }
      p.end();
    }
  }

  // Benutzerwerte: die Vorgaben aus geraet.h sind der Rueckfall, nicht der
  // gesetzte Wert - was einmal eingestellt wurde, ueberlebt den Neustart.
  nvs_ben.begin(NVS_BENUTZER, false);
  fahrtempo_ = nvs_ben.getUInt(K_TEMPO, g::TEMPO_FAHRT);
  effekt_kreise_ = nvs_ben.getInt(K_KREISE, 0);
  const uint32_t rampe = nvs_ben.getUInt(K_RAMPE, g::BESCHLEUNIGUNG);
  const uint8_t modus = nvs_ben.getUChar(K_MODUS, g::MODUS_VORGABE);
  effekt_start_ = nvs_ben.getBool(K_EFF_START, true);
  effekt_stunde_ = nvs_ben.getBool(K_EFF_STD, true);
  timerlauf_ = nvs_ben.getUChar(K_TIMER_LAUF, 0) == 1 ? Timerlauf::sekundentakt
                                                      : Timerlauf::gleitend;
  // Auch der gespeicherte Name geht durch die Pruefung - ein Wert aus einer
  // aelteren oder fremden Firmware soll nicht ungeprueft in JSON landen.
  if (!alias_pruefen(nvs_ben.getString(K_ALIAS, "").c_str(), &alias_))
    alias_.clear();
  const float licht_p = nvs_ben.getFloat(K_LICHT, g::LICHT_VORGABE_PROZENT);
  const bool licht_an = nvs_ben.getBool(K_LICHT_AN, true);
  const uint32_t farbe = nvs_ben.getUInt(K_FARBE, g::LED_FARBE_VORGABE);
  zeitzone_ = nvs_ben.getString(K_TZ, g::ZEITZONE_VORGABE).c_str();
  if (zeitzone_.empty())
    zeitzone_ = g::ZEITZONE_VORGABE;

  {
    Lichtautomatik a;
    a.an = nvs_ben.getBool(K_LA_AN, true);
    a.nacht = nvs_ben.getBool(K_LA_NACHT, true);
    a.anteil_min = nvs_ben.getFloat(K_LA_MIN, g::LUX_ANTEIL_MIN);
    a.lux_hell = nvs_ben.getFloat(K_LA_HELL, g::LUX_HELL);
    a.nacht_aus = nvs_ben.getFloat(K_LA_AUS, g::LUX_NACHT_AUS);
    automatik_ = lichtautomatik_begrenzt(a, g::LUX_DUNKEL);
    nachtschalter_ = Nachtschalter(automatik_.nacht_aus, nacht_an_lux(automatik_),
                                   g::LUX_NACHT_VERZUG_MS, g::LUX_NACHT_AN_VERZUG_MS);
  }

  mqtt_an_ = nvs_ben.getBool(K_MQ_AN, false);
  mqtt_host_ = nvs_ben.getString(K_MQ_HOST, "").c_str();
  mqtt_port_ = nvs_ben.getUShort(K_MQ_PORT, g::MQTT_PORT_VORGABE);
  if (mqtt_port_ == 0)
    mqtt_port_ = g::MQTT_PORT_VORGABE;
  mqtt_benutzer_ = nvs_ben.getString(K_MQ_USER, "").c_str();
  mqtt_passwort_ = nvs_ben.getString(K_MQ_PW, "").c_str();

  if (antrieb_ != nullptr) {
    antrieb_->rampe(rampe, rampe);
    antrieb_->tempo(fahrtempo_);
    antrieb_->haltestrom(nvs_ben.getUShort(K_HALT, g::STROM_HALT));
  }
  if (licht_ != nullptr)
    licht_->farbe(farbe);
  if (licht_ != nullptr)
    licht_->puls_einstellen(nvs_ben.getUInt(K_PULS_PER, PULS_PERIODE_VORGABE_MS),
                            nvs_ben.getFloat(K_PULS_MIN, PULS_MIN_VORGABE));
  if (licht_ != nullptr)
    licht_->effekte(effekt_start_);
  if (licht_ != nullptr && licht_p >= 0.0f) {
    licht_->soll_prozent(licht_p);
    // Auch der EIN-Zustand gehoert dazu: nur den Regler zu merken hiess, dass
    // eine eingeschaltete Klinge nach jedem Neustart dunkel blieb, waehrend
    // die Anzeige "Regler 42 %" behauptete.
    licht_->an(licht_an);
  }

  // Der Zeitmodus wird gemerkt, aber NICHT sofort gesetzt: ohne Uhrzeit gibt
  // es keine Sollposition, und die Zeit kommt erst mit dem Netz. `zeit_setzen`
  // holt ihn nach.
  modus_nach_zeit_ = static_cast<Zeitmodus>(modus <= 2 ? modus : 0);
  // Der Rueckfall zielt auf den GESPEICHERTEN Modus, nicht auf einen festen
  // Wert. Wer die Uhr ausdruecklich auf 'm 0' stellt und das sichert, will
  // sie aus - dann holt ihn auch keine Frist zurueck.
  rueckfall_modus_ = modus_nach_zeit_;
  letzte_handlung_ms_ = millis();
}

// ===========================================================================
// Einstellungen sichern - jede Setzfunktion schreibt selbst
// ===========================================================================

void Betrieb::werte_sichern() {
  nvs_ben.putUInt(K_TEMPO, fahrtempo_);
  nvs_ben.putInt(K_KREISE, effekt_kreise_);
  if (antrieb_ != nullptr)
    nvs_ben.putUInt(K_RAMPE, antrieb_->beschleunigung());
  // NICHT zeitmodus_, sondern das RUECKFALLZIEL. Der Unterschied ist der
  // Zustand waehrend einer Handfahrt: dort steht zeitmodus_ auf `aus`, und
  // zwar voruebergehend. Wird in diesen zehn Sekunden irgendetwas anderes
  // gesichert - Licht an, Tempo, Schwung, alle rufen hier herein -, dann
  // wandert dieses voruebergehende `aus` in den Speicher und die Uhr kommt
  // nach dem naechsten Start GEPARKT hoch.
  //
  // AM GERAET NACHGEWIESEN am 25.08.2026: von Hand auf 9:00 gefahren, innerhalb
  // der Frist das Licht geschaltet, neu gestartet - Modus `aus`, Rueckfall
  // `aus`, Zeiger steht. rueckfall_modus_ traegt dagegen das, was der Bediener
  // ausdruecklich gewaehlt hat, und genau das gehoert in den Speicher.
  nvs_ben.putUChar(K_MODUS, static_cast<uint8_t>(rueckfall_modus_));
  if (licht_ != nullptr) {
    nvs_ben.putFloat(K_LICHT, licht_->soll_prozent());
    // an_dauerhaft: waehrend eines Pulses der Zustand DAVOR (licht.h).
    nvs_ben.putBool(K_LICHT_AN, licht_->an_dauerhaft());
    nvs_ben.putUInt(K_FARBE, licht_->farbe());
  }
}

void Betrieb::haltestrom_setzen(uint16_t ma) {
  if (antrieb_ == nullptr)
    return;
  antrieb_->haltestrom(ma);
  nvs_ben.putUShort(K_HALT, antrieb_->haltestrom());
}

void Betrieb::puls_setzen(uint32_t periode_ms, float min_anteil) {
  if (licht_ == nullptr)
    return;
  licht_->puls_einstellen(periode_ms, min_anteil);
  nvs_ben.putUInt(K_PULS_PER, licht_->puls_periode());
  nvs_ben.putFloat(K_PULS_MIN, licht_->puls_min());
}

void Betrieb::lichtautomatik_setzen(const Lichtautomatik &a) {
  automatik_ = lichtautomatik_begrenzt(a, g::LUX_DUNKEL);
  nachtschalter_.schwellen(automatik_.nacht_aus, nacht_an_lux(automatik_));
  nvs_ben.putBool(K_LA_AN, automatik_.an);
  nvs_ben.putBool(K_LA_NACHT, automatik_.nacht);
  nvs_ben.putFloat(K_LA_MIN, automatik_.anteil_min);
  nvs_ben.putFloat(K_LA_HELL, automatik_.lux_hell);
  nvs_ben.putFloat(K_LA_AUS, automatik_.nacht_aus);
}

void Betrieb::identitaet_laden() {
  // Seriennummer AUS DER MAC - nicht gespeichert, sondern gerechnet. Damit
  // ueberlebt sie auch ein geloeschtes NVS, und zwei Geraete koennen nie
  // dieselbe haben.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char s[8];
  snprintf(s, sizeof(s), "%02X%02X", mac[4], mac[5]);
  seriennummer_ = s;
  geraetename_ = std::string("starwatch-") + s;

  // Passwoerter beim ERSTEN Start wuerfeln und behalten.
  ota_passwort_ = nvs.getString(K_OTA_PW, "").c_str();
  if (ota_passwort_.empty()) {
    ota_passwort_ = wuerfeln(12);
    nvs.putString(K_OTA_PW, ota_passwort_.c_str());
  }
  ap_passwort_ = nvs.getString(K_AP_PW, "").c_str();
  if (ap_passwort_.empty()) {
    ap_passwort_ = wuerfeln(12);
    nvs.putString(K_AP_PW, ap_passwort_.c_str());
  }
  // Laenger als die Passwoerter: das Token wird nicht abgetippt, sondern
  // einmal eingefuegt und dann vom Browser behalten. 24 Stellen sind rund
  // 120 Bit.
  token_ = nvs.getString(K_TOKEN, "").c_str();
  if (token_.empty()) {
    token_ = wuerfeln(24);
    nvs.putString(K_TOKEN, token_.c_str());
  }
}

std::string Betrieb::geraeteblatt() const {
  return "  Geraet        " + geraetename_ + "\n" +
         "  Seriennummer  " + seriennummer_ + "\n" +
         "  OTA-Passwort  " +
         (g::ENTWICKLUNGSGERAET && g::OTA_PASSWORT_GEMEINSAM
              ? std::string("(Entwicklung: gemeinsames Passwort aus dem Schluesselbund)")
              : ota_passwort_) + "\n" +
         "  Zugangspunkt  StarWatch-" + seriennummer_ + "\n" +
         "  dessen Schluessel  " + ap_passwort_ + "\n" +
         "  Zugriffstoken " + token_ + "\n";
}

void Betrieb::zeitzone(const std::string &tz) {
  zeitzone_ = tz.empty() ? std::string(g::ZEITZONE_VORGABE) : tz;
  nvs_ben.putString(K_TZ, zeitzone_.c_str());
}

void Betrieb::mqtt_an(bool ja) {
  mqtt_an_ = ja;
  nvs_ben.putBool(K_MQ_AN, mqtt_an_);
}

void Betrieb::mqtt_host(const std::string &h) {
  mqtt_host_ = h;
  nvs_ben.putString(K_MQ_HOST, mqtt_host_.c_str());
}

void Betrieb::mqtt_port(uint16_t p) {
  // Port 0 gibt es nicht. Ein leeres Feld in der Oberflaeche darf nicht die
  // Verbindung unmoeglich machen, sondern faellt auf den Standardport zurueck.
  mqtt_port_ = (p == 0) ? g::MQTT_PORT_VORGABE : p;
  nvs_ben.putUShort(K_MQ_PORT, mqtt_port_);
}

void Betrieb::mqtt_benutzer(const std::string &b) {
  mqtt_benutzer_ = b;
  nvs_ben.putString(K_MQ_USER, mqtt_benutzer_.c_str());
}

void Betrieb::mqtt_passwort(const std::string &p) {
  mqtt_passwort_ = p;
  nvs_ben.putString(K_MQ_PW, mqtt_passwort_.c_str());
}

bool Betrieb::zyklus_zaehlen(bool war_einschalten) {
  // EIGENER ZUGRIFF auf den Speicher, nicht ueber `nvs`: gezaehlt wird als
  // ERSTES im Start, vor beginn() - sonst ist ein kurz eingesteckter Strom
  // schon wieder weg, bevor die Uhr zaehlt (26.09.2026, siehe geraet.h).
  Preferences p;
  p.begin(NVS_FERTIGUNG, false);
  uint32_t n = p.getUInt(K_ZYKLEN, 0);
  // Neustart per Software, Update oder Watchdog: NICHT mitzaehlen, aber auch
  // nicht loeschen - sonst zerreisst ein OTA mitten in einer Zaehlreihe sie.
  if (war_einschalten) {
    n++;
    p.putUInt(K_ZYKLEN, n);
  }
  p.end();
  zyklen_ = n;
  // Der Zaehler bleibt AUF der Schwelle stehen, bis zyklen_loeschen() nach
  // dem ausgefuehrten Reset kommt. Wird beim fuenften Mal zu frueh
  // abgezogen, setzt das naechste Einschalten zurueck - vorher war der
  // Zaehler dann schon auf null und der Reset verloren.
  return war_einschalten && n >= g::RESET_ZYKLEN;
}

void Betrieb::zyklen_loeschen() {
  Preferences p;
  p.begin(NVS_FERTIGUNG, false);
  p.putUInt(K_ZYKLEN, 0);
  p.end();
  zyklen_ = 0;
  zyklen_geloescht_ = true;
}

void Betrieb::zyklus_tick() {
  if (zyklen_geloescht_ || zyklen_ == 0)
    return;
  if (millis() < g::RESET_FENSTER_MS)
    return;
  // Die Uhr laeuft laenger als das Fenster - das war kein Zaehlversuch.
  zyklen_loeschen();
}

void Betrieb::werksreset() {
  // NUR der Benutzerbereich. Der Fertigungsbereich mit dem Hall-Offset bleibt
  // - genau das ist der Sinn der Trennung.
  nvs_ben.clear();
}

// ===========================================================================
// Zeit
// ===========================================================================

void Betrieb::zeit_setzen(int stunde, int minute, int sekunde) {
  if (stunde < 0 || stunde > 23 || minute < 0 || minute > 59 || sekunde < 0 ||
      sekunde > 59)
    return;
  const bool erste_zeit = !zeit_gueltig_;
  basis_sek_ = stunde * 3600 + minute * 60 + sekunde;
  basis_ms_ = millis();
  zeit_gueltig_ = true;

  // Jetzt gibt es eine Sollposition - der aus dem NVS geladene Zeitmodus kann
  // anlaufen. Nur beim ERSTEN Stellen: wer die Zeit spaeter nachstellt, will
  // damit nicht den Modus umschalten.
  if (erste_zeit && modus_nach_zeit_ != Zeitmodus::aus &&
      zeitmodus_ == Zeitmodus::aus) {
    zeitmodus_ = modus_nach_zeit_;
  }
}

int32_t Betrieb::tageszeit_sek() const {
  if (!zeit_gueltig_)
    return 0;
  const uint32_t seit = (millis() - basis_ms_) / 1000u;
  return static_cast<int32_t>((static_cast<uint32_t>(basis_sek_) + seit) % 86400u);
}

std::string Betrieb::uhrzeit_text() const {
  if (!zeit_gueltig_)
    return "--:--:--";
  const int32_t s = tageszeit_sek();
  char puffer[16];
  snprintf(puffer, sizeof(puffer), "%02d:%02d:%02d", static_cast<int>(s / 3600),
           static_cast<int>((s % 3600) / 60), static_cast<int>(s % 60));
  return std::string(puffer);
}

bool Betrieb::alias(const std::string &name) {
  std::string sauber;
  if (!alias_pruefen(name, &sauber))
    return false;
  alias_ = sauber;
  nvs_ben.putString(K_ALIAS, alias_.c_str());
  return true;
}

void Betrieb::timerlauf(Timerlauf l) {
  timerlauf_ = l;
  nvs_ben.putUChar(K_TIMER_LAUF, static_cast<uint8_t>(l));
  // Laeuft gerade ein Timer, stellt timer_tick() im naechsten Durchlauf um -
  // er sieht timer_gleitet_ und die neue Laufart nicht mehr zusammenpassen.
}

void Betrieb::effekt_start(bool an) {
  effekt_start_ = an;
  nvs_ben.putBool(K_EFF_START, an);
  // Gilt seit 28.09.2026 fuer ALLE Zuendungen und das Einfahren, nicht nur
  // fuer das Einschalten - der Name im Code ist geblieben, der NVS-Schluessel
  // auch, damit die gespeicherte Wahl erhalten bleibt.
  if (licht_ != nullptr)
    licht_->effekte(an);
}

void Betrieb::effekt_stunde(bool an) {
  effekt_stunde_ = an;
  nvs_ben.putBool(K_EFF_STD, an);
  // Beim Einschalten NICHT sofort ausloesen: sonst blitzt die Klinge, nur
  // weil jemand den Schalter umgelegt hat. Erst zur naechsten vollen Stunde.
  if (an && zeit_gueltig_)
    effekt_letzte_stunde_ = tageszeit_sek() / 3600;
}

void Betrieb::zeitmodus(Zeitmodus m) {
  zeitmodus_ = m;
  // Sofort stellen, nicht erst beim naechsten Takt - sonst wirkt das
  // Einschalten, als haette es nicht funktioniert.
  uhr_letzter_tick_ = 0;
}

int32_t Betrieb::soll_jetzt() const {
  if (!zeit_gueltig_ || zeitmodus_ == Zeitmodus::aus)
    return -1;

  // OHNE REFERENZ KEINE UHRZEIT. Der Zaehler zeigt dann auf nichts, und jede
  // daraus gerechnete Position waere geraten. Lieber ein Zeiger, der steht -
  // das faellt auf und wird gemeldet. Ein falsch gehender wird geglaubt.
  //
  // Von Hand fahren geht weiterhin ('u', 'p', 'v', 'x'): das sind
  // ausdrueckliche Anweisungen, keine Behauptung ueber die Uhrzeit.
  if (!referenz_gueltig_)
    return -1;

  int32_t sek = tageszeit_sek();
  // Sekunde auf dem Zwoelferblatt. Im Minutensprung fallen die Sekunden weg -
  // dadurch steht das Ziel eine Minute lang still und springt dann um rund
  // 4,4 Schritte weiter.
  if (zeitmodus_ == Zeitmodus::minutensprung)
    sek = (sek / 60) * 60;

  return soll_position(sek, g::REV, g::SEK_JE_SCHRITT);
}

// ===========================================================================
// Kalibrierung
// ===========================================================================

void Betrieb::hall_offset(int32_t schritte) {
  // Von Hand gesetzt ('o <n>'): der Zaehler bleibt, wo er ist. Die naechste
  // Referenzfahrt misst dann die Verschiebung - kein Schrittverlust.
  offset_von_hand_ = true;
  offset_setzen(schritte);
}

void Betrieb::weiss(uint32_t rgb) {
  weiss_ = rgb & 0xFFFFFF;
  nvs.putUInt(K_WEISS, weiss_);
}

void Betrieb::offset_setzen(int32_t schritte) {
  const int32_t alt = hall_offset_;
  hall_offset_ = normiere(schritte, g::REV);
  nvs.putInt(K_OFFSET, hall_offset_);
  // Jede Aenderung kommt in den Verlauf - am 05.10.2026 war nicht mehr
  // festzustellen, wann der falsche Offset entstanden war.
  verlauf_eintragen(Hominganlass::offset, nullptr, false,
                    drift(hall_offset_, alt, g::REV));
}

void Betrieb::verlauf_eintragen(Hominganlass anlass, const Homingergebnis *e,
                                bool vergleichbar, int32_t abweichung) {
  Homingeintrag v;
  const time_t jetzt = time(nullptr);
  v.zeit = jetzt > 1700000000 ? static_cast<uint32_t>(jetzt) : 0;  // vor 2023: keine Zeit
  v.laufzeit_s = millis() / 1000u;
  v.anlass = static_cast<uint8_t>(anlass);
  v.vergleichbar = vergleichbar ? 1 : 0;
  v.abweichung = static_cast<int16_t>(abweichung);
  v.haltestrom_ma = antrieb_ != nullptr ? antrieb_->haltestrom() : 0;
  v.offset = static_cast<int16_t>(hall_offset_);
  if (e != nullptr) {
    v.grund = static_cast<uint8_t>(e->grund);
    v.feldbreite = static_cast<int16_t>(e->fenster);
    v.ruhe_ut = static_cast<uint16_t>(lroundf(fmaxf(e->ruhe_mt, 0.0f) * 1000.0f));
    v.spanne_ut = static_cast<uint16_t>(lroundf(fminf(fmaxf(e->spanne_mt, 0.0f), 65.0f) * 1000.0f));
  }
  verlauf_.hinzu(v);
  Preferences p;
  if (p.begin(NVS_VERLAUF, false)) {
    p.putBytes(K_VERLAUF, verlauf_.abbild(), Homingverlauf::abbild_groesse());
    p.end();
  }
}

std::string Betrieb::homingeintrag_text(const Homingeintrag &e) const {
  char wann[24];
  if (e.zeit != 0) {
    const time_t t = static_cast<time_t>(e.zeit);
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(wann, sizeof(wann), "%d.%m. %H:%M", &lt);
  } else {
    snprintf(wann, sizeof(wann), "Lauf %5lus ", (unsigned long)e.laufzeit_s);
  }
  const Hominganlass anlass = static_cast<Hominganlass>(e.anlass);
  char z[200];
  if (anlass == Hominganlass::offset) {
    snprintf(z, sizeof(z), "%s  %-9s  %ld -> %ld (%+d)  Halt %u mA", wann,
             hominganlass_text(anlass),
             (long)normiere(e.offset - e.abweichung, g::REV), (long)e.offset,
             (int)e.abweichung, (unsigned)e.haltestrom_ma);
    return z;
  }
  const Hominggrund grund = static_cast<Hominggrund>(e.grund);
  if (grund != Hominggrund::ok) {
    snprintf(z, sizeof(z), "%s  %-9s  %s  Halt %u mA", wann, hominganlass_text(anlass),
             hominggrund_text(grund), (unsigned)e.haltestrom_ma);
    return z;
  }
  char abw[48];
  if (e.vergleichbar) {
    const Verlustbefund b =
        schrittverlust(e.abweichung, g::SCHRITTE_JE_PERIODE, g::HOMING_STREUUNG);
    snprintf(abw, sizeof(abw), "Abw %+5d%s", (int)e.abweichung,
             b.periodisch ? "  SCHRITTVERLUST"
                          : (b.verlust ? "  ABWEICHUNG" : ""));
    if (b.periodisch) {
      const size_t l = strlen(abw);
      snprintf(abw + l, sizeof(abw) - l, " %+ld Per.", (long)b.perioden);
    }
  } else {
    snprintf(abw, sizeof(abw), "Abw    --");
  }
  snprintf(z, sizeof(z), "%s  %-9s  OK  %s  Halt %u mA  Breite %d  Ruhe %.2f  Spanne %.2f",
           wann, hominganlass_text(anlass), abw, (unsigned)e.haltestrom_ma,
           (int)e.feldbreite, e.ruhe_ut / 1000.0, e.spanne_ut / 1000.0);
  return z;
}

bool Betrieb::hier_ist_zwoelf() {
  if (antrieb_ == nullptr)
    return false;

  // Wie jeder Handeingriff: erst die Uhr aus der Fuehrung nehmen. Sonst
  // rechnet der Offset mit einer Position, die die Uhr im naechsten Takt
  // schon weitergeschoben hat.
  const bool war_an = hand_uebernimmt();

  // Die Rechnung steht in lib/zeiger und ist host-getestet - sie gehoert
  // NICHT hierher. Genau an dieser Stelle stand zuerst eine selbstgebaute
  // Formel mit falschem Vorzeichen, und weil sie hier stand, konnte kein
  // Host-Test sie fangen. Das ist der Grund fuer die Trennung.
  offset_setzen(offset_einmessen(hall_offset_, antrieb_->position(), g::REV));

  // Der Zeiger zeigt jetzt zwoelf, also ist die Position null.
  antrieb_->zaehler_setzen(0);

  // GESPEICHERT HEISST FERTIG (23.10.2026): sofort zurueck zur Uhrzeit, statt
  // auf zwoelf stehen zu bleiben. Die Frist gilt als abgelaufen - der
  // naechste tick() uebergibt an den Uhrbetrieb.
  rueckfall_freigeben();
  letzte_handlung_ms_ = millis() - rueckfall_frist_ms_;
  return war_an;
}

Homingergebnis Betrieb::referenzfahrt(Hall::Pumpe pumpe, Hominganlass anlass) {
  if (antrieb_ == nullptr || hall_ == nullptr) {
    letztes_homing_ = Homingergebnis();
    return letztes_homing_;
  }

  // Ist die Abweichung, die diese Fahrt misst, ein Verlust? Nur wenn vorher
  // eine Referenz galt, die Endstufe seitdem nie im Freilauf war und der
  // Offset nicht von Hand verschoben wurde.
  const bool vergleichbar = referenz_gueltig_ && !offset_von_hand_ &&
                            antrieb_->freilaeufe() == freilaeufe_bei_referenz_;

  // Timer und Uhrbetrieb aus: die Referenzfahrt hat Vorrang, und wer ihr in
  // den Zaehler schreibt, macht das Ergebnis wertlos. Im Pruefstand steht
  // diese Regel als Hinweistext an der Oberflaeche.
  timer_abbrechen();
  const Zeitmodus vorher = zeitmodus_;
  zeitmodus_ = Zeitmodus::aus;

  // Klinge merken und ausschalten. Gemerkt wird der ISTZUSTAND, nicht der
  // Reglerwert - sonst ginge sie nach der Fahrt an, obwohl sie vorher aus war.
  if (licht_ != nullptr) {
    licht_war_an_ = licht_->an_dauerhaft();  // ein Puls endet hier - der Zustand davor
    licht_->effekt_abbrechen();
    licht_->an(false);
    // Auch eine MELDUNG ruht waehrend der Fahrt: der Zeiger wischt uebers
    // Blatt, und ob der LED-Strom den Hall-Sensor stoert, ist nicht gemessen.
    licht_->meldung_pausieren(true);
  }

  homing_laeuft_ = true;
  letztes_homing_ = hall_->referenzfahrt(*antrieb_, hall_offset_, pumpe);
  homing_laeuft_ = false;

  // WO DER MAGNET NACH DER ZAEHLUNG HAETTE SEIN MUESSEN: die Referenzfahrt
  // setzt den Zaehler so, dass die Magnetmitte auf `offset` liegt. Der
  // Schwerpunkt ist noch im ALTEN Zaehler gemessen - die Differenz ist, was
  // seit der letzten Referenz verloren wurde (positiv: der Motor ist weniger
  // weit gedreht als gezaehlt).
  letzte_vergleichbar_ = false;
  letzte_abweichung_ = 0;
  if (letztes_homing_.ok()) {
    letzte_abweichung_ = drift(static_cast<int32_t>(lroundf(letztes_homing_.schwerpunkt)),
                               hall_offset_, g::REV);
    letzte_vergleichbar_ = vergleichbar;
    freilaeufe_bei_referenz_ = antrieb_->freilaeufe();
    offset_von_hand_ = false;
  }
  verlauf_eintragen(anlass, &letztes_homing_, letzte_vergleichbar_, letzte_abweichung_);

  // ---------------------------------------------------------------------
  // Gilt die Referenz danach?
  // ---------------------------------------------------------------------
  // Gelungen heisst: ja. Gescheitert heisst NICHT immer nein - es kommt
  // darauf an, ob der Zeiger sich dabei bewegt hat:
  //
  //   kein_sensor    bricht ab, BEVOR ein Schritt kommandiert wird. Eine
  //                  vorher gueltige Referenz bleibt damit gueltig - sonst
  //                  wuerde ein ausgefallener Sensor eine laufende Uhr
  //                  anhalten, obwohl der Zeiger unveraendert steht.
  //   alles andere   hat gesucht, zurueckgezogen oder fein angefahren und
  //                  den Zaehler dabei verlassen. Die alte Referenz ist weg.
  if (letztes_homing_.ok())
    referenz_gueltig_ = true;
  else if (letztes_homing_.grund != Hominggrund::kein_sensor)
    referenz_gueltig_ = false;

  // ---------------------------------------------------------------------
  // NACH DEM HOMING AUF ZWOELF FAHREN.
  //
  // Ohne das bleibt der Zeiger stehen, wo die Feinanfahrt endete, und nur der
  // Zaehler wird gesetzt - er steht dann auf (Offset + Nachlauf), also rund
  // 360. Rechnerisch richtig, aber es SIEHT aus, als haette die Referenzfahrt
  // nicht funktioniert: der Zeiger steht jedes Mal woanders, weil die
  // Grobsuche jedes Mal woanders begann.
  //
  // Genau das ist der Punkt, an dem der Projektinhaber am 22.08.2026 gemeldet
  // hat, das Homing funktioniere nicht. Die Kalibrierung war in Ordnung, die
  // Rueckmeldung nicht.
  //
  // Mit dieser Fahrt zeigt der Zeiger danach auf zwoelf - und man SIEHT
  // sofort, ob die Kalibrierung stimmt. Das ist auch der Schritt, den AP 0.3
  // zum Einmessen braucht.
  // ---------------------------------------------------------------------
  if (letztes_homing_.ok() && antrieb_ != nullptr) {
    fahre_auf(0);
    // Nicht warten: der Motor laeuft in Hardware, der Aufrufer sieht die
    // Fahrt in der Zustandsanzeige. Ein blockierendes Warten hier haette die
    // Konsole fuer bis zu 4 s stillgelegt.
  }

  // Klinge wiederherstellen - AUCH wenn die Fahrt abgebrochen ist. Sonst
  // bleibt das Licht dunkel und man sucht den Fehler beim Licht.
  if (licht_ != nullptr && licht_war_an_)
    licht_->an(true);
  if (licht_ != nullptr)
    licht_->meldung_pausieren(false);

  zeitmodus_ = vorher;
  uhr_letzter_tick_ = 0;
  return letztes_homing_;
}

// ===========================================================================
// Timer
// ===========================================================================

void Betrieb::timer_ende_merken(uint32_t epoch_ende) {
  timer_ende_epoch_ = epoch_ende;
  nvs_ben.putUInt(K_TIMER_ENDE, epoch_ende);
}

int32_t Betrieb::timer_wiederaufnehmen(uint32_t epoch_jetzt) {
  const uint32_t ende = nvs_ben.getUInt(K_TIMER_ENDE, 0);
  if (ende == 0 || epoch_jetzt == 0)
    return 0;
  if (ende <= epoch_jetzt) {
    // Waehrend der Auszeit abgelaufen. Nicht nachtraeglich ausloesen - ein
    // Wecker, der Stunden zu spaet klingelt, ist eine Falschmeldung.
    timer_ende_merken(0);
    return -1;
  }
  const uint32_t rest = ende - epoch_jetzt;
  timer_ende_epoch_ = ende;
  // Ohne erneutes Merken starten: das Ende steht schon und darf sich durch
  // die Wiederaufnahme NICHT verschieben.
  if (!timer_starten(rest, 0))
    return 0;
  timer_ende_epoch_ = ende;
  return static_cast<int32_t>(rest);
}

bool Betrieb::timer_starten(uint32_t sekunden, uint32_t epoch_jetzt) {
  if (antrieb_ == nullptr || sekunden == 0)
    return false;

  // Der Uhrbetrieb wird ausgeschaltet: beide brauchen den Zeiger als Anzeige,
  // und die Kollisionsregel sagt, dass die letzte Aktion gewinnt.
  zeitmodus_ = Zeitmodus::aus;
  timer_dauer_ms_ = sekunden * 1000u;
  if (epoch_jetzt > 0)
    timer_ende_merken(epoch_jetzt + sekunden);

  // Auf die naechste Zwoelf-Uhr-Lage fahren, vorwaerts.
  const int32_t ziel = antrieb_->ziel();
  timer_start_pos_ = ziel + delta_vorwaerts(normiere(ziel, g::REV), 0, g::REV);
  // MIT FAHRTEMPO, nicht mit der halben Geschwindigkeit: diese Anfahrt laeuft
  // VOR dem Start der Messung, und bei 400 Schritten/s dauerte sie bis zu 8 s.
  // Bei 't 1' sah das aus wie ein Timer, der um den Faktor 9 falsch liegt.
  antrieb_->tempo(fahrtempo_);
  antrieb_->fahre_weich_zu(timer_start_pos_);
  timer_gleitet_ = false;
  // DIE KLINGE ZEIGT DEN TIMER AN, auch in der Farbe - bis der Alarm vorbei
  // ist (Betrieb::tick) oder abgebrochen wird.
  if (licht_ != nullptr)
    licht_->farbe_vorrang(true, g::TIMER_FARBE);

  // LICHT AUS FUER DIE LEERFAHRT. Der Weg auf zwoelf gehoert nicht zur
  // Anzeige - er ist Vorbereitung. Eine leuchtende Klinge, die dabei einmal
  // quer ueber das Blatt wischt, sagt etwas, das nicht stimmt. Der Zustand
  // wird gemerkt und beim Start des Laufs wiederhergestellt.
  if (licht_ != nullptr) {
    licht_vor_leerfahrt_ = licht_->an_dauerhaft();  // der Timer beendet einen Puls
    licht_gemerkt_ = true;
    licht_->an(false);
    licht_->meldung_pausieren(true);  // dieselbe Regel gilt fuer eine Meldung
  }

  timer_phase_ = Timerphase::auf_zwoelf;
  timer_phase_ab_ms_ = millis();
  return true;
}

void Betrieb::timer_abbrechen() {
  if (timer_phase_ == Timerphase::aus)
    return;
  timer_phase_ = Timerphase::aus;
  timer_dauer_ms_ = 0;
  timer_gleitet_ = false;
  timer_ende_merken(0);
  if (antrieb_ != nullptr) {
    antrieb_->stopp_mit_rampe();
    antrieb_->tempo(fahrtempo_);
  }
  if (licht_ != nullptr)
    licht_->farbe_vorrang(false);
  // Auch ein Abbruch mitten in der Leerfahrt muss das Licht zurueckgeben.
  if (licht_gemerkt_ && licht_ != nullptr) {
    licht_->an(licht_vor_leerfahrt_);
    licht_->meldung_pausieren(false);
    licht_gemerkt_ = false;
  }
  handlung_gesehen();
}

const char *Betrieb::timer_zustand_text() const {
  switch (timer_phase_) {
    case Timerphase::aus:        return "aus";
    case Timerphase::auf_zwoelf: return "faehrt auf zwoelf";
    case Timerphase::countdown:  return "Countdown";
    case Timerphase::laeuft:     return "laeuft";
    case Timerphase::endfahrt:   return "Endfahrt";
  }
  return "aus";
}

std::string Betrieb::timer_restzeit() const {
  if (timer_phase_ == Timerphase::aus)
    return "--:--:--";
  // Waehrend der Anfahrt laeuft die Messung noch NICHT. Vorher stand hier die
  // volle Dauer, als zaehle sie schon herunter - genau daran war 't 1' nicht
  // als Anfahrt zu erkennen.
  if (timer_phase_ == Timerphase::auf_zwoelf)
    return "noch nicht gestartet";
  // Im Countdown ist timer_ab_ms_ noch der Wert des VORIGEN Laufs - die
  // Restzeit daraus waere Unsinn. Genau das stand hier zuerst: waehrend des
  // Countdowns meldete der Zustand "Timer aus, Rest 0:00:00", weil auch der
  // Phasentext die neue Phase nicht kannte und auf "aus" durchfiel.
  if (timer_phase_ == Timerphase::countdown) {
    const uint32_t seit = millis() - timer_phase_ab_ms_;
    const uint32_t ganz = g::TIMER_COUNTDOWN_S * 1000u;
    const uint32_t rest = seit >= ganz ? 0u : (ganz - seit);
    char puffer[24];
    snprintf(puffer, sizeof(puffer), "Start in %lu s",
             static_cast<unsigned long>(rest / 1000u + 1u));
    return std::string(puffer);
  }
  return starwatch::timer_restzeit(millis() - timer_ab_ms_, timer_dauer_ms_);
}

void Betrieb::timer_tick() {
  if (antrieb_ == nullptr)
    return;

  switch (timer_phase_) {
    case Timerphase::aus:
      return;

    case Timerphase::auf_zwoelf:
      if (antrieb_->ziel_erreicht()) {
        // Angekommen. Jetzt NICHT sofort loslaufen, sondern drei Sekunden
        // herunterblinken - sonst beginnt der Lauf, bevor jemand hinsieht.
        timer_phase_ = Timerphase::countdown;
        timer_phase_ab_ms_ = millis();
        // Die Leerfahrt ist vorbei, der Lauf ist die Anzeige: Licht zurueck.
        if (licht_gemerkt_ && licht_ != nullptr) {
          licht_->an(licht_vor_leerfahrt_);
          licht_->meldung_pausieren(false);
          licht_gemerkt_ = false;
        }
        if (licht_ != nullptr)
          licht_->countdown(static_cast<int>(g::TIMER_COUNTDOWN_S));
      } else if (millis() - timer_phase_ab_ms_ > TIMER_ANFAHRT_TIMEOUT_MS) {
        timer_abbrechen();
      }
      return;

    case Timerphase::countdown:
      // Der Zeiger steht auf zwoelf und wartet. Erst wenn die drei Sekunden
      // um sind, faengt die gemessene Zeit an - die Blitze gehoeren NICHT zur
      // Timerdauer, sonst waere ein Timer von 5 s in Wirklichkeit 8 s.
      if (millis() - timer_phase_ab_ms_ >= g::TIMER_COUNTDOWN_S * 1000u) {
        timer_ab_ms_ = millis();
        timer_phase_ = Timerphase::laeuft;
        timer_phase_ab_ms_ = millis();
        timer_gleitet_ = false;
        timer_sekunde_ = 0;
      }
      return;

    case Timerphase::laeuft: {
      const uint32_t verstrichen = millis() - timer_ab_ms_;
      if (verstrichen >= timer_dauer_ms_) {
        // Die letzte Strecke in einem Zug, damit der Zeiger sicher auf zwoelf
        // ankommt und nicht ein paar Schritte davor stehenbleibt. VORHER das
        // Fahrtempo zurueck: nach einer Gleitfahrt gilt noch deren Tempo, und
        // beim 12-h-Timer braeuchte ein einziger fehlender Schritt damit
        // 13,5 s bis zum Alarm.
        antrieb_->tempo(fahrtempo_);
        antrieb_->fahre_weich_zu(timer_start_pos_ + g::REV);
        timer_gleitet_ = false;
        timer_phase_ = Timerphase::endfahrt;
        timer_phase_ab_ms_ = millis();
        return;
      }
      if (timerlauf_ == Timerlauf::sekundentakt)
        timer_springen(verstrichen);
      else
        timer_gleiten(verstrichen);
      return;
    }

    case Timerphase::endfahrt:
      if (antrieb_->ziel_erreicht() ||
          millis() - timer_phase_ab_ms_ > TIMER_ENDFAHRT_TIMEOUT_MS) {
        timer_phase_ = Timerphase::aus;
        timer_dauer_ms_ = 0;
        antrieb_->tempo(fahrtempo_);
        // Alarm: der Zeiger wird dabei BEWUSST nicht bewegt - er steht auf
        // zwoelf, und das ist die Aussage.
        if (licht_ != nullptr)
          licht_->alarm();
        timer_ende_merken(0);
        // Der Timer ist durch. Die Frist laeuft an, danach nimmt die Uhr den
        // Zeiger zurueck - er soll nicht auf zwoelf stehenbleiben und eine
        // Zeit behaupten, die nicht stimmt. NACH EINEM TIMER kuerzer als
        // sonst: drei Sekunden statt zehn.
        rueckfall_frist_ms_ = g::RUECKFALL_TIMER_MS;
        handlung_gesehen();
        rueckfall_freigeben();
      }
      return;
  }
}

/// GLEITEND: eine einzige Fahrt bis zum Ende, mit dem Tempo, das genau eine
/// Umdrehung in der Timerdauer ergibt. Die Impulse kommen gleichmaessig aus
/// der Hardware; der Treiber interpoliert dazu auf 1/256 (intpol).
void Betrieb::timer_gleiten(uint32_t verstrichen_ms) {
  const int32_t ende = timer_start_pos_ + g::REV;
  const int32_t soll =
      timer_ziel(timer_start_pos_, verstrichen_ms, timer_dauer_ms_, g::REV);

  if (!timer_gleitet_) {
    // Beginn des Laufs - oder Umschalten aus dem Sekundentakt.
    const uint32_t mhz = timer_gleittempo_mhz(
        g::REV, timer_dauer_ms_, soll - antrieb_->position(), TIMER_AUSGLEICH_MS,
        fahrtempo_);
    if (mhz == 0 || !antrieb_->tempo_mhz(mhz)) {
      // Langsamer als FastAccelStepper kann (Timer ueber rund 13,9 h): dann
      // eben im Sekundentakt. Einen Schritt je 15 s sieht ohnehin niemand
      // gleiten.
      timer_springen(verstrichen_ms);
      return;
    }
    antrieb_->fahre_zu(ende);
    timer_gleitet_ = true;
    timer_gleit_mhz_ = mhz;
    timer_nachgefuehrt_ms_ = millis();
    return;
  }

  if (millis() - timer_nachgefuehrt_ms_ < TIMER_NACHFUEHREN_MS)
    return;
  timer_nachgefuehrt_ms_ = millis();

  // Nur schreiben, wenn sich etwas aendert. Bei langen Timern liegt ein
  // Schritt 13,5 s in der Warteschlange, und die soll nicht jede Sekunde
  // neu gerechnet werden.
  const uint32_t mhz = timer_gleittempo_mhz(
      g::REV, timer_dauer_ms_, soll - antrieb_->position(), TIMER_AUSGLEICH_MS,
      fahrtempo_);
  if (mhz != timer_gleit_mhz_ && antrieb_->tempo_mhz(mhz))
    timer_gleit_mhz_ = mhz;
}

/// SEKUNDENTAKT: eine Sekunde stehen, dann mit Fahrtempo und Rampe auf das
/// Ziel dieser Sekunde springen. Beim 5-min-Timer 11 Schritte (1,2 Grad) in
/// rund 30 ms - ein kurzer, bewusster Sprung statt vier kleiner Rucke.
void Betrieb::timer_springen(uint32_t verstrichen_ms) {
  const uint32_t sekunde = verstrichen_ms / 1000u;
  const int32_t ziel =
      timer_sprungziel(timer_start_pos_, verstrichen_ms, timer_dauer_ms_, g::REV);

  if (timer_gleitet_) {
    // Umgeschaltet, waehrend er gleitet. Sein Ziel ist noch das Ende der
    // Umdrehung - ohne neues Ziel liefe er dorthin einfach weiter. Also
    // Fahrtempo zurueck und dort anhalten, wo er ist; er steht dann hoechstens
    // eine Sekunde vor dem Takt und wartet, bis der ihn einholt.
    timer_gleitet_ = false;
    antrieb_->tempo(fahrtempo_);
    const int32_t ist = antrieb_->position();
    antrieb_->fahre_weich_zu(ziel > ist ? ziel : ist);
    timer_sekunde_ = sekunde;
    return;
  }

  if (sekunde == timer_sekunde_)
    return;
  timer_sekunde_ = sekunde;
  // Nur vorwaerts: nie ein Ziel setzen, das hinter dem aktuellen liegt.
  if (ziel > antrieb_->ziel())
    antrieb_->fahre_weich_zu(ziel);
}

// ===========================================================================
// Fahren von Hand
// ===========================================================================

/// Gemeinsamer Teil jedes Handeingriffs: den Uhrbetrieb beenden. Steht hier
/// EINMAL, damit kein Fahrweg von aussen daran vorbeikommt - die Begruendung
/// steht bei den Deklarationen in betrieb.h.
bool Betrieb::hand_uebernimmt() {
  if (zeitmodus_ == Zeitmodus::aus)
    return false;
  zeitmodus_ = Zeitmodus::aus;
  return true;
}

void Betrieb::fahrtempo(uint32_t schritte_je_s) {
  if (schritte_je_s == 0)
    return;
  fahrtempo_ = schritte_je_s;
}

void Betrieb::effekt_kreise(int32_t n) {
  if (n < 0)
    n = 0;
  if (n > g::EFFEKT_KREISE_MAX)
    n = g::EFFEKT_KREISE_MAX;
  effekt_kreise_ = n;
}

bool Betrieb::fahre_auf(int32_t position, int32_t kreise) {
  if (antrieb_ == nullptr)
    return false;
  // Ein Anfahren ist eine Episode, kein Einmessen: die Frist laeuft an, und
  // eine Sperre vom Verstellen wird damit aufgehoben.
  rueckfall_freigeben();
  handlung_gesehen();
  const bool war_an = hand_uebernimmt();
  const int32_t ziel = antrieb_->ziel();
  const int32_t delta =
      delta_vorwaerts(normiere(ziel, g::REV), normiere(position, g::REV), g::REV);
  if (kreise < 0)
    kreise = 0;
  antrieb_->tempo(fahrtempo_ > 0 ? fahrtempo_ : g::TEMPO_FAHRT);
  // Die Kreise werden dem Weg VORANGESTELLT, nicht hinterher gefahren: so
  // ist es eine Fahrt mit einer Rampe, und der Zeiger kommt aus dem Schwung
  // heraus auf dem Ziel zur Ruhe.
  antrieb_->fahre_weich_zu(ziel + kreise * g::REV + delta);
  return war_an;
}

bool Betrieb::steuere_auf(int32_t position) {
  if (antrieb_ == nullptr)
    return false;
  // Wie ein Anfahren eine Episode: die Frist laeuft an, der Rueckfall holt den
  // Uhrbetrieb von selbst zurueck, sobald das Steuern aufhoert.
  rueckfall_freigeben();
  handlung_gesehen();
  const bool war_an = hand_uebernimmt();
  const int32_t ziel = antrieb_->ziel();
  // Vorzeichenbehafteter kuerzester Weg, bezogen auf das laufende ZIEL - nicht
  // auf eine normierte Position. FastAccelStepper bekommt den fortlaufenden
  // Zaehler, und der Umlauf von 3199 auf 0 waere sonst eine ganze Umdrehung
  // rueckwaerts statt ein Schritt vorwaerts.
  const int32_t delta = drift(position, ziel, g::REV);
  if (delta == 0)
    return war_an;  // nichts zu tun - und kein moveTo, das die Rampe neu setzt
  // Eigenes Tempo, NICHT fahrtempo_: siehe g::TEMPO_STEUERN. Ein 'f' von
  // aussen gilt den Handfahrten und soll das Steuern nicht hart machen.
  antrieb_->tempo(g::TEMPO_STEUERN);
  antrieb_->fahre_zu(ziel + delta);
  return war_an;
}

bool Betrieb::verstelle_um(int32_t schritte) {
  if (antrieb_ == nullptr)
    return false;
  // Verstellen ist das Werkzeug zum EINMESSEN. Hier darf der Uhrbetrieb nicht
  // nach zehn Sekunden zugreifen - sonst waere der Offset falsch.
  rueckfall_sperren();
  const bool war_an = hand_uebernimmt();
  antrieb_->tempo(fahrtempo_ > 0 ? fahrtempo_ : g::TEMPO_FAHRT);
  antrieb_->fahre_weich_weiter(schritte);
  return war_an;
}

bool Betrieb::auf_zwoelf_halten() {
  const bool war_an = fahre_auf(0, 0);
  // fahre_auf gibt den Rueckfall frei - hier gilt das Einmessen, wie bei 'v'.
  rueckfall_sperren();
  return war_an;
}

void Betrieb::stopp() {
  timer_abbrechen();
  zeitmodus_ = Zeitmodus::aus;
  if (antrieb_ != nullptr)
    antrieb_->stopp_mit_rampe();
}

Fuehrung Betrieb::fuehrung() const {
  const bool hand = antrieb_ != nullptr && antrieb_->laeuft() &&
                    timer_phase_ == Timerphase::aus &&
                    zeitmodus_ == Zeitmodus::aus;
  return fuehrung_bestimmen(homing_laeuft_, hand, timer_laeuft(),
                            zeitmodus_ != Zeitmodus::aus);
}

// ===========================================================================
// Hauptschleife
// ===========================================================================

void Betrieb::uhr_stellen() {
  if (antrieb_ == nullptr)
    return;

  const int32_t ziel = soll_jetzt();
  if (ziel < 0)
    return;

  // Bezug ist das ZIEL, nicht die Istposition: laeuft gerade eine Fahrt, wird
  // ihr Ziel weitergedreht statt sie abzuschneiden.
  const int32_t ist = antrieb_->ziel();
  const int32_t delta = delta_vorwaerts(normiere(ist, g::REV), ziel, g::REV);

  if (delta == 0)
    return;

  // Die Uhr faehrt mit ihrem eigenen, leisen Tempo - nicht mit dem der
  // Handfahrten. Warum, steht bei TEMPO_UHR in geraet.h.
  //
  // AUSSER BEIM AUFHOLEN (27.09.2026): nach Timer, Handfahrt oder Start steht
  // der Zeiger weit weg, und im Uhrtempo dauerte die Rueckkehr bis zu 8 s -
  // "sollte schnellstmoeglich dort hin springen". Ab AUFHOLEN_AB fahrt die
  // Uhr mit dem eingestellten Fahrtempo. Die Minutenschritte (4,4 Schritte)
  // bleiben leise. Und NIE waehrend einer laufenden Fahrt herunterschalten:
  // wechselt mitten im Aufholen die Minute, bremste sonst die ganze Fahrt.
  if (delta >= AUFHOLEN_AB)
    antrieb_->tempo(fahrtempo_ > 0 ? fahrtempo_ : g::TEMPO_FAHRT);
  else if (!antrieb_->laeuft())
    antrieb_->tempo(g::TEMPO_UHR);

  // Steht der Zeiger ein paar Schritte VOR der Zeit, waere delta fast eine
  // ganze Umdrehung. Dann ist Stehenbleiben richtig, bis die Zeit aufgeholt
  // hat - eine Umdrehung nachzufahren waere ein Richtungswechsel im Ergebnis.
  if (warten_statt_umrunden(delta, g::REV, VORLAUF_TOLERANZ))
    return;

  antrieb_->fahre_weich_zu(ist + delta);
}

// ===========================================================================
// Rueckfall in den Uhrbetrieb
// ===========================================================================

void Betrieb::handlung_gesehen() { letzte_handlung_ms_ = millis(); }

void Betrieb::rueckfall_sperren() {
  rueckfall_gesperrt_ = true;
  letzte_handlung_ms_ = millis();
}

void Betrieb::rueckfall_freigeben() { rueckfall_gesperrt_ = false; }

void Betrieb::rueckfall_sofort() { letzte_handlung_ms_ = millis() - rueckfall_frist_ms_; }

uint32_t Betrieb::rueckfall_rest_ms() const {
  if (rueckfall_gesperrt_ || rueckfall_modus_ == Zeitmodus::aus ||
      zeitmodus_ != Zeitmodus::aus || !zeit_gueltig_)
    return 0;
  const uint32_t seit = millis() - letzte_handlung_ms_;
  return seit >= rueckfall_frist_ms_ ? 0 : (rueckfall_frist_ms_ - seit);
}

void Betrieb::rueckfall_pruefen() {
  // Der Rueckfall greift nur, wenn wirklich nichts anderes laeuft. Jede
  // einzelne dieser Bedingungen hat einen Grund:
  if (rueckfall_gesperrt_)
    return;                       // Einmessen laeuft - siehe RUECKFALL_MS
  if (zeitmodus_ != Zeitmodus::aus)
    return;                       // die Uhr fuehrt schon
  if (rueckfall_modus_ == Zeitmodus::aus)
    return;                       // der Bediener will sie ausdruecklich aus
  if (!zeit_gueltig_)
    return;                       // ohne Zeit gibt es keine Sollposition
  if (!referenz_gueltig_)
    return;                       // ohne Referenz waere die Position geraten
  if (timer_phase_ != Timerphase::aus)
    return;                       // ein Timer haelt den Zeiger
  if (antrieb_ == nullptr || antrieb_->laeuft())
    return;                       // erst ankommen, dann uebernehmen
  if (millis() - letzte_handlung_ms_ < rueckfall_frist_ms_)
    return;                       // die Frist laeuft noch

  zeitmodus_ = rueckfall_modus_;
  rueckfall_geschehen_ = true;
  // Die kurze Frist gilt EINMAL, direkt nach dem Timer. Danach wieder die
  // normale - sonst griffe die Uhr auch beim Einmessen nach drei Sekunden zu.
  rueckfall_frist_ms_ = g::RUECKFALL_MS;
}

void Betrieb::stundeneffekt_pruefen() {
  if (!effekt_stunde_ || !zeit_gueltig_ || licht_ == nullptr)
    return;
  const int32_t s = tageszeit_sek();
  const int stunde = static_cast<int>(s / 3600);
  if (stunde == effekt_letzte_stunde_)
    return;                       // in dieser Stunde war er schon

  // DIE STUNDE WIRD IN JEDEM FALL VERMERKT, auch wenn nicht ausgeloest wird.
  // Sonst versuchte es der Takt eine ganze Stunde lang immer wieder.
  const bool erster_lauf = (effekt_letzte_stunde_ < 0);
  effekt_letzte_stunde_ = stunde;

  if (erster_lauf)
    return;   // beim Start nur merken - sonst blitzt es bei jedem Neustart
  // NICHT auf s % 3600 == 0 pruefen: das ist genau EINE Sekunde je Stunde,
  // und ein blockierter Takt laesst sie ausfallen. Fuenf Sekunden Fenster.
  // Liegt der Wechsel weiter zurueck (Uhr gerade erst gestellt, Netz war
  // weg), ist der Zug abgefahren - dann lieber gar nicht als zwanzig
  // Minuten zu spaet.
  if (s % 3600 >= 5)
    return;
  // Nur wenn die Klinge ueberhaupt brennt und nichts anderes sie fuehrt.
  // Ein Effekt mitten im Timeralarm waere ein Fehlalarm.
  if (!licht_->ist_an() || timer_phase_ != Timerphase::aus || licht_->puls_aktiv())
    return;
  licht_->stundeneffekt();
}

void Betrieb::tick() {
  if (licht_ != nullptr)
    licht_->tick();
  if (hall_ != nullptr && antrieb_ != nullptr)
    hall_->tick(antrieb_->position());

  timer_tick();
  // Timerfarbe zuruecknehmen, sobald Timer UND Alarm vorbei sind. Der Alarm
  // ist ein Effekt; solange er laeuft, bleibt die Klinge orange.
  if (licht_ != nullptr && licht_->farbe_vorrang_an() &&
      timer_phase_ == Timerphase::aus && !licht_->effekt_laeuft())
    licht_->farbe_vorrang(false);
  rueckfall_pruefen();
  stundeneffekt_pruefen();

  // Waehrend eines Timerlaufs stellt die Uhr nicht - sonst zerren zwei
  // Automatiken am selben Zeiger.
  if (timer_phase_ == Timerphase::aus && zeitmodus_ != Zeitmodus::aus) {
    if (millis() - uhr_letzter_tick_ >= UHR_TAKT_MS || uhr_letzter_tick_ == 0) {
      uhr_letzter_tick_ = millis();
      uhr_stellen();
    }
  }
}

}  // namespace starwatch
