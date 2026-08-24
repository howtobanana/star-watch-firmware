#include "betrieb.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <cstring>
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

/// Takt, in dem der Timer sein Ziel weiterdreht - OBERGRENZE. Bei fuenf
/// Minuten sind das rund 2,7 Schritte je Schub, bei zwoelf Stunden einer alle
/// 13,5 s.
constexpr uint32_t TIMER_TAKT_MAX_MS = 250;

/// ... und Untergrenze. Schneller als das braucht kein Ziel nachgesetzt zu
/// werden; darunter frisst der Takt nur Rechenzeit.
constexpr uint32_t TIMER_TAKT_MIN_MS = 20;

/// So viele Zielschuebe soll ein Timerlauf mindestens bekommen.
///
/// WARUM ADAPTIV: mit den festen 250 ms bekam ein Timer von 1 s genau VIER
/// Schuebe zu je 800 Schritten. Jeder davon wurde einzeln beschleunigt und
/// abgebremst - vier Rucke, keine Fahrt. Bei langen Laufzeiten ist der
/// gleiche Takt dagegen richtig, weil dort ohnehin nur alle paar Sekunden ein
/// Schritt anfaellt.
constexpr uint32_t TIMER_SCHUEBE = 200;

/// Takt fuer eine gegebene Laufzeit. Gerechnet, nicht getippt.
uint32_t timer_takt_ms(uint32_t dauer_ms) {
  const uint32_t takt = dauer_ms / TIMER_SCHUEBE;
  if (takt < TIMER_TAKT_MIN_MS)
    return TIMER_TAKT_MIN_MS;
  if (takt > TIMER_TAKT_MAX_MS)
    return TIMER_TAKT_MAX_MS;
  return takt;
}

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
/// Zaehler der Stromzyklen. Im FERTIGUNGSbereich, damit ihn ein Werksreset
/// nicht selbst loescht - sonst zaehlte der Reset seinen eigenen Zaehler weg.
const char *K_ZYKLEN = "zyklen";
const char *K_TEMPO = "tempo";
const char *K_RAMPE = "rampe";
const char *K_KREISE = "kreise";
const char *K_MODUS = "modus";
const char *K_LICHT = "licht";
const char *K_LICHT_AN = "licht_an";
const char *K_TZ = "tz";
const char *K_TIMER_ENDE = "t_ende";

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

void Betrieb::beginn(Antrieb *antrieb, Hall *hall, Licht *licht) {
  antrieb_ = antrieb;
  hall_ = hall;
  licht_ = licht;

  nvs.begin(NVS_FERTIGUNG, false);
  hall_offset_ = nvs.getInt(K_OFFSET, 0);
  identitaet_laden();

  // Benutzerwerte: die Vorgaben aus geraet.h sind der Rueckfall, nicht der
  // gesetzte Wert - was einmal eingestellt wurde, ueberlebt den Neustart.
  nvs_ben.begin(NVS_BENUTZER, false);
  fahrtempo_ = nvs_ben.getUInt(K_TEMPO, g::TEMPO_FAHRT);
  effekt_kreise_ = nvs_ben.getInt(K_KREISE, 0);
  const uint32_t rampe = nvs_ben.getUInt(K_RAMPE, g::BESCHLEUNIGUNG);
  const uint8_t modus = nvs_ben.getUChar(K_MODUS, g::MODUS_VORGABE);
  const float licht_p = nvs_ben.getFloat(K_LICHT, g::LICHT_VORGABE_PROZENT);
  const bool licht_an = nvs_ben.getBool(K_LICHT_AN, true);
  zeitzone_ = nvs_ben.getString(K_TZ, g::ZEITZONE_VORGABE).c_str();
  if (zeitzone_.empty())
    zeitzone_ = g::ZEITZONE_VORGABE;

  if (antrieb_ != nullptr) {
    antrieb_->rampe(rampe, rampe);
    antrieb_->tempo(fahrtempo_);
  }
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
  nvs_ben.putUChar(K_MODUS, static_cast<uint8_t>(zeitmodus_));
  if (licht_ != nullptr) {
    nvs_ben.putFloat(K_LICHT, licht_->soll_prozent());
    nvs_ben.putBool(K_LICHT_AN, licht_->ist_an());
  }
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
         "  OTA-Passwort  " + ota_passwort_ + "\n" +
         "  Zugangspunkt  StarWatch-" + seriennummer_ + "\n" +
         "  dessen Schluessel  " + ap_passwort_ + "\n" +
         "  Zugriffstoken " + token_ + "\n";
}

void Betrieb::zeitzone(const std::string &tz) {
  zeitzone_ = tz.empty() ? std::string(g::ZEITZONE_VORGABE) : tz;
  nvs_ben.putString(K_TZ, zeitzone_.c_str());
}

bool Betrieb::zyklus_zaehlen(bool war_einschalten) {
  if (!war_einschalten) {
    // Neustart per Software, Update oder Watchdog: NICHT mitzaehlen, aber auch
    // nicht loeschen - sonst zerreisst ein OTA mitten in einer Zaehlreihe sie.
    zyklen_ = nvs.getUInt(K_ZYKLEN, 0);
    return false;
  }
  zyklen_ = nvs.getUInt(K_ZYKLEN, 0) + 1;
  nvs.putUInt(K_ZYKLEN, zyklen_);
  if (zyklen_ >= g::RESET_ZYKLEN) {
    nvs.putUInt(K_ZYKLEN, 0);
    zyklen_ = 0;
    return true;
  }
  return false;
}

void Betrieb::zyklus_tick() {
  if (zyklen_geloescht_ || zyklen_ == 0)
    return;
  if (millis() < g::RESET_FENSTER_MS)
    return;
  // Die Uhr laeuft laenger als das Fenster - das war kein Zaehlversuch.
  nvs.putUInt(K_ZYKLEN, 0);
  zyklen_ = 0;
  zyklen_geloescht_ = true;
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

void Betrieb::zeitmodus(Zeitmodus m) {
  zeitmodus_ = m;
  // Sofort stellen, nicht erst beim naechsten Takt - sonst wirkt das
  // Einschalten, als haette es nicht funktioniert.
  uhr_letzter_tick_ = 0;
}

int32_t Betrieb::soll_jetzt() const {
  if (!zeit_gueltig_ || zeitmodus_ == Zeitmodus::aus)
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
  hall_offset_ = normiere(schritte, g::REV);
  nvs.putInt(K_OFFSET, hall_offset_);
}

bool Betrieb::hier_ist_zwoelf() {
  if (antrieb_ == nullptr)
    return false;

  // Wie jeder Handeingriff: erst die Uhr aus der Fuehrung nehmen. Sonst
  // rechnet der Offset mit einer Position, die die Uhr im naechsten Takt
  // schon weitergeschoben hat. Und den Rueckfall sperren - nach 'o!' folgt
  // die Kontrollfahrt, und dabei darf die Uhr nicht dazwischenfahren.
  rueckfall_sperren();
  const bool war_an = hand_uebernimmt();

  // Die Rechnung steht in lib/zeiger und ist host-getestet - sie gehoert
  // NICHT hierher. Genau an dieser Stelle stand zuerst eine selbstgebaute
  // Formel mit falschem Vorzeichen, und weil sie hier stand, konnte kein
  // Host-Test sie fangen. Das ist der Grund fuer die Trennung.
  hall_offset(offset_einmessen(hall_offset_, antrieb_->position(), g::REV));

  // Der Zeiger zeigt jetzt zwoelf, also ist die Position null.
  antrieb_->zaehler_setzen(0);
  return war_an;
}

Homingergebnis Betrieb::referenzfahrt(Hall::Pumpe pumpe) {
  if (antrieb_ == nullptr || hall_ == nullptr) {
    letztes_homing_ = Homingergebnis();
    return letztes_homing_;
  }

  // Timer und Uhrbetrieb aus: die Referenzfahrt hat Vorrang, und wer ihr in
  // den Zaehler schreibt, macht das Ergebnis wertlos. Im Pruefstand steht
  // diese Regel als Hinweistext an der Oberflaeche.
  timer_abbrechen();
  const Zeitmodus vorher = zeitmodus_;
  zeitmodus_ = Zeitmodus::aus;

  // Klinge merken und ausschalten. Gemerkt wird der ISTZUSTAND, nicht der
  // Reglerwert - sonst ginge sie nach der Fahrt an, obwohl sie vorher aus war.
  if (licht_ != nullptr) {
    licht_war_an_ = licht_->ist_an();
    licht_->effekt_abbrechen();
    licht_->an(false);
  }

  homing_laeuft_ = true;
  letztes_homing_ = hall_->referenzfahrt(*antrieb_, hall_offset_, pumpe);
  homing_laeuft_ = false;

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
  antrieb_->fahre_zu(timer_start_pos_);

  // LICHT AUS FUER DIE LEERFAHRT. Der Weg auf zwoelf gehoert nicht zur
  // Anzeige - er ist Vorbereitung. Eine leuchtende Klinge, die dabei einmal
  // quer ueber das Blatt wischt, sagt etwas, das nicht stimmt. Der Zustand
  // wird gemerkt und beim Start des Laufs wiederhergestellt.
  if (licht_ != nullptr) {
    licht_vor_leerfahrt_ = licht_->ist_an();
    licht_gemerkt_ = true;
    licht_->an(false);
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
  timer_ende_merken(0);
  if (antrieb_ != nullptr) {
    antrieb_->stopp_mit_rampe();
    antrieb_->tempo(fahrtempo_);
  }
  // Auch ein Abbruch mitten in der Leerfahrt muss das Licht zurueckgeben.
  if (licht_gemerkt_ && licht_ != nullptr) {
    licht_->an(licht_vor_leerfahrt_);
    licht_gemerkt_ = false;
  }
  handlung_gesehen();
}

const char *Betrieb::timer_zustand_text() const {
  switch (timer_phase_) {
    case Timerphase::aus:        return "aus";
    case Timerphase::auf_zwoelf: return "faehrt auf zwoelf";
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
        timer_ab_ms_ = millis();
        timer_phase_ = Timerphase::laeuft;
        timer_phase_ab_ms_ = millis();
        // Die Leerfahrt ist vorbei, der Lauf ist die Anzeige: Licht zurueck.
        if (licht_gemerkt_ && licht_ != nullptr) {
          licht_->an(licht_vor_leerfahrt_);
          licht_gemerkt_ = false;
        }
      } else if (millis() - timer_phase_ab_ms_ > TIMER_ANFAHRT_TIMEOUT_MS) {
        timer_abbrechen();
      }
      return;

    case Timerphase::laeuft: {
      const uint32_t verstrichen = millis() - timer_ab_ms_;
      if (verstrichen >= timer_dauer_ms_) {
        // Die letzte Strecke in einem Zug, damit der Zeiger sicher auf zwoelf
        // ankommt und nicht ein paar Schritte davor stehenbleibt.
        antrieb_->fahre_zu(timer_start_pos_ + g::REV);
        timer_phase_ = Timerphase::endfahrt;
        timer_phase_ab_ms_ = millis();
        return;
      }
      if (millis() - timer_phase_ab_ms_ < timer_takt_ms(timer_dauer_ms_))
        return;
      timer_phase_ab_ms_ = millis();

      const int32_t ziel =
          timer_ziel(timer_start_pos_, verstrichen, timer_dauer_ms_, g::REV);
      // Nur vorwaerts: nie ein Ziel setzen, das hinter dem aktuellen liegt.
      if (ziel > antrieb_->ziel())
        antrieb_->fahre_zu(ziel);
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
        // Zeiger zurueck - der Zeiger soll nicht auf zwoelf stehenbleiben und
        // eine Zeit behaupten, die nicht stimmt.
        handlung_gesehen();
        rueckfall_freigeben();
      }
      return;
  }
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
  antrieb_->fahre_zu(ziel + kreise * g::REV + delta);
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
  antrieb_->fahre_weiter(schritte);
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
  antrieb_->tempo(g::TEMPO_UHR);

  // Steht der Zeiger ein paar Schritte VOR der Zeit, waere delta fast eine
  // ganze Umdrehung. Dann ist Stehenbleiben richtig, bis die Zeit aufgeholt
  // hat - eine Umdrehung nachzufahren waere ein Richtungswechsel im Ergebnis.
  if (warten_statt_umrunden(delta, g::REV, VORLAUF_TOLERANZ))
    return;

  antrieb_->fahre_zu(ist + delta);
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

uint32_t Betrieb::rueckfall_rest_ms() const {
  if (rueckfall_gesperrt_ || rueckfall_modus_ == Zeitmodus::aus ||
      zeitmodus_ != Zeitmodus::aus || !zeit_gueltig_)
    return 0;
  const uint32_t seit = millis() - letzte_handlung_ms_;
  return seit >= g::RUECKFALL_MS ? 0 : (g::RUECKFALL_MS - seit);
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
  if (timer_phase_ != Timerphase::aus)
    return;                       // ein Timer haelt den Zeiger
  if (antrieb_ == nullptr || antrieb_->laeuft())
    return;                       // erst ankommen, dann uebernehmen
  if (millis() - letzte_handlung_ms_ < g::RUECKFALL_MS)
    return;                       // die Frist laeuft noch

  zeitmodus_ = rueckfall_modus_;
  rueckfall_geschehen_ = true;
}

void Betrieb::tick() {
  if (licht_ != nullptr)
    licht_->tick();
  if (hall_ != nullptr && antrieb_ != nullptr)
    hall_->tick(antrieb_->position());

  timer_tick();
  rueckfall_pruefen();

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
