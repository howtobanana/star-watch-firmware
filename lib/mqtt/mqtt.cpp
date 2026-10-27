#include "mqtt.h"

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include <geraet.h>

namespace starwatch {
namespace g = geraet;
namespace {

WiFiClient netz;
PubSubClient klient(netz);
Mqtt *aktiv = nullptr;

// DIE FALLE VON PubSubClient: die Vorgabe ist 256 Byte fuer das GANZE Paket.
// Ein Discovery-Rumpf mit Geraeteblock liegt darueber - und die Bibliothek
// verwirft ihn dann STILLSCHWEIGEND. In Home Assistant erscheint einfach
// nichts, und im Log steht kein Wort. Deshalb ausdruecklich hochgesetzt, und
// jeder publish-Rueckgabewert wird geprueft.
constexpr uint16_t PUFFER = 1024;

/// Wie oft ein Verbindungsversuch hoechstens ansteht. Ohne Bremse klopft die
/// Uhr bei abgeschaltetem Broker im Sekundentakt an und fuellt das Log.
constexpr uint32_t VERSUCH_ABSTAND_MS = 15000;

/// JSON-Text entschaerfen. Broker-Benutzernamen und Geraetenamen sind frei
/// waehlbar; ein Anfuehrungszeichen darin zerlegte sonst die Discovery.
std::string json_text(const std::string &roh) {
  std::string s;
  s.reserve(roh.size() + 8);
  for (const char c : roh) {
    if (c == '"' || c == '\\')
      s.push_back('\\');
    s.push_back(c);
  }
  return s;
}

}  // namespace

void Mqtt::beginn(Betrieb *betrieb, Licht *licht, Antrieb *antrieb, Lux *lux,
                  Kommandofn kommando) {
  betrieb_ = betrieb;
  licht_ = licht;
  lux_ = lux;
  antrieb_ = antrieb;
  kommando_ = kommando;
  aktiv = this;

  // DIE KENNUNG HAENGT AN DER SERIENNUMMER, nicht am Geraetenamen und nicht
  // an der MAC-Formatierung. Sie darf sich nie aendern: eine neue `unique_id`
  // gibt dem Kunden jede Entitaet ein zweites Mal, und seine
  // Automatisierungen zeigen ins Leere.
  const std::string serie = betrieb_->seriennummer();
  basis_ = "starwatch/" + serie;
  kennung_ = "starwatch_" + serie;

  klient.setBufferSize(PUFFER);
  klient.setKeepAlive(30);
  klient.setCallback([](char *thema, uint8_t *nutzlast, unsigned int len) {
    if (aktiv == nullptr)
      return;
    std::string inhalt(reinterpret_cast<const char *>(nutzlast), len);
    aktiv->nachricht(thema, inhalt.c_str());
  });

  gestartet_ = true;
  lage_ = betrieb_->mqtt_bereit() ? "wartet auf Verbindung"
                                  : "aus (kein Broker eingetragen)";
}

bool Mqtt::verbunden() const { return klient.connected(); }

void Mqtt::verbinden() {
  klient.setServer(betrieb_->mqtt_host().c_str(), betrieb_->mqtt_port());

  // LETZTER WILLE. Bricht die Uhr weg - Stromausfall, Absturz, Update -,
  // schreibt der Broker "offline" auf dieses Thema, und in Home Assistant
  // stehen alle vier Entitaeten auf "nicht verfuegbar" statt auf dem zuletzt
  // gesehenen Wert. Ein Schalter, der "an" zeigt, obwohl das Geraet weg ist,
  // ist schlimmer als gar keine Anzeige.
  const std::string status = basis_ + "/status";
  const bool mit_anmeldung = !betrieb_->mqtt_benutzer().empty();
  const bool ok = klient.connect(
      betrieb_->geraetename().c_str(),
      mit_anmeldung ? betrieb_->mqtt_benutzer().c_str() : nullptr,
      mit_anmeldung ? betrieb_->mqtt_passwort().c_str() : nullptr,
      status.c_str(), 0, true, "offline", true);

  if (ok) {
    angemeldet();
    return;
  }
  // Der Zahlencode ist die einzige Auskunft, die PubSubClient gibt: -2 kommt
  // das Netz nicht zustande, 4 stimmt die Anmeldung nicht, 5 ist sie nicht
  // erlaubt. Ohne ihn steht der Bediener vor "geht nicht".
  lage_ = "kein Anschluss (Code " + std::to_string(klient.state()) + ")";
  konsole.printf("[mqtt] Verbindung zu %s:%u fehlgeschlagen, Code %d\n",
                 betrieb_->mqtt_host().c_str(), betrieb_->mqtt_port(),
                 klient.state());
}

void Mqtt::angemeldet() {
  lage_ = "verbunden";
  konsole.printf("[mqtt] Verbunden mit %s:%u\n", betrieb_->mqtt_host().c_str(),
                 betrieb_->mqtt_port());

  const std::string status = basis_ + "/status";
  klient.publish(status.c_str(), "online", true);

  for (const char *t : {"/licht/set", "/betrieb/set", "/melden/set", "/automatik/set",
                        "/nacht/set", "/puls_periode/set", "/puls_min/set"}) {
    const std::string thema = basis_ + t;
    klient.subscribe(thema.c_str(), 1);
  }

  discovery_senden();
  zustand_senden(true);
}

void Mqtt::discovery_senden() {
  // EIN Geraeteblock fuer alle vier Entitaeten - so stehen sie in Home
  // Assistant auf EINER Geraetekarte und nicht als vier lose Dinger.
  const std::string dev =
      "\"dev\":{\"ids\":[\"" + kennung_ + "\"],\"name\":\"" +
      json_text(betrieb_->anzeigename()) + "\",\"mdl\":\"Star Watch\"," +
      "\"mf\":\"Fabian Brandl\",\"sw\":\"" + g::FIRMWARE_VERSION + "\"}";
  const std::string avty = "\"avty_t\":\"" + basis_ + "/status\"";
  s_name_ = betrieb_->anzeigename();

  struct Eintrag {
    const char *bauart;   ///< HA-Komponente
    const char *objekt;   ///< Objekt-ID, Teil der unique_id
    std::string rumpf;
  };

  const Eintrag eintraege[] = {
      // Die Klinge. JSON-Schema mit brightness_scale 100, damit die Prozente
      // aus `Licht` unveraendert durchgehen - keine 0-255-Umrechnung, keine
      // Rundungsfehler an den Enden.
      //
      // Die Klinge meldet sich als RGB-Licht (`sup_clrm`). Home
      // Assistant schickt die Farbe dann als {"color":{"r":..,"g":..,"b":..}}
      // und die Helligkeit getrennt - genau die Trennung, die lib/licht
      // selbst macht.
      {"light", "licht",
       std::string("\"schema\":\"json\",\"name\":\"Klinge\",\"brightness\":true,"
       "\"brightness_scale\":100,\"sup_clrm\":[\"rgb\"],"
       // 0.20.0: Uebergaenge, Blitze und der Effekt "Puls" werden wirklich
       // ausgefuehrt. transition und flash kuendigte Home Assistant schon
       // vorher an (Vorgabe true im JSON-Schema) - die Uhr las sie nur nicht.
       "\"transition\":true,\"flash\":true,"
       "\"effect\":true,\"effect_list\":[\"Puls\"],") +
       "\"stat_t\":\"" + basis_ + "/licht/state\","
       "\"cmd_t\":\"" + basis_ + "/licht/set\""},

      // Betrieb. `ent_cat: config` ist hier kein Schoenheitsfehler, sondern
      // der Schutz: siehe die Begruendung im Header.
      {"switch", "betrieb",
       "\"name\":\"Betrieb\",\"ent_cat\":\"config\","
       "\"stat_t\":\"" + basis_ + "/betrieb/state\","
       "\"cmd_t\":\"" + basis_ + "/betrieb/set\","
       "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""},

      // Melden. Als Knopf fuer den bequemen Fall - dasselbe Thema nimmt aber
      // auch eine Nutzlast entgegen, damit eine Automatisierung spaeter
      // {"licht":"blitz","dauer":30} schicken kann, ohne dass sich der
      // Discovery-Vertrag noch einmal aendert.
      {"button", "melden",
       "\"name\":\"Melden\",\"cmd_t\":\"" + basis_ + "/melden/set\""},

      // Meldung quittieren. Derselbe Weg wie {"aus":true}, nur als Knopf -
      // damit auf einer Karte in Home Assistant nicht erst ein Skript noetig
      // ist, um das Gelb wieder wegzubekommen.
      {"button", "quittieren",
       "\"name\":\"Meldung quittieren\",\"cmd_t\":\"" + basis_ +
       "/melden/set\",\"pl_prs\":\"OFF\""},

      // Steht eine Meldung? Die Rueckrichtung - eine Automatisierung kann so
      // sehen, ob jemand schon quittiert hat.
      {"binary_sensor", "meldung",
       "\"name\":\"Meldung\","
       "\"stat_t\":\"" + basis_ + "/melden/aktiv\","
       "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""},

      // Timer. Die Rueckrichtung: damit wird die Uhr zum Ausloeser fuer
      // fremde Automatisierungen und nicht nur zum Empfaenger.
      {"binary_sensor", "timer",
       "\"name\":\"Timer\","
       "\"stat_t\":\"" + basis_ + "/timer/state\","
       "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""},

      // Lichtautomatik und Nachtabschaltung (27.09.2026). `config`, weil es
      // Einstellungen sind und keine Bedienung - sie stehen in Home Assistant
      // unter "Konfiguration", nicht zwischen Klinge und Timer.
      {"switch", "automatik",
       "\"name\":\"Lichtautomatik\",\"ent_cat\":\"config\",\"ic\":\"mdi:brightness-auto\","
       "\"stat_t\":\"" + basis_ + "/automatik/state\","
       "\"cmd_t\":\"" + basis_ + "/automatik/set\","
       "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""},
      {"switch", "nacht",
       "\"name\":\"Nachtabschaltung\",\"ent_cat\":\"config\",\"ic\":\"mdi:weather-night\","
       "\"stat_t\":\"" + basis_ + "/nacht/state\","
       "\"cmd_t\":\"" + basis_ + "/nacht/set\","
       "\"pl_on\":\"ON\",\"pl_off\":\"OFF\""},

      // Puls (0.20.0): Geschwindigkeit und unterste Stufe. `config` wie die
      // Automatik - Einstellungen, keine Bedienung.
      {"number", "puls_periode",
       "\"name\":\"Puls-Periode\",\"ent_cat\":\"config\",\"ic\":\"mdi:sine-wave\","
       "\"min\":400,\"max\":4000,\"step\":100,\"unit_of_meas\":\"ms\",\"mode\":\"slider\","
       "\"stat_t\":\"" + basis_ + "/puls_periode/state\","
       "\"cmd_t\":\"" + basis_ + "/puls_periode/set\""},
      {"number", "puls_min",
       "\"name\":\"Puls-Minimum\",\"ent_cat\":\"config\",\"ic\":\"mdi:brightness-5\","
       "\"min\":1,\"max\":50,\"step\":1,\"unit_of_meas\":\"%\",\"mode\":\"slider\","
       "\"stat_t\":\"" + basis_ + "/puls_min/state\","
       "\"cmd_t\":\"" + basis_ + "/puls_min/set\""},

      // Umgebungslicht. `dev_cla: illuminance` sagt Home Assistant, was die
      // Zahl bedeutet - daran haengen Einheit, Symbol und die richtige Art
      // von Diagramm. Ohne die Angabe waere es eine nackte Zahl.
      {"sensor", "lux",
       "\"name\":\"Umgebungslicht\",\"dev_cla\":\"illuminance\","
       "\"unit_of_meas\":\"lx\",\"stat_cla\":\"measurement\","
       "\"stat_t\":\"" + basis_ + "/lux/state\""},
  };

  for (const Eintrag &e : eintraege) {
    // Ohne Sensor an J8 gibt es keine Entitaet. Eine angelegte, die nie
    // einen Wert bekommt, steht in Home Assistant als "Unbekannt" und sieht
    // aus wie ein Fehler der Uhr.
    const std::string obj(e.objekt);
    if ((obj == "lux" || obj == "automatik" || obj == "nacht") &&
        (lux_ == nullptr || !lux_->bereit()))
      continue;
    const std::string thema = std::string("homeassistant/") + e.bauart + "/" +
                              kennung_ + "/" + e.objekt + "/config";
    const std::string inhalt = "{" + e.rumpf + ",\"uniq_id\":\"" + kennung_ +
                               "_" + e.objekt + "\"," + avty + "," + dev + "}";
    // RETAINED. Startet Home Assistant neu, sind die Entitaeten sofort wieder
    // da - ohne dass die Uhr etwas davon mitbekommen muss.
    if (!klient.publish(thema.c_str(), inhalt.c_str(), true))
      konsole.printf("[mqtt] Discovery '%s' ABGEWIESEN - %u Byte, Puffer %u.\n",
                     e.objekt, (unsigned)(thema.size() + inhalt.size()),
                     (unsigned)PUFFER);
  }
  konsole.printf("[mqtt] Discovery gesendet: light (transition, flash, Puls), switch, 2 x button,\n"
                 "       2 x binary_sensor, 2 x number (Puls)%s.\n",
                 (lux_ != nullptr && lux_->bereit())
                     ? ", sensor (Lux), 2 x switch (Automatik, Nacht)"
                     : "");
}

void Mqtt::zustand_senden(bool erzwingen) {
  // WAEHREND DES PULSES die Sollwerte des Pulses - nicht jede Stufe. Die
  // wechselt alle 20 ms und waere eine MQTT-Flut; gesendet wird nur, wenn
  // sich ein Sollwert aendert, und beim Start und Ende.
  const bool puls = licht_->puls_aktiv();
  const bool licht_an = puls || licht_->ist_an();
  const int hell = static_cast<int>((puls ? licht_->puls_max() : licht_->soll_prozent()) + 0.5f);
  const uint32_t puls_per = licht_->puls_periode();
  const int puls_min = static_cast<int>(licht_->puls_min() * 100.0f + 0.5f);
  const bool betrieb_an = licht_->ist_an() && antrieb_->endstufe_an();
  const bool timer = betrieb_->timer_laeuft();

  // Der Lux-Wert bekommt eine eigene Schwelle: 5 % Unterschied ODER 0,2 lx.
  // Die zweite Bedingung ist die wichtigere - unten an der Skala, wo die
  // Nachtabschaltung sitzt, sind 5 % von 0,4 lx nur 0,02 lx und damit
  // Rauschen.
  const bool lux_da = lux_ != nullptr && lux_->bereit() && lux_->gemessen();
  const float lux_wert = lux_da ? lux_->lux() : -1.0f;
  const bool lux_neu =
      lux_da && (s_lux_ < 0.0f || fabsf(lux_wert - s_lux_) > 0.2f ||
                 fabsf(lux_wert - s_lux_) > 0.05f * s_lux_);

  const uint32_t farbe = puls ? licht_->puls_farbe() : licht_->farbe();
  const auto meld = licht_->signal_zustand(Licht::Ebene::meldung);
  const bool meld_neu = meld.aktiv != s_meld_ ||
                        (meld.aktiv && (meld.farbe != s_meld_farbe_ ||
                                        static_cast<int>(meld.muster) != s_meld_muster_));

  const bool la_an = betrieb_->lichtautomatik().an;
  const bool la_nacht = betrieb_->lichtautomatik().nacht;

  if (!erzwingen && zustand_gueltig_ && licht_an == s_licht_an_ &&
      hell == s_hell_ && betrieb_an == s_betrieb_an_ && timer == s_timer_ &&
      farbe == s_farbe_ && !lux_neu && !meld_neu && la_an == s_la_an_ &&
      la_nacht == s_la_nacht_ && puls == s_puls_ && puls_per == s_puls_per_ &&
      puls_min == s_puls_min_)
    return;

  std::string licht_json = std::string("{\"state\":\"") +
                           (licht_an ? "ON" : "OFF") +
                           "\",\"brightness\":" + std::to_string(hell);
  licht_json += ",\"color_mode\":\"rgb\",\"color\":{\"r\":" +
                  std::to_string((farbe >> 16) & 0xFF) +
                  ",\"g\":" + std::to_string((farbe >> 8) & 0xFF) +
                  ",\"b\":" + std::to_string(farbe & 0xFF) + "}";
  licht_json += std::string(",\"effect\":\"") + (puls ? "Puls" : "none") + "\"}";
  const std::string t_licht = basis_ + "/licht/state";
  const std::string t_betrieb = basis_ + "/betrieb/state";
  const std::string t_timer = basis_ + "/timer/state";
  klient.publish(t_licht.c_str(), licht_json.c_str(), true);
  klient.publish(t_betrieb.c_str(), betrieb_an ? "ON" : "OFF", true);
  klient.publish(t_timer.c_str(), timer ? "ON" : "OFF", true);
  if (erzwingen || !zustand_gueltig_ || puls_per != s_puls_per_ || puls_min != s_puls_min_) {
    const std::string t_pp = basis_ + "/puls_periode/state";
    const std::string t_pm = basis_ + "/puls_min/state";
    klient.publish(t_pp.c_str(), std::to_string(puls_per).c_str(), true);
    klient.publish(t_pm.c_str(), std::to_string(puls_min).c_str(), true);
  }
  if (lux_ != nullptr && lux_->bereit()) {
    const std::string t_la = basis_ + "/automatik/state";
    const std::string t_ln = basis_ + "/nacht/state";
    klient.publish(t_la.c_str(), la_an ? "ON" : "OFF", true);
    klient.publish(t_ln.c_str(), la_nacht ? "ON" : "OFF", true);
  }

  if (meld_neu || erzwingen) {
    char m[120];
    if (meld.aktiv)
      // KEINE Restzeit: gesendet wird nur bei einer Aenderung, eine Zahl hier
      // stuende also eingefroren da und luege. Ob die Meldung von selbst
      // endet, bleibt wahr, solange sie steht.
      snprintf(m, sizeof(m),
               "{\"aktiv\":true,\"farbe\":\"#%06lx\",\"muster\":\"%s\",\"bis_quittiert\":%s}",
               (unsigned long)meld.farbe, muster_text(meld.muster),
               meld.rest_ms == 0 ? "true" : "false");
    else
      snprintf(m, sizeof(m), "{\"aktiv\":false}");
    const std::string t_ms = basis_ + "/melden/state";
    const std::string t_ma = basis_ + "/melden/aktiv";
    klient.publish(t_ms.c_str(), m, true);
    klient.publish(t_ma.c_str(), meld.aktiv ? "ON" : "OFF", true);
    s_meld_ = meld.aktiv;
    s_meld_farbe_ = meld.farbe;
    s_meld_muster_ = static_cast<int>(meld.muster);
  }

  if (lux_da && (lux_neu || erzwingen)) {
    const std::string t_lux = basis_ + "/lux/state";
    char zahl[16];
    snprintf(zahl, sizeof(zahl), "%.1f", static_cast<double>(lux_wert));
    klient.publish(t_lux.c_str(), zahl, true);
    s_lux_ = lux_wert;
  }

  s_licht_an_ = licht_an;
  s_hell_ = hell;
  s_farbe_ = farbe;
  s_betrieb_an_ = betrieb_an;
  s_timer_ = timer;
  s_la_an_ = la_an;
  s_la_nacht_ = la_nacht;
  s_puls_ = puls;
  s_puls_per_ = puls_per;
  s_puls_min_ = puls_min;
  zustand_gueltig_ = true;
}

void Mqtt::nachricht(const char *thema, const char *inhalt) {
  const std::string t(thema);

  // ALLES GEHT DURCH DIESELBE KOMMANDOFUNKTION wie Konsole und
  // Weboberflaeche. Ein zweiter Weg in die Mechanik waere ein zweiter Ort,
  // an dem Sperren, Rueckfall und Homing beruecksichtigt werden muessten -
  // und der eine wuerde frueher oder spaeter vergessen.
  char zeile[64];

  if (t == basis_ + "/licht/set") {
    // 0.20.0: der Befehl wird VOLLSTAENDIG gelesen (lichtbefehl_lesen,
    // host-getestet) - auch "transition", "effect" und "flash". Bis 0.19.8
    // wurden daraus 'lf' (mit Zuendung, die warmweiss beginnt) und 'l <p>'
    // (Sprung), und transition fiel unter den Tisch.
    Lichtbefehl b;
    lichtbefehl_lesen(inhalt, &b);
    char h[12] = "", f[12] = "";
    if (b.hell > 0)
      snprintf(h, sizeof(h), " h %d", b.hell);
    if (b.farbe_da)
      snprintf(f, sizeof(f), " f %06lX", static_cast<unsigned long>(b.farbe));
    const bool puls = licht_->puls_aktiv();

    if (b.state == 0 || b.hell == 0) {
      // AUS. Mit transition weich, sonst wie bisher. Das zieht "Betrieb" mit
      // (gewollt) - deshalb schaltet der Puls selbst nie aus.
      if (b.uebergang_ms > 0)
        snprintf(zeile, sizeof(zeile), "lg %ld o", static_cast<long>(b.uebergang_ms));
      else
        snprintf(zeile, sizeof(zeile), "lo");
    } else if (b.effekt == 1) {
      snprintf(zeile, sizeof(zeile), "lp%s%s", h, f);
    } else if (b.blitze > 0) {
      if (b.farbe_da || b.hell > 0) {
        snprintf(zeile, sizeof(zeile), "lg 0 a%s%s", h, f);
        kommando_(zeile);
      }
      snprintf(zeile, sizeof(zeile), "lb %d", b.blitze);
    } else if (puls && !b.farbe_da && b.hell < 0) {
      // Puls beenden ("effect":"none" oder ein Befehl ohne Werte): weich auf
      // den Zustand DAVOR, an/aus eingeschlossen.
      snprintf(zeile, sizeof(zeile), "lg %ld",
               static_cast<long>(b.uebergang_ms >= 0 ? b.uebergang_ms : 1000));
    } else if (!puls && !licht_->ist_an() && b.uebergang_ms < 0) {
      // Einschalten aus AUS ohne transition: die Zuendung, wie bisher beim
      // Start - Werte vorher still gesetzt.
      snprintf(zeile, sizeof(zeile), "lg 0 z%s%s", h, f);
    } else {
      // Alles andere gleitet vom GEZEIGTEN Wert: mit transition so lange wie
      // verlangt, sonst 0,5 s (aus einem Puls heraus 1 s).
      const long ms = b.uebergang_ms >= 0 ? b.uebergang_ms : (puls ? 1000 : 500);
      snprintf(zeile, sizeof(zeile), "lg %ld a%s%s", ms, h, f);
    }
    kommando_(zeile);
    // Sofort melden, was jetzt gilt - HA soll nicht auf den naechsten Takt
    // warten, um den Effekt zu sehen.
    zustand_senden(false);
  } else if (t == basis_ + "/betrieb/set") {
    snprintf(zeile, sizeof(zeile), "q %d",
             std::string(inhalt) == "ON" ? 1 : 0);
    kommando_(zeile);
  } else if (t == basis_ + "/puls_periode/set" || t == basis_ + "/puls_min/set") {
    // HA schickt die Zahl als Text, gern mit ".0".
    const long wert = static_cast<long>(atof(inhalt) + 0.5);
    snprintf(zeile, sizeof(zeile), "lp %s %ld",
             t == basis_ + "/puls_periode/set" ? "periode" : "min", wert);
    kommando_(zeile);
    zustand_senden(false);
  } else if (t == basis_ + "/automatik/set") {
    snprintf(zeile, sizeof(zeile), "lu %d", std::string(inhalt) == "ON" ? 1 : 0);
    kommando_(zeile);
  } else if (t == basis_ + "/nacht/set") {
    snprintf(zeile, sizeof(zeile), "lu n %d", std::string(inhalt) == "ON" ? 1 : 0);
    kommando_(zeile);
  } else if (t == basis_ + "/melden/set") {
    // Drei Faelle, siehe Kopf von mqtt.h. Die Meldung geht als 'lm ...'
    // durch DIESELBE Kommandofunktion wie Konsole und Oberflaeche - die
    // Pruefung der Werte steht damit an einer Stelle (meldung_lesen, host-
    // getestet), nicht hier.
    const std::string j(inhalt);
    if (j == "OFF" || j.find("\"aus\":true") != std::string::npos ||
        j.find("\"aus\": true") != std::string::npos) {
      snprintf(zeile, sizeof(zeile), "lm-");
      kommando_(zeile);
    } else if (j.find("\"farbe\"") != std::string::npos) {
      // Den Wert hinter einem Schluessel: Text in Anfuehrungszeichen, Zahl
      // oder Objekt. Kein vollstaendiger JSON-Leser - Home Assistant schickt
      // flaches JSON, und was hier nicht passt, weist meldung_lesen ab.
      auto wert = [&](const char *schluessel, char *aus, size_t platz) -> bool {
        const size_t k = j.find(std::string("\"") + schluessel + "\"");
        if (k == std::string::npos) return false;
        size_t p = j.find(':', k);
        if (p == std::string::npos) return false;
        p++;
        while (p < j.size() && j[p] == ' ') p++;
        if (p >= j.size()) return false;
        size_t n = 0;
        if (j[p] == '"') {
          p++;
          while (p < j.size() && j[p] != '"' && n + 1 < platz) aus[n++] = j[p++];
        } else {
          // Nur der ganzzahlige Teil: Home Assistant liefert aus Vorlagen
          // gern "600.0", und Sekunden bzw. Prozent brauchen keine Stellen.
          while (p < j.size() && j[p] >= '0' && j[p] <= '9' && n + 1 < platz)
            aus[n++] = j[p++];
        }
        aus[n] = '\0';
        return n > 0;
      };
      char farbe[24] = "", muster[16] = "leuchten", dauer[12] = "0", hell[8] = "";
      const size_t pf = j.find("\"farbe\"");
      const size_t pk = j.find('{', pf);
      const size_t pq = j.find('"', j.find(':', pf) + 1);
      if (pk != std::string::npos && (pq == std::string::npos || pk < pq)) {
        // {"r":..,"g":..,"b":..}
        auto teil = [&](const char *s) -> int {
          const size_t q = j.find(s, pk);
          const size_t d = q == std::string::npos ? q : j.find(':', q);
          // Fehlt Schluessel oder Doppelpunkt, ist der Teil ungueltig - nicht
          // "ab Textanfang gelesen", was npos + 1 = 0 ergaebe.
          return d == std::string::npos ? -1 : atoi(j.c_str() + d + 1);
        };
        const int r = teil("\"r\""), gr = teil("\"g\""), b = teil("\"b\"");
        if (r >= 0 && r <= 255 && gr >= 0 && gr <= 255 && b >= 0 && b <= 255)
          snprintf(farbe, sizeof(farbe), "%02x%02x%02x", r, gr, b);
      } else {
        wert("farbe", farbe, sizeof(farbe));
      }
      wert("muster", muster, sizeof(muster));
      wert("dauer", dauer, sizeof(dauer));
      wert("hell", hell, sizeof(hell));
      // Leerzeichen im Farbtext wuerden die Zeile anders zerlegen - weg damit.
      for (char *c = farbe; *c; c++)
        if (*c == ' ') *c = '_';
      snprintf(zeile, sizeof(zeile), "lm %s %s %s %s", farbe, muster, dauer, hell);
      kommando_(zeile);
    } else {
      // Knopf (PRESS) oder unbekannte Nutzlast: drei kurze Blitze wie bisher.
      licht_->alarm();
    }
  } else {
    return;
  }
  zustand_senden(true);
}

void Mqtt::tick() {
  if (!gestartet_)
    return;

  if (!betrieb_->mqtt_bereit()) {
    if (klient.connected())
      klient.disconnect();
    zustand_gueltig_ = false;
    lage_ = "aus (kein Broker eingetragen)";
    return;
  }

  if (klient.connected()) {
    klient.loop();
    // UMBENANNT? Dann die Discovery neu - Home Assistant uebernimmt den
    // Geraetenamen daraus. Die unique_id haengt an der Seriennummer, also
    // bleiben Entitaeten, Verlauf und Automatisierungen, wo sie sind.
    if (betrieb_->anzeigename() != s_name_)
      discovery_senden();
    zustand_senden(false);
    return;
  }
  // Erst hier gemerkt, dass die Verbindung weg ist: PubSubClient meldet das
  // nicht von selbst. Der naechste Aufbau muss deshalb alles neu schicken.
  if (zustand_gueltig_) {
    zustand_gueltig_ = false;
    lage_ = "getrennt";
    konsole.println("[mqtt] Verbindung weg.");
  }

  // NICHT WAEHREND EINER FAHRT ANKLOPFEN. `connect()` haelt kurz an, solange
  // die Verbindung aufgebaut wird; das waere im Zeigerlauf zu sehen. Ein paar
  // Sekunden spaeter verbunden zu sein, sieht niemand.
  if (antrieb_->laeuft())
    return;
  if (letzter_versuch_ms_ != 0 &&
      millis() - letzter_versuch_ms_ < VERSUCH_ABSTAND_MS)
    return;
  letzter_versuch_ms_ = millis();
  lage_ = "verbinde ...";
  verbinden();
}

}  // namespace starwatch
