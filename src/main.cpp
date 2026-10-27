// ---------------------------------------------------------------------------
// Star Watch - Betriebsfirmware, Zusammenbau. AP 2.
//
// Bedient wird auf ZWEI Wegen, die dieselben Kommandos ausfuehren:
//
//   1. serielle Konsole, 115200 Baud - `?` und Enter zeigt die Liste
//   2. Test-Weboberflaeche im Browser, siehe lib/testweb
//
// Dass beide dieselbe Funktion aufrufen, ist Absicht und keine Bequemlichkeit:
// zwei Bedienwege mit eigenem Code laufen auseinander, und dann debuggt man
// die Oberflaeche statt die Uhr. AP 3.4 stellt fuer die echte API dieselbe
// Regel auf - alle Schnittstellen bedienen denselben Zustand.
//
// DIE WEBOBERFLAECHE IST KEINE API. Ihre Endpunkte heissen `/test/...`, damit
// daraus kein Vertrag wird; die echte API entsteht in AP 3.4 nach der
// OpenAPI-Datei. Begruendung im Kopf von lib/testweb/testweb.h.
//
//   ~/.platformio/penv/bin/pio run -e starwatch -t upload -t monitor
//
// ===========================================================================
// WAS HIER NICHT STEHT
// ===========================================================================
// Keine Rechnung. Positionen, Zeiten, Schwerpunkte, Helligkeitskennlinie und
// die Kollisionsregel stehen in lib/zeiger und sind host-getestet
// (`pio test -e native`). Diese Datei liest Kommandos, ruft die
// Bibliotheken und schreibt ins Log.
//
// AUSGABEN OHNE UMLAUTE - die Konsole ist nicht zuverlaessig auf UTF-8.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <esp_system.h>
#include <Preferences.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <aktualisierung.h>
#include <antrieb.h>
#include <betrieb.h>
#include <geraet.h>
#include <hall.h>
#include <i2cbus.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcpip.h>
#include <lux.h>
#include <WiFi.h>
#include <einrichtung.h>
#include <licht.h>
#include <mqtt.h>
#include <testweb.h>
#include <zeiger.h>

namespace g = starwatch::geraet;

starwatch::Antrieb antrieb;
starwatch::Hall hall;
starwatch::Lux lux;

// ===========================================================================
// LICHTAUTOMATIK (VEML7700 an J8)
// ===========================================================================
//
// Zwei Wirkungen aus EINEM Messwert, und beide sind absichtlich getrennt:
//
//   1. DIMMEN      - ein Faktor auf den eingestellten Wert, stufenlos.
//                    licht_anteil() in lib/zeiger, host-getestet.
//   2. NACHTS AUS  - ein Schalter mit Hysterese und Frist. Nachtschalter in
//                    lib/zeiger, host-getestet.
//
// Warum getrennt: das Dimmen ist eine Kennlinie ohne Gedaechtnis, das
// Abschalten ein Zustand mit Gedaechtnis. In eine Funktion gelegt haette
// man einen Zustandsautomaten, der auch noch rechnet - und genau dort sitzen
// die Fehler, die am Geraet als "flackert manchmal" auftreten.
//
// Die Automatik laesst sich abschalten. Ist sie aus oder fehlt der Sensor,
// steht der Faktor auf 1 und die Klinge folgt allein dem Regler.
/// Wollte der Benutzer die Klinge an? Aus dem gemerkten Zustand beim Start.
///
/// WARUM DAS EXTRA GEMERKT WIRD: `betrieb.beginn()` stellt den gespeicherten
/// Ein-Zustand wieder her (`betrieb.cpp:188`) - im laufenden Betrieb richtig,
/// beim Start aber, BEVOR feststeht, ob es eine Referenz gibt. Am 24.09.2026
/// am Geraet gesehen: `licht_an: true` bei `referenz: FEHLT`. Die Klinge
/// leuchtete also doch ueber einer unbekannten Zeigerlage - genau das, was
/// die Aenderung vom Vorabend verhindern sollte.
bool klinge_wunsch_an = false;

/// Brownouts in Folge - Begruendung in setup(). Ab zwei startet kein WLAN.
uint32_t brownout_folge = 0;

starwatch::Licht licht;
starwatch::Betrieb betrieb;
starwatch::Aktualisierung aktualisierung;
starwatch::Testweb testweb;
starwatch::BleEinrichtung ble;
starwatch::Mqtt mqtt;

// ===========================================================================
// Konsole. NICHT blockierend - waehrend eine Eingabe aussteht, laufen der
// Uhrbetrieb, die Lichteffekte und der Timer weiter.
// ===========================================================================
namespace {

char eingabe[g::KOMMANDO_MAX];
bool eingabe_zu_lang = false;
size_t eingabe_len = 0;
bool abbruch_gewuenscht = false;

/// Selbstheilung der Referenzfahrt - Begruendung bei den Kennzahlen in
/// geraet.h. Hier oben, weil `zustand()` sie ebenfalls liest.
uint32_t homing_fehlschlaege = 0;
uint32_t homing_letzter_versuch_ms = 0;
int homing_letzte_stunde = -1;

// ===========================================================================
// DAS ROLLBACK SCHARF HALTEN - gefunden und behoben am 24.08.2026
// ===========================================================================
// Der Bootloader setzt ein frisch eingespieltes Image auf PENDING_VERIFY
// (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y). Bestaetigt es sich nicht selbst,
// faehrt der naechste Start die vorige Fassung.
//
// DER ARDUINO-KERN ENTSCHAERFT DAS ABER VON SELBST. In `initArduino()` -
// NOCH VOR setup() - steht in esp32-hal-misc.c:
//
//     if (!verifyRollbackLater()) {
//       ... if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
//             if (verifyOta()) esp_ota_mark_app_valid_cancel_rollback();
//
// und `verifyOta()` gibt in seiner schwachen Vorgabe blind `true` zurueck.
// Damit ist jedes Image bestaetigt, sobald es ueberhaupt bootet - und das
// Rollback ist Zierde. Genau davor warnt der Umsetzungsplan bei AP 3.7.1:
// "Eine Firmware, die bootet, aber deren Antrieb nicht anlaeuft, gibt sich
// sonst selbst frei."
//
// AM GERAET GESEHEN: nach dem ersten echten Update meldete 'y' die Partition
// als "app1, bestaetigt" statt "auf Bewaehrung".
//
// Diese Ueberschreibung des schwachen Symbols schiebt die Entscheidung dorthin,
// wo sie hingehoert: in die Hauptschleife, nach WLAN, Zeit und Referenzfahrt.
// `extern "C"`, weil das Symbol aus einer .c-Datei stammt.
extern "C" bool verifyRollbackLater() { return true; }

/// Laeuft gerade ein langer, blockierender Ablauf (Referenzfahrt, Grenzsuche,
/// Pendellauf)? Dann nimmt `kommando()` nur noch das Noetigste an.
bool langlauf = false;

/// Wird waehrend langer Ablaeufe (Referenzfahrt, Grenzsuche) aufgerufen.
/// Haelt Lichteffekte UND die Weboberflaeche am Laufen und nimmt einen
/// Abbruch an.
///
/// WARUM DIE OBERFLAECHE JETZT MITLAEUFT (24.08.2026): eine Referenzfahrt
/// dauert 40 bis 60 s. Vorher stand in dieser Zeit der Webserver - die Seite
/// lief in Zeitueberschreitungen, und wer zusah, sah ein totes Geraet. Genau
/// das ist beim Messen am 23./24.08. mehrfach passiert.
///
/// DIE URSPRUENGLICHE SORGE BLEIBT BERECHTIGT und ist deshalb nicht
/// weggefallen, sondern an die richtige Stelle gewandert: aus einer
/// Referenzfahrt heraus darf keine zweite gestartet werden. Das verhindert
/// jetzt `langlauf` in `kommando()` - dort, wo entschieden wird, statt hier,
/// wo nur gepumpt wird. Erlaubt bleiben 's' (Abbruch), 'i' und '?'.
bool pumpe() {
  licht.tick();
  testweb.tick();
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == 's' || c == 'S') {
      abbruch_gewuenscht = true;
      starwatch::konsole.println("\nAbbruch.");
    }
  }
  return !abbruch_gewuenscht;
}

/// Ein Ablauf, der die Schleife blockiert, klammert sich hiermit ein.
/// Als Objekt, damit auch ein `return` mitten drin das Flag zuruecksetzt.
struct Langlauf {
  Langlauf() { langlauf = true; }
  ~Langlauf() { langlauf = false; }
};

void hilfe() {
  starwatch::konsole.println(
      "\n=== Star Watch - Betriebsfirmware (AP 2) ===\n"
      "ZEIT\n"
      "  z HH:MM:SS  Uhrzeit von Hand stellen (im Netz kommt sie per NTP)\n"
      "  z <zone>    POSIX-Zeitzone setzen, ohne Wert nur anzeigen\n"
      "  m 0|1|2     Zeitmodus: 0 aus, 1 fliessend, 2 Minutensprung\n"
      "ZEIGER  (jeder Fahrbefehl schaltet den Uhrbetrieb ab, zurueck mit 'm 1')\n"
      "  p <n>       auf Mikroschritt n fahren (vorwaerts)\n"
      "  ps <n>      Ziel laufend nachschieben - kuerzester Weg, auch\n"
      "              rueckwaerts. Fuers Steuern von aussen, schweigt dabei\n"
      "  u <ziel>    anfahren: Stunde 0..12 ODER Uhrzeit HH:MM[:SS]\n"
      "              u 12   u 4.5   u 15:00   u 21:30   (24 h aufs Zwoelferblatt)\n"
      "  uz          auf zwoelf fahren und halten, bis 'o!' bestaetigt\n"
      "  v <n>       um n Schritte verstellen (+/-, auch rueckwaerts)\n"
      "  f <hz>      Fahrtempo in Schritten/s\n"
      "  a <n>       Rampe in Schritten/s^2; ohne Wert nur anzeigen\n"
      "  k <n>       Schwungkreise vor jedem Ziel (Effekt), 0 = aus\n"
      "  s           STOPP (Timer aus, Uhr aus, Rampe herunter)\n"
      "PRUEFLAEUFE\n"
      "  x <n> <z1> <z2>  n mal zwischen zwei Stellungen pendeln\n"
      "              x 10 12:00 18:00     x 5 12 6     x 20 21:00 3:00\n"
      "  g           Grenzsuche: acht Tempostufen, Nachweis ueber Hall-Flanken\n"
      "ZWOELF EINMESSEN - drei Schritte, in dieser Reihenfolge\n"
      "  h           1. Homing: der Zeiger sucht seine Nulllage\n"
      "  hu          Homing, danach sofort die Uhrzeit (Ende des Steuerns)\n"
      "  hf [f] [g]  Tempo der Referenzfahrt: Feinfahrt f, Grobsuche g (Schritte/s)\n"
      "  hv          Homing-Verlauf: Abweichungen und Offsetaenderungen, gespeichert\n"
      "  v <n>       2. auf zwoelf nachstellen, falls er daneben steht\n"
      "  o!          3. \"das ist zwoelf\" - rechnet den Offset selbst aus\n"
      "              danach 'h' zur Kontrolle. Landet er wieder auf zwoelf,\n"
      "              sitzt es.\n"
      "  o <n>       Offset von Hand zurueckschreiben (0..3199). NUR fuer\n"
      "              notierte Werte, nicht zum Einmessen\n"
      "TIMER\n"
      "  t <sek>     Timer starten (eine Umdrehung in dieser Zeit)\n"
      "  t-          Timer abbrechen\n"
      "  tl [0|1]    Laufart: 0 gleitend, 1 Sekundentakt (gespeichert, wirkt sofort)\n"
      "LICHT\n"
      "  l <p>       Helligkeit in Prozent (0 = aus)\n"
      "  la / lo     Klinge an / aus\n"
      "  lf [farbe]  Farbe der Klinge: lf ff8800, lf 255 136 0 oder lf gelb\n"
      "  lw [farbe]  Weiss dieser Uhr (gespeichert, ueberlebt Werksreset)\n"
      "              (zuendet neu und gleitet hinueber; 'lfr' ohne Zuendung)\n"
      "  lm <farbe> [muster] [dauer_s] [hell]  Meldung: leuchten|pulsieren|blinken\n"
      "  lm- / lm    Meldung quittieren / anzeigen\n"
      "  lu [0|1]    Lichtautomatik (dimmen nach Umgebungslicht), gespeichert\n"
      "  lg <ms> [a|o|z] [h <p>] [f <farbe>]  weich setzen, ab dem gezeigten Wert\n"
      "              (z = einschalten mit Zuendung)\n"
      "  lp [h <p>] [f <farbe>]  Puls   lp- [ms] beenden   lp periode <ms>   lp min <%>\n"
      "  lb <n>      n Blitze (flash)\n"
      "  lu n [0|1]  Nachtabschaltung  lu min <%>  lu hell <lx>  lu aus <lx>\n"
      "  lz / ll     zuenden / loeschen\n"
      "  ls          Stundeneffekt (60 s auf 100 %)\n"
      "  lr / lt     Rampe 0-20 % / Treppe 0-100 % (Messhilfen AP 0.5)\n"
      "  w <p>       PWM direkt in Prozent, ohne Kennlinie (F: mit Leistungsgrenze)\n"
      "MESSEN UND DIAGNOSE\n"
      "  i           Zustand\n"
      "  d           Treiber auslesen (blockiert rund 100 ms am UART)\n"
      "  c           Hall-Spitzenwerte zuruecksetzen\n"
      "  g           Grenzsuche (sieben Stufen, je 4 s)\n"
      "  e 0|1       Endstufe aus/ein (aus = Freilauf, die Klinge kippt)\n"
      "  r           Registersatz neu schreiben\n"
      "  n           WLAN und Weboberflaeche von Hand starten\n"
      "  nl          erreichbare Netze suchen\n"
      "  ns <ssid> <pass>  Heimnetz merken und verbinden (ohne neu zu flashen)\n"
      "  ni dhcp | ni <ip> <maske> <gw> [dns]  Adresse im Heimnetz (fest/DHCP)\n"
      "  na          eigenen Zugangspunkt abschalten (nur mit Heimnetz)\n"
      "  nr          Verbindung neu aufbauen (prueft den Ausfallpfad)\n"
      "  nx          gespeicherte Zugangsdaten vergessen\n"
      "GERAET\n"
      "  y           Firmware und Updates: Stand, Modus, URL\n"
      "  y 0|1|2     Updates aus / manuell / automatisch (taeglich, nachts)\n"
      "  yp / yi     jetzt nachsehen / gefundene Fassung einspielen\n"
      "  y!          laufende Fassung von Hand bestaetigen (Pruefstand ohne Hall)\n"
      "  b           Neustart\n"
      "  ib          I2C-Bus pruefen: Pegel, freitakten, neu suchen\n"
      "  ih [mA]     Haltestrom zeigen / setzen (gespeichert)\n"
      "  dn          TCP-Verbindungen der Uhr\n"
      "  b werksreset  Benutzerwerte und WLAN loeschen; Hall-Offset BLEIBT\n"
      "  alias [name|-]  Name der Uhr (Anzeige und Home Assistant), gespeichert\n"
      "\nEinstellungen (Tempo, Rampe, Kreise, Zeitmodus, Timer-Lauf, Helligkeit,\n"
      "Offset) "
      "ueberleben den Neustart. Export und Import gehen ueber die Oberflaeche.\n"
      "\nDieselben Kommandos gehen ueber die Test-Weboberflaeche im Browser.\n"
      "Sie ist ein Messgeraet und KEINE API - die entsteht in AP 3.4.");
}

// ===========================================================================
// ZEIT AUS DEM NETZ (AP 3.2)
// ===========================================================================
//
// Die Uhr hat WLAN, also kann sie ihre Zeit selbst holen - es waere sonderbar,
// sie von Hand stellen zu lassen. `configTzTime` startet den SNTP-Dienst des
// ESP-IDF; der laeuft danach von allein und stellt die SYSTEMzeit nach.
//
// WAS DIESE FIRMWARE ZUSAETZLICH TUN MUSS: die Zeitbasis in `Betrieb` rechnet
// mit `millis()`, und dessen Quarz geht um einige ppm falsch. SNTP korrigiert
// die Systemzeit, nicht diese Basis. Deshalb wird sie im Takt von
// NTP_NACHSTELLEN_MS nachgezogen - sonst wandert die Anzeige langsam weg,
// obwohl die Systemzeit stimmt.
//
// OHNE NETZ BLEIBT ALLES WIE VORHER: am eigenen Zugangspunkt oder in einem
// Netz ohne Internet gibt es keine NTP-Zeit, und dann gilt weiter 'z HH:MM:SS'
// oder der Browser-Knopf.

bool ntp_gestartet = false;
bool ntp_gemeldet = false;
uint32_t ntp_letztes_nachstellen = 0;

void ntp_starten() {
  if (ntp_gestartet || !testweb.verbunden() || testweb.eigener_ap())
    return;
  configTzTime(betrieb.zeitzone().c_str(), g::NTP_SERVER_1, g::NTP_SERVER_2);
  ntp_gestartet = true;
  starwatch::konsole.printf("Zeit aus dem Netz: frage %s / %s, Zeitzone %s\n",
                g::NTP_SERVER_1, g::NTP_SERVER_2, betrieb.zeitzone().c_str());
}

/// Systemzeit in die Zeitbasis der Uhr uebernehmen.
void ntp_nachstellen() {
  if (!ntp_gestartet)
    return;
  // Kam das WLAN gerade zurueck, sofort nachstellen statt auf den Takt zu
  // warten: SNTP stellt nur stuendlich nach, und nach einem langen Ausfall ist
  // die Zeit dann noch eine Stunde daneben.
  if (testweb.wlan_rueckkehr_abholen()) {
    ntp_letztes_nachstellen = 0;
    starwatch::konsole.println("[netz] Zeit wird sofort neu geholt.");
  }

  // Solange noch keine Zeit da war, oft fragen - danach selten. Ohne
  // Zeitchip (AP 1.2, entschieden am 24.08.2026) ist das die einzige Quelle.
  const uint32_t takt = ntp_gemeldet ? g::NTP_NACHSTELLEN_MS : g::NTP_ERSTVERSUCH_MS;
  if (millis() - ntp_letztes_nachstellen < takt && ntp_letztes_nachstellen != 0)
    return;

  struct tm t;
  // Frist 0: NICHT warten. Diese Funktion laeuft aus der Hauptschleife, und
  // eine blockierende Abfrage haette den Zeiger angehalten.
  if (!getLocalTime(&t, 0)) {
    ntp_letztes_nachstellen = millis();
    return;
  }
  ntp_letztes_nachstellen = millis();
  betrieb.zeit_setzen(t.tm_hour, t.tm_min, t.tm_sec);
  betrieb.zeit_aus_netz(true);
  if (!ntp_gemeldet) {
    ntp_gemeldet = true;
    // Lief beim Ausschalten ein Timer? Jetzt, mit gueltiger Zeit, laesst sich
    // sein gemerktes Ende einordnen.
    const int32_t rest = betrieb.timer_wiederaufnehmen((uint32_t)time(nullptr));
    if (rest > 0)
      starwatch::konsole.printf(
          "\nTimer wieder aufgenommen: noch %ld s (das Ende war gemerkt).\n",
          (long)rest);
    else if (rest < 0)
      starwatch::konsole.println(
          "\nEin gemerkter Timer ist waehrend der Auszeit abgelaufen.\n"
          "Er wird NICHT nachtraeglich ausgeloest - ein Wecker, der Stunden zu\n"
          "spaet klingelt, ist eine Falschmeldung.");
    starwatch::konsole.printf(
        "\nZeit steht: %02d:%02d:%02d (%02d.%02d.%04d) aus dem Netz.\n"
        "Der gemerkte Zeitmodus laeuft damit von selbst an.\n> ",
        t.tm_hour, t.tm_min, t.tm_sec, t.tm_mday, t.tm_mon + 1,
        t.tm_year + 1900);
  }
}

/// Ein Handeingriff nimmt die Uhr aus der Fuehrung (siehe betrieb.h). Das
/// darf nicht stillschweigend geschehen - sonst wundert sich der Bediener
/// spaeter, warum die Uhr steht.
void uhr_ging_aus(bool ja) {
  if (ja)
    starwatch::konsole.println("Uhrbetrieb dafuer abgeschaltet. Zurueck mit 'm 1'.");
}

/// Nach einer Referenzfahrt: war die Abweichung ein Verlust? (05.10.2026)
/// Ganze Perioden sind die Handschrift des Motors (Haltestrom zu klein,
/// Ueberlast), eine krumme Zahl spricht fuer Riemen, Magnet oder Mechanik.
void verlust_melden(const char *vorsatz) {
  if (!betrieb.letztes_homing().ok() || !betrieb.letzte_vergleichbar())
    return;
  const starwatch::Verlustbefund b = starwatch::schrittverlust(
      betrieb.letzte_abweichung(), g::SCHRITTE_JE_PERIODE, g::HOMING_STREUUNG);
  if (b.periodisch)
    starwatch::konsole.printf(
        "%sSCHRITTVERLUST: %+ld Schritte = %+ld ganze Perioden (Rest %+ld) bei\n"
        "%sHaltestrom %u mA. Der Motor hat seit der letzten Referenz Schritte\n"
        "%sverloren - 'hv' zeigt den Verlauf.\n",
        vorsatz, (long)betrieb.letzte_abweichung(), (long)b.perioden, (long)b.rest, vorsatz,
        (unsigned)antrieb.haltestrom(), vorsatz);
  else if (b.verlust)
    starwatch::konsole.printf(
        "%sABWEICHUNG %+ld Schritte, KEINE ganze Periode (Rest %+ld) - eher\n"
        "%sRiemen, Magnet oder Mechanik als der Motor. 'hv' zeigt den Verlauf.\n",
        vorsatz, (long)betrieb.letzte_abweichung(), (long)b.rest, vorsatz);
}

/// 'hv': die gespeicherten Referenzfahrten und Offsetaenderungen.
void verlauf_zeigen() {
  const starwatch::Homingverlauf &v = betrieb.homingverlauf();
  starwatch::konsole.printf(
      "Homing-Verlauf, neueste zuerst (%u Eintraege, ueberlebt den Neustart).\n"
      "Abw = Abweichung zur vorigen Referenz; ganze Perioden (%ld Schritte)\n"
      "heissen Schrittverlust.\n",
      (unsigned)v.anzahl(), (long)g::SCHRITTE_JE_PERIODE);
  for (size_t i = 0; i < v.anzahl(); i++)
    starwatch::konsole.printf("  %s\n", betrieb.homingeintrag_text(v.neuer(i)).c_str());
}

void zustand() {
  const int32_t pos = antrieb.position();
  starwatch::konsole.println("\n--- Zustand ---");
  starwatch::konsole.printf("Zeitquelle   %s, Zone %s\n",
                betrieb.zeit_aus_netz() ? "Netz (NTP)" : "von Hand / unbekannt",
                betrieb.zeitzone().c_str());
  if (testweb.wlan_weg_ms() > 0)
    starwatch::konsole.printf(
        "WLAN         WEG seit %lu s. Die Uhr laeuft weiter, nur die\n"
        "             Zeitkorrektur pausiert - Drift etwa 1 bis 4 s je Tag.\n",
        (unsigned long)(testweb.wlan_weg_ms() / 1000u));
  starwatch::konsole.printf("Uhrzeit      %s   Modus %s\n", betrieb.uhrzeit_text().c_str(),
                starwatch::zeitmodus_text(betrieb.zeitmodus()));
  starwatch::konsole.printf("Zeiger       Schritt %ld von %ld, zeigt %s\n", (long)pos,
                (long)g::REV,
                starwatch::zeigerzeit(pos, g::REV, g::SEK_JE_SCHRITT).c_str());

  const int32_t soll = betrieb.soll_jetzt();
  if (soll >= 0)
    starwatch::konsole.printf("             Soll %ld, Abweichung %ld Schritte\n", (long)soll,
                  (long)starwatch::drift(pos, soll, g::REV));

  starwatch::konsole.printf("             Ziel %ld, %s\n", (long)antrieb.ziel(),
                antrieb.laeuft() ? "FAEHRT" : "steht");

  const char *wer = "niemand";
  switch (betrieb.fuehrung()) {
    case starwatch::Fuehrung::homing: wer = "Homing"; break;
    case starwatch::Fuehrung::hand:   wer = "Hand";          break;
    case starwatch::Fuehrung::timer:  wer = "Timer";         break;
    case starwatch::Fuehrung::uhr:    wer = "Uhrbetrieb";    break;
    case starwatch::Fuehrung::niemand: break;
  }
  starwatch::konsole.printf("Fuehrung     %s\n", wer);
  starwatch::konsole.printf("Endstufe     %s\n", antrieb.endstufe_an() ? "bestromt" : "AUS (Freilauf)");

  starwatch::konsole.printf("Referenz     %s\n",
                starwatch::hominggrund_text(betrieb.letztes_homing().grund));
  if (betrieb.letztes_homing().ok())
    starwatch::konsole.printf("             Schwerpunkt %.2f, Nachlauf %ld, Spanne %.2f mT\n",
                  (double)betrieb.letztes_homing().schwerpunkt,
                  (long)betrieb.letztes_homing().nachlauf,
                  (double)betrieb.letztes_homing().spanne_mt);
  if (!betrieb.referenz_gueltig())
    starwatch::konsole.println(
        "             KEINE GUELTIGE REFERENZ - der Uhrbetrieb ist gesperrt.\n"
        "             Der Zeiger bleibt stehen, statt eine geratene Zeit zu\n"
        "             zeigen. Von Hand fahren geht ('u', 'p', 'v'). Mit 'h'\n"
        "             eine Referenzfahrt starten.");
  starwatch::konsole.printf("Hall-Offset  %ld Schritte\n", (long)betrieb.hall_offset());

  if (!hall.bereit())
    starwatch::konsole.printf("Hall         SENSOR FEHLT: %s - ohne ihn kann die Uhr nicht "
                  "referenzieren\n", starwatch::tmaggrund_text(hall.sensorgrund()));
  else
    starwatch::konsole.printf("Hall         %s, Feld %.2f mT, Ruhe %.2f mT, Spanne %.2f mT "
                  "(TMAG5273 auf 0x%02X)\n",
                  hall.magnet_erkannt() ? "MAGNET" : "frei", (double)hall.feld_gefiltert(),
                  (double)hall.ruhe_mt(), (double)hall.spanne_mt(),
                  (unsigned)hall.sensoradresse());
  starwatch::konsole.printf("             Flanken %lu, Fenster %ld Schritte%s\n",
                (unsigned long)hall.flanken(), (long)hall.fenster_schritte(),
                hall.fenster_schritte() == 0 ? " (noch nicht gemessen)" : "");

  if (!lux.bereit())
    starwatch::konsole.printf("Umgebungslicht  %s - Automatik ohne Wirkung\n",
                  starwatch::luxgrund_text(lux.grund()));
  else if (!lux.gemessen())
    starwatch::konsole.println("Umgebungslicht  VEML7700 da, noch kein Messwert");
  else
    starwatch::konsole.printf("Umgebungslicht  %.1f lx (Stufe %u, x%.3f, %u ms, roh %u), "
                  "Automatik %s, Faktor %.0f %%\n",
                  (double)lux.lux(), (unsigned)lux.stufe(), (double)lux.verstaerkung(),
                  (unsigned)lux.integrationszeit_ms(), (unsigned)lux.rohwert(),
                  betrieb.lichtautomatik().an ? "an" : "aus",
                  (double)(licht.dimmfaktor() * 100.0f));

  starwatch::konsole.printf("Licht        %s, Regler %.0f %%, Ausgabe %.1f %%, Effekt %s\n",
                licht.ist_an() ? "an" : "aus", (double)licht.soll_prozent(),
                (double)licht.ausgabe_prozent(),
                starwatch::effekt_text(licht.effekt()));
  starwatch::konsole.printf("Farbe        #%06lX, LED %.2f W von hoechstens %.2f W, %lu Hz\n",
                              (unsigned long)licht.farbe(), (double)licht.leistung_w(),
                              (double)g::LED_MAX_W, (unsigned long)g::LED_PWM_HZ);
  starwatch::konsole.printf("Timer        %s (Rest %s), %s\n",
                betrieb.timer_zustand_text(),
                betrieb.timer_restzeit().c_str(),
                starwatch::timerlauf_text(betrieb.timerlauf()));
  if (!betrieb.letztes_homing().ok() && homing_letzter_versuch_ms != 0) {
    const uint32_t frist = homing_fehlschlaege >= g::HOMING_VERSUCHE_KURZ
                               ? g::HOMING_WIEDERHOLUNG_LANG_MS
                               : g::HOMING_WIEDERHOLUNG_MS;
    const uint32_t seit = millis() - homing_letzter_versuch_ms;
    starwatch::konsole.printf(
        "Selbstheilung %lu Fehlschlaege, naechster Versuch in %.1f min\n",
        (unsigned long)homing_fehlschlaege,
        seit >= frist ? 0.0 : (double)(frist - seit) / 60000.0);
  }
  if (betrieb.rueckfall_gesperrt())
    starwatch::konsole.println("Rueckfall    GESPERRT (Einmessen laeuft) - 'm 1' oder 'm 2' gibt frei");
  else if (betrieb.rueckfall_rest_ms() > 0)
    starwatch::konsole.printf("Rueckfall    in %.1f s auf %s\n",
                  (double)betrieb.rueckfall_rest_ms() / 1000.0,
                  starwatch::zeitmodus_text(betrieb.rueckfall_modus()));
  starwatch::konsole.printf("Mikroschritte 1/%ld -> %ld je Umdrehung, %.3f s je Schritt\n",
                (long)g::MIKROSCHRITTE, (long)g::REV, (double)g::SEK_JE_SCHRITT);
}

void diagnose() {
  starwatch::konsole.println("\n--- Treiber ---");
  const uint8_t v = antrieb.ic_version();
  starwatch::konsole.printf("IC-Version   0x%02X %s\n", v,
                v == 0x21 ? "(TMC2209)"
                          : (v == 0x20 ? "(TMC2208 - falscher Chip!)"
                                       : "(keine gueltige Antwort)"));
  if (v != 0x21) {
    starwatch::konsole.println(
        "Keine Antwort. Die Verdrahtung ist Teil des Bretts - es bleiben, in\n"
        "dieser Reihenfolge:\n"
        " 1 12 V an J1 fehlen oder F1 hat ausgeloest. VM (U2.22/U2.28) ist\n"
        "   +12V; ohne Motorspannung schweigt der Treiber.\n"
        " 2 Loetstelle am U2 (QFN, nur mit der Lupe) oder an R6 (1 kOhm\n"
        "   zwischen TX_VOR_R und UART). Messpunkte: TP8 = TX, TP9 = UART.\n"
        " 3 Adresse: MS1/MS2 (U2.9/U2.10) liegen fest auf GND, also 0x00.\n"
        "   Steht in geraet.h etwas anderes, ist DAS der Fehler.");
    return;
  }
  starwatch::konsole.printf("Zustand      %s\n",
                starwatch::treiberzustand_text(antrieb.zustand()));
  starwatch::konsole.printf("Mikroschritte 1/%u (aus dem Chip gelesen)\n", antrieb.mikroschritte());
  starwatch::konsole.printf("Motorstrom   %u mA RMS\n", antrieb.strom_ma());
  starwatch::konsole.printf("Motorlast    %u (StallGuard, faellt an der Grenze)\n", antrieb.last());
  starwatch::konsole.printf("DRV_STATUS   0x%08lX\n", (unsigned long)antrieb.drv_status());
}

/// Eine Zeitangabe in eine Zielposition auf dem Blatt uebersetzen.
///
/// Nimmt BEIDES an, weil beides gebraucht wird:
///   "15:00" / "15:30:20"  eine Uhrzeit - 24 h landen auf dem Zwoelferblatt,
///                         15:00 zeigt also wie 3:00. Das rechnet
///                         soll_position() ohnehin, hier steht kein Modulo.
///   "3" / "4.5"           eine Stunde 0..12, wie 'u' es schon konnte.
///
/// @return false, wenn nichts Brauchbares darin stand.
bool zeit_zu_position(const char *text, int32_t *ziel) {
  if (text == nullptr || *text == '\0' || ziel == nullptr)
    return false;

  int hh = 0, mm = 0, ss = 0;
  if (strchr(text, ':') != nullptr) {
    const int felder = sscanf(text, "%d:%d:%d", &hh, &mm, &ss);
    if (felder < 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59)
      return false;
    *ziel = starwatch::soll_position(hh * 3600 + mm * 60 + ss, g::REV,
                                     g::SEK_JE_SCHRITT);
    return true;
  }

  char *ende = nullptr;
  const float stunde = strtof(text, &ende);
  if (ende == text || stunde < 0.0f || stunde > 12.0f)
    return false;
  *ziel = starwatch::soll_position(static_cast<int32_t>(stunde * 3600.0f), g::REV,
                                   g::SEK_JE_SCHRITT);
  return true;
}

/// Warten, bis der Zeiger sein Ziel erreicht hat. Haelt dabei die Konsole am
/// Leben und laesst 's' abbrechen.
/// @return false, wenn abgebrochen oder die Frist verstrichen ist.
bool warte_auf_ziel(uint32_t frist_ms) {
  const uint32_t ab = millis();
  while (antrieb.laeuft()) {
    if (!pumpe())
      return false;
    hall.tick(antrieb.position());
    if (millis() - ab > frist_ms)
      return false;
    delay(1);
  }
  return true;
}

/// Frist fuer eine Fahrt: dreifache Sollzeit plus zwei Sekunden.
uint32_t frist_fuer(int32_t schritte) {
  const uint32_t tempo = antrieb.tempo() > 0 ? antrieb.tempo() : 1;
  const int32_t weg = schritte < 0 ? -schritte : schritte;
  return static_cast<uint32_t>(3000.0 * weg / tempo) + 2000;
}

/// Pendellauf: n mal zwischen zwei Uhrzeiten hin und her.
///
/// Wozu: eine einzelne Fahrt sagt wenig. Erst die Wiederholung zeigt, ob der
/// Antrieb bei diesem Tempo Schritte verliert - und weil der Zeiger am Ende
/// wieder dort stehen MUSS, wo er angefangen hat, ist die Abweichung am Schluss
/// das Mass dafuer. Der Impulszaehler taugt dazu nicht; er zaehlt auch, was
/// die Welle nicht getan hat. Deshalb steht am Ende der Vergleich mit dem
/// Hall-Sensor.
void pendellauf(long runden, int32_t ziel_a, int32_t ziel_b) {
  Langlauf sperre;
  const int32_t start_pos = antrieb.position();
  const uint32_t flanken_vorher = hall.flanken();
  const uint32_t ab = millis();

  starwatch::konsole.printf(
      "\nPendellauf: %ld mal zwischen Schritt %ld und %ld,\n"
      "%lu Schritte/s = %.2f U/s, Rampe %lu. 's' bricht ab.\n\n",
      runden, (long)ziel_a, (long)ziel_b, (unsigned long)antrieb.tempo(),
      (double)antrieb.tempo() / g::REV,
      (unsigned long)antrieb.beschleunigung());

  for (long i = 1; i <= runden; i++) {
    for (int b = 0; b < 2; b++) {
      const int32_t ziel = (b == 0) ? ziel_a : ziel_b;
      const int32_t vorher = antrieb.ziel();
      betrieb.fahre_auf(ziel);
      const int32_t weg = antrieb.ziel() - vorher;
      const uint32_t t0 = millis();
      if (!warte_auf_ziel(frist_fuer(weg))) {
        starwatch::konsole.println("Pendellauf abgebrochen.");
        goto ende;
      }
      starwatch::konsole.printf("  %3ld.%d  %5ld Schritte in %5.2f s\n", i, b + 1,
                    (long)weg, (double)(millis() - t0) / 1000.0);
    }
  }

ende:
  {
    const int32_t weg_gesamt = antrieb.position() - start_pos;
    const uint32_t flanken = hall.flanken() - flanken_vorher;

    // Jede volle Umdrehung MUSS eine Flanke gemacht haben. Die Zahl ist
    // BRUCHTEILIG anzugeben: bei 9,7 Umdrehungen sind 9 oder 10 Flanken
    // beides richtig, je nachdem wo der Magnet stand. Ganzzahlig geteilt sah
    // "Zaehler 9 / Flanken 10" nach einem Widerspruch aus, der keiner ist.
    const double runden = static_cast<double>(weg_gesamt) / g::REV;
    const bool passt = flanken + 1 >= static_cast<uint32_t>(runden) &&
                       flanken <= static_cast<uint32_t>(runden) + 1;

    starwatch::konsole.printf(
        "\nGesamt %ld Schritte in %.2f s = %.2f Umdrehungen.\n"
        "Hall-Flanken %lu, erwartet %.2f (+/-1) - %s\n"
        "Zeiger steht auf Schritt %ld (Blatt %ld).\n",
        (long)weg_gesamt, (double)(millis() - ab) / 1000.0, runden,
        (unsigned long)flanken, runden,
        passt ? "passt zusammen."
              : "PASST NICHT: die Welle hat nicht alles mitgedreht.",
        (long)antrieb.position(),
        (long)starwatch::normiere(antrieb.position(), g::REV));
  }
}

/// Grenzsuche: sieben Stufen von 0,5 bis 6 Umdrehungen je Sekunde.
///
/// GEZAEHLT WERDEN HALL-FLANKEN, NICHT IMPULSE. Das ist der ganze Punkt der
/// Neufassung vom 23.08.2026. Die alte Fassung verglich den Impulszaehler mit
/// der Sollzahl - und der Zaehler zaehlt, was der ESP32 AUSGEGEBEN hat, nicht
/// was die Welle getan hat. Bleibt der Motor stehen, stimmt er trotzdem. Genau
/// diese Verwechslung hat am 23.08. eine halbe Stunde Fehlersuche gekostet:
/// A0 schlug beim Verstellen um 1,7 V aus, obwohl der Rotor stand - das war
/// das Statorfeld bei geaenderter Phasenlage.
///
/// Der Magnet laeuft einmal je Umdrehung am Sensor vorbei, also MUSS die Zahl
/// der Flanken der Zahl der Umdrehungen entsprechen. Fehlen Flanken, hat die
/// Welle nicht mitgedreht - unabhaengig davon, was der Zaehler sagt.
///
/// Die Entprellung von D0 (10 ms) traegt das: bei 6 U/s ist das Sensorfenster
/// noch rund 28 ms breit. Erst oberhalb von etwa 16 U/s wuerde sie zum
/// Problem, und so schnell laesst TEMPO_MAX es nicht zu.
///
/// ACHTUNG BEI DER AUSWERTUNG: die Impulse entstehen in HARDWARE (MCPWM),
/// nicht in der Hauptschleife. Die Grenze, die man findet, ist damit wirklich
/// der Motor und nicht der Rechner - anders als im ESPHome-Pruefstand, wo
/// oberhalb von rund 2 kSchritten/s der ESP32 aufgab.
void grenzsuche() {
  Langlauf sperre;
  // In Umdrehungen je Sekunde gedacht und daraus gerechnet - so steht der
  // Bezug zur Aufgabe im Code und nicht im Kopf des Lesers.
  static const float u_je_s[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
  constexpr int32_t RUNDEN = 3;

  starwatch::konsole.printf(
      "\nGrenzsuche: %d Umdrehungen je Stufe, Hall-Flanken als Nachweis.\n"
      "Rampe %lu Schritte/s^2. Zuhoeren und zusehen; 's' bricht ab.\n\n"
      "  Stufe    Tempo      soll     gemessen   Flanken   Urteil\n",
      (int)RUNDEN, (unsigned long)antrieb.beschleunigung());

  uint32_t letzte_gute = 0;

  for (size_t i = 0; i < sizeof(u_je_s) / sizeof(u_je_s[0]); i++) {
    const uint32_t tempo = static_cast<uint32_t>(u_je_s[i] * g::REV);
    if (tempo == 0 || tempo > g::TEMPO_MAX)
      continue;

    const uint32_t flanken_vorher = hall.flanken();
    const int32_t weg = RUNDEN * g::REV;

    // Sollzeit: Fahrt auf Tempo plus die beiden Rampen. Die Rampen kosten
    // zusammen so viel Zeit wie eine Fahrt der Strecke v^2/a bei v.
    const double soll_s =
        static_cast<double>(weg) / tempo +
        static_cast<double>(tempo) / antrieb.beschleunigung();

    antrieb.tempo(tempo);
    const uint32_t ab = millis();
    antrieb.fahre_weiter(weg);

    // Grosszuegig, aber endlich: dreifache Sollzeit plus zwei Sekunden.
    const uint32_t frist = static_cast<uint32_t>(soll_s * 3000.0) + 2000;
    bool abgebrochen = false;
    while (antrieb.laeuft()) {
      if (!pumpe()) {
        abgebrochen = true;
        break;
      }
      hall.tick(antrieb.position());
      if (millis() - ab > frist)
        break;
      delay(1);
    }
    const uint32_t gemessen = millis() - ab;
    const uint32_t flanken = hall.flanken() - flanken_vorher;

    const char *urteil;
    if (flanken >= static_cast<uint32_t>(RUNDEN)) {
      urteil = "dreht";
      letzte_gute = tempo;
    } else if (flanken == 0) {
      urteil = "STEHT - keine einzige Flanke";
    } else {
      urteil = "VERLIERT Umdrehungen";
    }

    starwatch::konsole.printf("  %.1f U/s   %5lu/s   %6.2f s   %6.2f s   %2lu/%ld   %s\n",
                  (double)u_je_s[i], (unsigned long)tempo, soll_s,
                  (double)gemessen / 1000.0, (unsigned long)flanken,
                  (long)RUNDEN, urteil);

    if (abgebrochen)
      break;
    // Zwischen den Stufen zur Ruhe kommen, sonst wandert die Rampe der einen
    // in die Messung der naechsten.
    antrieb.stopp_mit_rampe();
    delay(300);
  }

  antrieb.rampe(g::BESCHLEUNIGUNG, g::BESCHLEUNIGUNG);
  antrieb.tempo(g::TEMPO_FAHRT);

  if (letzte_gute > 0)
    starwatch::konsole.printf(
        "\nHoechste Stufe mit vollstaendigen Flanken: %lu Schritte/s = %.1f U/s.\n"
        "Das ist der Wert fuer TEMPO_FAHRT in geraet.h und fuer das\n"
        "Messprotokoll - mit Sicherheitsabstand nach unten.\n",
        (unsigned long)letzte_gute, (double)letzte_gute / g::REV);
  else
    starwatch::konsole.println("\nKeine Stufe hat vollstaendig gedreht. Erst Strom, Rampe und\n"
                   "Magnetabstand pruefen - siehe 'd'.");
}

// ---------------------------------------------------------------------------
// Kommandos
// ---------------------------------------------------------------------------

void kommando(char *z) {
  // Fuehrende Leerzeichen weg
  while (*z == ' ')
    z++;
  if (*z == '\0')
    return;

  const char k = z[0];
  char *rest = z + 1;
  while (*rest == ' ')
    rest++;

  // Waehrend eines blockierenden Ablaufs nur das Noetigste. Diese Abfrage ist
  // der Ersatz fuer die frueher stumme `pumpe()` - siehe dort.
  if (langlauf && k != 's' && k != 'i' && k != '?') {
    starwatch::konsole.printf(
        "Laeuft gerade (Homing, Grenzsuche oder Pendellauf).\n"
        "Jetzt gehen nur 's' (Abbruch), 'i' (Zustand) und '?'. '%c' ignoriert.\n", k);
    return;
  }

  switch (k) {
    case '?':
      hilfe();
      return;

    case 'i':
      if (z[1] == 'h') {
        // ih [mA]: Haltestrom zeigen oder setzen (01.10.2026). Er fliesst
        // rund um die Uhr; die Verluste im Motor wachsen mit dem Quadrat.
        const char *arg = z + 2;
        while (*arg == ' ')
          arg++;
        if (*arg != '\0') {
          const long ma = atol(arg);
          if (ma <= 0) {
            starwatch::konsole.printf("ih <%u..%u mA>\n", (unsigned)g::HALTESTROM_MIN,
                                      (unsigned)g::STROM_LAUF);
            return;
          }
          betrieb.haltestrom_setzen(static_cast<uint16_t>(ma));
        }
        starwatch::konsole.printf("Haltestrom %u mA (Lauf %u mA), gemeldet %u mA.\n",
                                  (unsigned)antrieb.haltestrom(), (unsigned)g::STROM_LAUF,
                                  (unsigned)antrieb.strom_ma());
        return;
      }
      if (z[1] == 'b') {
        // ib: I2C-Bus pruefen, freitakten, neu absuchen, Sensoren neu anmelden.
        bool sda = false, scl = false, sda2 = false;
        starwatch::i2cbus::pruefen(&sda, &scl, &sda2);
        starwatch::konsole.printf("I2C-Pegel    SDA %s, SCL %s (GPIO%u/%u, Pull-ups R11/R12)\n",
                                  sda ? "hoch" : "TIEF", scl ? "hoch" : "TIEF",
                                  g::PIN_SDA, g::PIN_SCL);
        if (!sda || !scl)
          starwatch::konsole.printf(
              "             %s\n",
              !scl ? "SCL tief: Masseschluss an SCL, Stecker verdreht oder Ader gequetscht."
                   : (sda2 ? "SDA hing - nach 9 Takten frei. Ein Baustein hatte den Bus gehalten."
                           : "SDA bleibt tief: Masseschluss an SDA oder Stecker verdreht."));
        uint8_t gefunden[8];
        const size_t n = starwatch::i2cbus::absuchen(gefunden, sizeof(gefunden));
        starwatch::konsole.printf("I2C-Bus      %u Teilnehmer\n", (unsigned)n);
        for (size_t i = 0; i < n && i < sizeof(gefunden); i++) {
          const char *wer = starwatch::i2cbus::name(gefunden[i]);
          starwatch::konsole.printf("             0x%02X  %s\n", (unsigned)gefunden[i],
                                    wer != nullptr ? wer : "UNBEKANNT");
        }
        if (n == 0 && sda && scl)
          starwatch::konsole.println(
              "             Leitungen frei, aber niemand antwortet: die Sensoren\n"
              "             bekommen keine Versorgung (3V3/GND an J5/J8) oder die\n"
              "             Stecker sitzen nicht.");
        const bool h = hall.beginn();
        const bool l = lux.beginn();
        starwatch::konsole.printf("Neu angemeldet: Hall %s, Licht %s.%s\n", h ? "ja" : "nein",
                                  l ? "ja" : "nein", h ? " Jetzt 'h'." : "");
        return;
      }
      zustand();
      return;

    case 'd':
      if (z[1] == 'n') {
        // dn: alle TCP-Verbindungen der Uhr (01.10.2026). Port 80 nahm von
        // aussen minutenlang keine Verbindung an, waehrend Port 23 ging - und
        // der Stapel kennt nur CONFIG_LWIP_MAX_ACTIVE_TCP = 16 Verbindungen.
        static const char *const ZUSTAND[] = {"CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD",
                                              "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
                                              "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"};
        // ERST SAMMELN, DANN DRUCKEN: die Ausgabe laeuft auch ueber Telnet,
        // also ueber denselben Stapel - unter seiner Sperre zu schreiben,
        // haengte die Uhr auf.
        char text[1100];
        size_t t = 0;
        int n = 0;
        auto zeile = [&](const char *fmt, auto... a) {
          if (t < sizeof(text))
            t += snprintf(text + t, sizeof(text) - t, fmt, a...);
        };
        LOCK_TCPIP_CORE();
        for (int liste = 0; liste < 2; liste++) {
          for (struct tcp_pcb *p = liste == 0 ? tcp_active_pcbs : tcp_tw_pcbs; p != nullptr;
               p = p->next) {
            char ip[16];
            ipaddr_ntoa_r(&p->remote_ip, ip, sizeof(ip));
            zeile("  %-11s :%u <- %s:%u\n", p->state <= TIME_WAIT ? ZUSTAND[p->state] : "?",
                  (unsigned)p->local_port, ip, (unsigned)p->remote_port);
            n++;
          }
        }
        for (struct tcp_pcb_listen *l = tcp_listen_pcbs.listen_pcbs; l != nullptr; l = l->next)
#if TCP_LISTEN_BACKLOG
          zeile("  LISTEN      :%u  wartend %u von %u\n", (unsigned)l->local_port,
                (unsigned)l->accepts_pending, (unsigned)l->backlog);
#else
          zeile("  LISTEN      :%u\n", (unsigned)l->local_port);
#endif
        UNLOCK_TCPIP_CORE();
        if (t >= sizeof(text))
          t = sizeof(text) - 1;
        text[t] = '\0';
        starwatch::konsole.print(text);
        starwatch::konsole.printf("TCP: %d Verbindungen (hoechstens %d)\n", n,
                                  CONFIG_LWIP_MAX_ACTIVE_TCP);
        return;
      }
      if (z[1] == 'm') {
        // dm: eigener Zaehler gegen den Mikroschrittzaehler des Treibers
        // (27.09.2026). Je Mikroschritt zaehlt MSCNT um 256/MIKROSCHRITTE,
        // Richtung je nach Verdrahtung - deshalb beide Differenzen. Die
        // passende bleibt zwischen zwei Referenzfahrten KONSTANT, solange
        // jeder Impuls ankommt. Aendert sie sich, fehlen Impulse; bleibt sie
        // und der Magnet wandert trotzdem, rutscht der Rotor.
        if (antrieb.laeuft()) {
          starwatch::konsole.println("dm nur im Stillstand.");
          return;
        }
        const int32_t pos = antrieb.position();
        const int32_t ms = antrieb.mscnt();
        const int32_t je = 256 / g::MIKROSCHRITTE;
        const int32_t vor = ((ms - je * pos) % 1024 + 1024) % 1024;
        const int32_t rueck = ((ms + je * pos) % 1024 + 1024) % 1024;
        starwatch::konsole.printf("MSCNT %ld bei Zaehler %ld: Differenz vor %ld / rueck %ld\n",
                                  (long)ms, (long)pos, (long)vor, (long)rueck);
        return;
      }
      diagnose();
      return;

    case 'c':
      hall.spitzen_ruecksetzen();
      hall.zaehler_ruecksetzen();
      starwatch::konsole.println("Hall-Spitzenwerte, Flanken und Fensterbreite zurueckgesetzt.");
      return;

    case 'g':
      abbruch_gewuenscht = false;
      grenzsuche();
      return;

    case 's':
      betrieb.stopp();
      starwatch::konsole.println("STOPP. Timer aus, Uhrbetrieb aus, Zeiger bremst ab.");
      return;

    case 'n': {
      const char zweit = z[1];
      if (zweit == 'l') {  // nl = Netze listen
        testweb.netze_suchen();
        return;
      }
      if (zweit == 'a') {  // na = eigenen Zugangspunkt abschalten
        testweb.ap_abschalten();
        return;
      }
      if (zweit == 'r') {  // nr = Verbindung neu aufbauen
        // WOZU: der Ausfallpfad soll pruefbar sein, ohne den Router
        // anzufassen. `WiFi.reconnect()` trennt und verbindet mit den
        // GESPEICHERTEN Daten - die Aufsicht sieht denselben Ablauf wie bei
        // einem echten Ausfall, inklusive Zeit-Nachstellen danach.
        starwatch::konsole.println("Verbindung wird neu aufgebaut (Test des Ausfallpfads).");
        testweb.neu_verbinden();
        return;
      }
      if (zweit == 'x') {  // nx = Zugangsdaten vergessen
        testweb.wlan_setzen("", "");
        return;
      }
      if (zweit == 'i') {  // ni dhcp | ni <ip> <maske> <gateway> [dns]
        // Die Adresse im GESPEICHERTEN Netz umstellen - ueber das Kabel der
        // Rettungsweg, wenn eine feste Adresse nicht passt und die Uhr im
        // Heimnetz nicht mehr zu finden ist.
        char *arg = z + 2;
        while (*arg == ' ')
          arg++;
        const String ssid = testweb.gespeicherte_ssid();
        if (ssid.length() == 0) {
          starwatch::konsole.println("Kein Heimnetz gespeichert - erst 'ns <SSID> <PASSWORT>'.");
          return;
        }
        starwatch::Testweb::Adresswahl a;
        if (strcmp(arg, "dhcp") != 0) {
          char ip[20] = "", maske[20] = "", gw[20] = "", dns[20] = "";
          if (sscanf(arg, "%19s %19s %19s %19s", ip, maske, gw, dns) < 3) {
            const auto g2 = testweb.gespeicherte_adresse();
            starwatch::konsole.printf(
                "Adresse jetzt: %s\n"
                "Format: ni dhcp   oder   ni <ip> <maske> <gateway> [dns]\n"
                "        ni 192.168.1.50 255.255.255.0 192.168.1.1\n",
                g2.fest ? (g2.ip + " / " + g2.maske + ", Gateway " + g2.gw).c_str()
                        : "per DHCP");
            return;
          }
          a.fest = true;
          a.ip = ip;
          a.maske = maske;
          a.gw = gw;
          a.dns = dns;
          String fehler;
          if (!starwatch::Testweb::adresse_pruefen(a, &fehler)) {
            starwatch::konsole.printf("Nicht gespeichert: %s\n", fehler.c_str());
            return;
          }
        }
        testweb.wlan_setzen(ssid, testweb.gespeichertes_passwort(), &a);
        return;
      }
      if (zweit == 's') {  // ns SSID PASSWORT
        char *arg = z + 2;
        while (*arg == ' ')
          arg++;
        char *trenn = strchr(arg, ' ');
        if (trenn == nullptr) {
          starwatch::konsole.println(
              "Format: ns <SSID> <PASSWORT>\n"
              "Enthaelt die SSID ein Leerzeichen, geht nur die Weboberflaeche.");
          return;
        }
        *trenn = '\0';
        testweb.wlan_setzen(String(arg), String(trenn + 1));
        return;
      }
      if (testweb.verbunden()) {
        starwatch::konsole.printf("Netz steht schon: http://%s/\n",
                                  testweb.adresse().c_str());
        return;
      }
      starwatch::konsole.println(
          "Starte WLAN von Hand. Bricht die Versorgung ein, startet das Geraet\n"
          "neu - dann ist es die Hardware, siehe die Brownout-Meldung.");
      testweb.beginn(&antrieb, &hall, &licht, &lux, &betrieb, kommando);
      return;
    }

    case 'r':
      antrieb.register_setzen(static_cast<uint16_t>(g::MIKROSCHRITTE));
      starwatch::konsole.printf("Registersatz neu geschrieben. Treiber antwortet: %s\n",
                    antrieb.treiber_antwortet() ? "ja" : "NEIN");
      return;

    case 'z': {
      // Eine Uhrzeit beginnt mit einer ZIFFER, eine Zeitzone mit einem
      // Buchstaben ("CET-1CEST,..."). Damit braucht die Zeitzone kein eigenes
      // Kommando und die Unterscheidung ist eindeutig.
      if (*rest != '\0' && !isdigit((unsigned char)*rest)) {
        betrieb.zeitzone(rest);
        ntp_gestartet = false;  // mit der neuen Zone neu anmelden
        ntp_gemeldet = false;
        ntp_starten();
        starwatch::konsole.printf("Zeitzone: %s\n", betrieb.zeitzone().c_str());
        return;
      }
      int hh = 0, mm = 0, ss = 0;
      if (sscanf(rest, "%d:%d:%d", &hh, &mm, &ss) >= 2) {
        betrieb.zeit_setzen(hh, mm, ss);
        betrieb.zeit_aus_netz(false);
        starwatch::konsole.printf("Zeit gestellt: %s (von Hand)\n",
                      betrieb.uhrzeit_text().c_str());
      } else {
        starwatch::konsole.printf(
            "z HH:MM:SS       Uhrzeit von Hand\n"
            "z <POSIX-Zone>   Zeitzone, z. B. z CET-1CEST,M3.5.0,M10.5.0/3\n"
            "Jetzt: Zone %s, Zeit %s\n",
            betrieb.zeitzone().c_str(),
            betrieb.zeit_aus_netz() ? "aus dem Netz" : "von Hand oder unbekannt");
      }
      return;
    }

    case 'm': {
      const int n = atoi(rest);
      if (n < 0 || n > 2) {
        starwatch::konsole.println("Modus: 0 aus, 1 fliessend, 2 Minutensprung");
        return;
      }
      if (n != 0 && !betrieb.zeit_gueltig()) {
        starwatch::konsole.println("Erst die Zeit stellen (z HH:MM:SS) - ohne Zeit gibt es\n"
                       "keine Sollposition, und der Zeiger faehrt nicht los.");
        return;
      }
      betrieb.zeitmodus(static_cast<starwatch::Zeitmodus>(n));
      // Was der Bediener hier waehlt, ist ab jetzt auch das Ziel des
      // Rueckfalls - und eine Sperre vom Einmessen ist damit aufgehoben.
      betrieb.rueckfall_modus(static_cast<starwatch::Zeitmodus>(n));
      betrieb.rueckfall_freigeben();
      betrieb.handlung_gesehen();
      betrieb.werte_sichern();
      starwatch::konsole.printf("Zeitmodus: %s\n", starwatch::zeitmodus_text(betrieb.zeitmodus()));
      return;
    }

    case 'p': {
      if (z[1] == 's') {
        // ps <n>: laufend nachgeschobenes Ziel (21.10.2026). Fuer das Steuern
        // von aussen - die App schickt es ueber eine Dauerverbindung auf dem
        // Log-Port mehrmals je Sekunde.
        //
        // SCHWEIGT IM ERFOLGSFALL: bei 50 Befehlen je Sekunde ist jede Zeile
        // Log eine Last fuer sich, und sie ginge ueber dieselbe Verbindung
        // zurueck, die die Befehle bringt.
        const char *arg = z + 2;
        while (*arg == ' ')
          arg++;
        // ABWEISEN, NICHT RATEN: `atol` macht aus jedem Unsinn eine 0, und das
        // waere ein Sprung auf zwoelf. Bei einem Strom aus dem Netz ist ein
        // verstuemmeltes Paket der Normalfall, nicht die Ausnahme.
        if (*arg != '-' && !isdigit((unsigned char)*arg)) {
          starwatch::konsole.printf(
              "ps <n>           Ziel laufend nachschieben, kuerzester Weg\n"
              "                 (auch rueckwaerts; ausserhalb 0..%ld wird gefaltet)\n",
              (long)g::REV - 1);
          return;
        }
        const bool uhr_aus = betrieb.steuere_auf(static_cast<int32_t>(atol(arg)));
        uhr_ging_aus(uhr_aus);
        return;
      }
      const long n = atol(rest);
      const bool uhr_aus = betrieb.fahre_auf(static_cast<int32_t>(n));
      starwatch::konsole.printf("Fahre auf Schritt %ld (vorwaerts, Ziel %ld).\n", n,
                    (long)antrieb.ziel());
      uhr_ging_aus(uhr_aus);
      return;
    }

    case 'u': {
      if (z[1] == 'z') {
        const bool uhr_aus = betrieb.auf_zwoelf_halten();
        starwatch::konsole.println("Fahre auf ZWOELF und halte dort, bis 'o!' bestaetigt.");
        uhr_ging_aus(uhr_aus);
        return;
      }
      int32_t ziel = 0;
      if (!zeit_zu_position(rest, &ziel)) {
        starwatch::konsole.println("u <stunde 0..12> oder u HH:MM[:SS]\n"
                       "Beispiele: u 12   u 3   u 4.5   u 15:00   u 21:30");
        return;
      }
      const int32_t kreise = betrieb.effekt_kreise();
      const bool uhr_aus = betrieb.fahre_auf(ziel, kreise);
      const int32_t weg = antrieb.ziel() - antrieb.position();
      starwatch::konsole.printf(
          "Fahre auf '%s' = Schritt %ld von %ld.\n", rest, (long)ziel, (long)g::REV);
      if (kreise > 0)
        starwatch::konsole.printf(
            "  mit %ld Schwungkreis%s: %ld Schritte, rund %.2f s.\n", (long)kreise,
            kreise == 1 ? "" : "en", (long)weg,
            (double)starwatch::fahrzeit_ms(weg, antrieb.beschleunigung(),
                                           betrieb.fahrtempo()) / 1000.0);
      uhr_ging_aus(uhr_aus);
      return;
    }

    case 'y': {
      const char zweit = z[1];
      if (zweit == '!') {  // y! = laufende Fassung von Hand bestaetigen
        // FUER DEN PRUEFSTAND. Ein per OTA eingespieltes Image bestaetigt
        // sich erst mit WLAN, Netzzeit UND erfolgreicher Referenzfahrt. Ohne
        // Hall-Sensor gelingt die nie - und beim naechsten Neustart faehrt
        // der Bootloader die vorige Fassung. Am 25.09.2026 am Geraet
        // genau so passiert: das Update war nach einem Stromzyklus weg.
        // Wie alle 'y'-Kommandos nur mit Token (kommando_braucht_token).
        if (!aktualisierung.auf_bewaehrung()) {
          starwatch::konsole.println("Nichts zu bestaetigen - diese Fassung ist nicht auf Bewaehrung.");
          return;
        }
        aktualisierung.bewaehrt_melden();
        starwatch::konsole.printf(
            aktualisierung.auf_bewaehrung()
                ? "Bestaetigen FEHLGESCHLAGEN - die Fassung bleibt auf Bewaehrung.\n"
                : "Fassung %s von Hand bestaetigt. Kein Rollback mehr - auch ohne\n"
                  "Referenzfahrt. Ob sie laeuft, hat damit DER BEDIENER entschieden.\n",
            g::FIRMWARE_VERSION);
        return;
      }
      if (zweit == 'p') {  // yp = jetzt nachsehen
        Langlauf sperre;
        starwatch::konsole.println("Sehe nach, ob es eine neuere Fassung gibt ...");
        const bool neu_da = aktualisierung.pruefen();
        starwatch::konsole.printf("%s\n", aktualisierung.meldung().c_str());
        if (neu_da)
          starwatch::konsole.println("Einspielen mit 'yi'. Die Uhr steht dabei und\n"
                         "braucht danach ein Homing.");
        return;
      }
      if (zweit == 'i') {  // yi = einspielen
        Langlauf sperre;
        starwatch::konsole.println("Lade und pruefe das Image. Das dauert.");
        const bool ok = aktualisierung.einspielen();
        starwatch::konsole.printf("%s\n", aktualisierung.meldung().c_str());
        if (ok) {
          starwatch::konsole.println("Neustart in 3 s.");
          delay(3000);
          ESP.restart();
        }
        return;
      }
      if (zweit == 'u') {  // yu <url>
        char *arg = z + 2;
        while (*arg == ' ')
          arg++;
        if (*arg == '\0') {
          starwatch::konsole.printf("Manifest-URL: %s\n", aktualisierung.url().c_str());
          return;
        }
        aktualisierung.url(arg);
        starwatch::konsole.printf("Manifest-URL: %s\n", aktualisierung.url().c_str());
        return;
      }
      if (*rest == 'k') {
        const char *arg = rest + 1;
        while (*arg == ' ')
          arg++;
        if (*arg == 's')
          aktualisierung.kanal(starwatch::Updatekanal::stabil);
        else if (*arg == 'b')
          aktualisierung.kanal(starwatch::Updatekanal::beta);
        else if (*arg != '\0') {
          starwatch::konsole.println("yk s | yk b   - stabil oder beta");
          return;
        }
        starwatch::konsole.printf("Kanal: %s\n%s\n",
                      starwatch::updatekanal_text(aktualisierung.kanal()),
                      aktualisierung.url().c_str());
        if (aktualisierung.kanal() == starwatch::Updatekanal::beta)
          starwatch::konsole.println(
              "BETA. Diese Uhr zieht Vorabfassungen. Fuer eine verschenkte Uhr\n"
              "ist das die falsche Einstellung - dort gilt 'yk s'.");
        return;
      }
      if (*rest >= '0' && *rest <= '2') {
        aktualisierung.modus(static_cast<starwatch::Updatemodus>(*rest - '0'));
        starwatch::konsole.printf("Updates: %s\n",
                      starwatch::updatemodus_text(aktualisierung.modus()));
        if (aktualisierung.modus() == starwatch::Updatemodus::automatisch)
          starwatch::konsole.printf(
              "Einmal taeglich gegen %d Uhr - nachts, weil die Uhr dabei STEHT\n"
              "und danach ein Homing braucht. Nie waehrend eines Timers.\n",
              g::UPDATE_STUNDE);
        return;
      }
      starwatch::konsole.printf(
          "Firmware %s, Updates %s, Kanal %s\n"
          "Partition: %s\n"
          "Stand: %s\n"
          "Manifest: %s\n\n"
          "  y 0|1|2    aus / manuell / automatisch (taeglich)\n"
          "  yk s|b     Kanal stabil oder beta\n"
          "  yp         jetzt nachsehen\n"
          "  yi         gefundene Fassung einspielen\n"
          "  yu <url>   Manifest-URL; ohne Wert nur anzeigen\n",
          g::FIRMWARE_VERSION, starwatch::updatemodus_text(aktualisierung.modus()),
          starwatch::updatekanal_text(aktualisierung.kanal()),
          aktualisierung.partition_text().c_str(),
          aktualisierung.meldung().empty() ? "noch nicht geprueft"
                                           : aktualisierung.meldung().c_str(),
          aktualisierung.url().c_str());
      return;
    }

    // ALLES EIN ODER AUS. Ein einziger Schalter, weil 'Klinge aus, aber Motor
    // laeuft weiter' kein Zustand ist, den jemand haben will. Die Reihenfolge
    // ist nicht beliebig:
    //   aus  erst anhalten (MIT RAMPE - hart abschneiden reisst dem Riemen
    //        einen Zahn), dann Licht, dann Endstufe. Andersherum stuende der
    //        Zeiger stromlos da, waehrend noch Schritte kommen.
    //   ein  erst Endstufe, dann Referenzfahrt - im Freilauf ist die Klinge
    //        aus ihrer Lage gekippt, die gezaehlte Position ist wertlos.
    //        Ohne die Fahrt zeigte die Uhr danach irgendwohin.
    case 'q': {
      if (*rest != '0' && *rest != '1') {
        starwatch::konsole.printf(
            "Alles ein oder aus. Jetzt: Klinge %s, Endstufe %s.\n"
            "  q 0   anhalten, Klinge aus, Endstufe aus (Freilauf)\n"
            "  q 1   Endstufe ein, Homing, Klinge an\n",
            licht.ist_an() ? "an" : "aus",
            antrieb.endstufe_an() ? "bestromt" : "aus");
        return;
      }
      if (*rest == '0') {
        antrieb.stopp_mit_rampe();
        licht.an(false);
        antrieb.endstufe(false);
        betrieb.werte_sichern();
        starwatch::konsole.println(
            "Alles aus. Die Klinge kippt jetzt aus ihrer Lage - beim\n"
            "Einschalten faehrt die Uhr deshalb erst die Referenz.");
        return;
      }
      antrieb.endstufe(true);
      licht.an(true);
      betrieb.werte_sichern();
      starwatch::konsole.println("Alles ein. Homing laeuft, 's' bricht ab.");
      abbruch_gewuenscht = false;
      {
        Langlauf sperre;
        const starwatch::Homingergebnis e =
            betrieb.referenzfahrt(pumpe, starwatch::Hominganlass::alles_ein);
        starwatch::konsole.printf("%s\n", starwatch::hominggrund_text(e.grund));
      }
      return;
    }

    // Effekte, die von SELBST laufen. Nicht die Ausloeser - die bleiben bei
    // 'lz' und 'ls'. Hier steht nur, OB die Uhr sie selbst startet.
    case 'j': {
      const char welcher = z[1];
      const char *w = rest + 1;
      while (*w == ' ')
        w++;
      if ((welcher == 'z' || welcher == 's') && (*w == '0' || *w == '1')) {
        const bool an = (*w == '1');
        if (welcher == 'z')
          betrieb.effekt_start(an);
        else
          betrieb.effekt_stunde(an);
        starwatch::konsole.printf("%s: %s\n",
            welcher == 'z' ? "Zuend- und Einfahreffekt" : "Stundeneffekt",
            an ? "ein" : "aus");
        return;
      }
      starwatch::konsole.printf(
          "Selbstlaufende Effekte\n"
          "  Zuend- und Einfahreffekt : %s   (jz 0|1) - Einschalten, Farbwechsel, Nacht\n"
          "  Stundeneffekt            : %s   (js 0|1)\n",
          betrieb.effekt_start() ? "ein" : "aus",
          betrieb.effekt_stunde() ? "ein" : "aus");
      return;
    }

    case 'b': {
      if (strcmp(rest, "werksreset") == 0) {
        starwatch::konsole.println(
            "WERKSRESET. Benutzerwerte und WLAN werden geloescht.\n"
            "Der Hall-Offset BLEIBT - er entsteht bei der Montage und ist\n"
            "fuer jede Uhr anders (AP 3.1). Neustart in 2 s.");
        betrieb.werksreset();
        testweb.wlan_setzen("", "");  // wie 'nx'
        delay(2000);
        ESP.restart();
        return;
      }
      if (*rest != '\0') {
        starwatch::konsole.println("b            Neustart\n"
                       "b werksreset Benutzerwerte + WLAN loeschen, Offset behalten");
        return;
      }
      starwatch::konsole.println("Neustart in 2 s.");
      delay(2000);
      ESP.restart();
      return;
    }

    case 'k': {
      if (*rest == '\0') {
        const int32_t n = betrieb.effekt_kreise();
        starwatch::konsole.printf(
            "Schwungkreise %ld%s\n"
            "Setzen mit 'k <n>', 0 schaltet ab. Hoechstens %ld.\n",
            (long)n, n == 0 ? " (aus)" : "", (long)g::EFFEKT_KREISE_MAX);
        return;
      }
      char *ende = nullptr;
      const long n = strtol(rest, &ende, 10);
      if (ende == rest || n < 0) {
        starwatch::konsole.println("k <n> - Schwungkreise vor dem Ziel, 0 = aus.");
        return;
      }
      betrieb.effekt_kreise(static_cast<int32_t>(n));
      betrieb.werte_sichern();
      const int32_t k = betrieb.effekt_kreise();
      if (k != n)
        starwatch::konsole.printf("Auf %ld begrenzt (EFFEKT_KREISE_MAX).\n", (long)k);
      // Was das bedeutet, gleich mitsagen: der schlimmste Fall ist ein Ziel
      // eine ganze Umdrehung entfernt.
      starwatch::konsole.printf(
          "Schwungkreise %ld. Ein Sprung dauert damit %.2f bis %.2f s\n"
          "(%ld bis %ld Schritte bei %lu Schritten/s, Rampe %lu).\n",
          (long)k,
          (double)starwatch::fahrzeit_ms(k * g::REV + 1, antrieb.beschleunigung(),
                                         betrieb.fahrtempo()) / 1000.0,
          (double)starwatch::fahrzeit_ms((k + 1) * g::REV, antrieb.beschleunigung(),
                                         betrieb.fahrtempo()) / 1000.0,
          (long)(k * g::REV), (long)((k + 1) * g::REV),
          (unsigned long)betrieb.fahrtempo(),
          (unsigned long)antrieb.beschleunigung());
      return;
    }

    case 'x': {
      // x <n> <zeit1> <zeit2> - n mal hin und her.
      char eins[24] = {0}, zwei[24] = {0};
      long n = 0;
      if (sscanf(rest, "%ld %23s %23s", &n, eins, zwei) != 3 || n <= 0) {
        starwatch::konsole.println(
            "x <n> <zeit1> <zeit2>  - n mal zwischen zwei Stellungen pendeln\n"
            "Beispiele: x 10 12:00 18:00     x 5 12 6     x 20 21:00 3:00");
        return;
      }
      int32_t a = 0, b = 0;
      if (!zeit_zu_position(eins, &a) || !zeit_zu_position(zwei, &b)) {
        starwatch::konsole.println("Zeitangabe unverstaendlich. Stunde 0..12 oder HH:MM[:SS].");
        return;
      }
      if (a == b) {
        starwatch::konsole.println("Beide Stellungen sind dieselbe - da pendelt nichts.");
        return;
      }
      pendellauf(n, a, b);
      return;
    }

    case 'v': {
      const long n = atol(rest);
      const bool uhr_aus = betrieb.verstelle_um(static_cast<int32_t>(n));
      starwatch::konsole.printf("Verstelle um %ld Schritte.\n", n);
      uhr_ging_aus(uhr_aus);
      return;
    }

    case 'f': {
      const long hz = atol(rest);
      if (hz <= 0) {
        starwatch::konsole.println("Tempo in Schritten/s, groesser 0.");
        return;
      }
      if (static_cast<uint32_t>(hz) > g::TEMPO_MAX) {
        starwatch::konsole.printf("Hoechstens %lu Schritte/s (%.1f U/s).\n",
                      (unsigned long)g::TEMPO_MAX,
                      (double)g::TEMPO_MAX / g::REV);
        return;
      }
      // BEIDES setzen: der Antrieb fuer die naechste Fahrt, und das Fahrtempo
      // in Betrieb - sonst ueberschreibt der naechste Sprung den Wert wieder.
      betrieb.fahrtempo(static_cast<uint32_t>(hz));
      antrieb.tempo(static_cast<uint32_t>(hz));
      betrieb.werte_sichern();
      starwatch::konsole.printf(
          "Tempo %ld Schritte/s = %.2f U/s. Eine Umdrehung in %.2f s.\n", hz,
          (double)hz / g::REV,
          (double)starwatch::fahrzeit_ms(g::REV, antrieb.beschleunigung(),
                                         static_cast<uint32_t>(hz)) / 1000.0);
      return;
    }

    case 'a': {
      // NAME DER UHR. Vor der Rampe pruefen: 'alias ...' begaenne sonst mit
      // 'a' und waere eine Rampe von 0.
      if (strncmp(z, "alias", 5) == 0 && (z[5] == ' ' || z[5] == '\0')) {
        const char *name = z + 5;
        while (*name == ' ')
          name++;
        if (*name == '\0') {
          starwatch::konsole.printf(
              "Name der Uhr: %s\n"
              "  alias <name>   setzen, hoechstens %u Zeichen\n"
              "  alias -        loeschen, dann gilt wieder %s\n",
              betrieb.alias().empty() ? "(keiner)" : betrieb.alias().c_str(),
              (unsigned)starwatch::ALIAS_MAX_ZEICHEN, betrieb.geraetename().c_str());
          return;
        }
        const bool loeschen = (strcmp(name, "-") == 0);
        if (!betrieb.alias(loeschen ? std::string() : std::string(name))) {
          starwatch::konsole.printf(
              "Name abgewiesen. Hoechstens %u Zeichen, keine Steuerzeichen,\n"
              "kein \" \\ < >.\n", (unsigned)starwatch::ALIAS_MAX_ZEICHEN);
          return;
        }
        starwatch::konsole.printf("Name der Uhr: %s\n", betrieb.anzeigename().c_str());
        return;
      }
      const long n = atol(rest);
      if (n <= 0) {
        starwatch::konsole.printf(
            "Rampe %lu Schritte/s^2. Aus dem Stand auf %.1f U/s in %.2f s.\n"
            "Eine Umdrehung geht damit in mindestens %.2f s.\n"
            "Setzen mit 'a <n>'.\n",
            (unsigned long)antrieb.beschleunigung(),
            (double)g::TEMPO_FAHRT / g::REV,
            (double)g::TEMPO_FAHRT / antrieb.beschleunigung(),
            (double)starwatch::timer_mindestdauer_ms(
                g::REV, antrieb.beschleunigung(), g::TEMPO_FAHRT) / 1000.0);
        return;
      }
      antrieb.rampe(static_cast<uint32_t>(n), static_cast<uint32_t>(n));
      betrieb.werte_sichern();
      starwatch::konsole.printf(
          "Rampe %ld Schritte/s^2. Aus dem Stand auf %.1f U/s in %.2f s,\n"
          "eine Umdrehung in mindestens %.2f s.\n",
          n, (double)g::TEMPO_FAHRT / g::REV, (double)g::TEMPO_FAHRT / n,
          (double)starwatch::timer_mindestdauer_ms(
              g::REV, static_cast<uint32_t>(n), g::TEMPO_FAHRT) / 1000.0);
      return;
    }

    case 'h': {
      if (z[1] == 'f') {  // hf [fein] [grob] - Tempo der Referenzfahrt messen
        long fein = 0, grob = 0;
        const int n = sscanf(z + 2, "%ld %ld", &fein, &grob);
        if (n >= 1)
          hall.tempi(static_cast<uint32_t>(fein),
                     n >= 2 ? static_cast<uint32_t>(grob) : hall.grob_tempo());
        starwatch::konsole.printf(
            "Referenzfahrt: Feinfahrt %lu Schritte/s, Grobsuche %lu Schritte/s\n"
            "(nur bis zum Neustart; 'hf <fein> [grob]', fein 10..800).\n",
            (unsigned long)hall.fein_tempo(), (unsigned long)hall.grob_tempo());
        return;
      }
      if (z[1] == 'v') {  // hv - Homing-Verlauf (05.10.2026)
        verlauf_zeigen();
        return;
      }
      abbruch_gewuenscht = false;
      starwatch::konsole.println("\nHoming. 's' bricht ab.");
      const uint32_t homing_ab = millis();
      Langlauf sperre;
      const starwatch::Homingergebnis e =
          betrieb.referenzfahrt(pumpe, starwatch::Hominganlass::hand);
      starwatch::konsole.printf("%s\n", starwatch::hominggrund_text(e.grund));
      // hu: danach SOFORT auf die Uhrzeit statt zehn Sekunden auf zwoelf -
      // das Ende des Steuerns in der App (27.10.2026).
      if (e.ok() && z[1] == 'u')
        betrieb.rueckfall_sofort();
      if (e.ok()) {
        starwatch::konsole.printf("  Messpunkte %u, Schwelle %.2f mT (%.0f %% des Gipfels)\n",
                      (unsigned)e.punkte, (double)(e.spanne_mt * g::HALL_SCHWELLE_ANTEIL),
                      (double)(g::HALL_SCHWELLE_ANTEIL * 100.0f));
        starwatch::konsole.printf("  Schwerpunkt %.2f, Flanke bei %ld, Versatz %.2f\n",
                      (double)e.schwerpunkt, (long)e.pos_flanke,
                      (double)(e.schwerpunkt - e.pos_flanke));
        // Die Abweichung rechnet Betrieb::referenzfahrt() - dort steht sie
        // seit 05.10.2026 auch im Homing-Verlauf ('hv').
        const int32_t abw = betrieb.letzte_abweichung();
        starwatch::konsole.printf(
            "  Abweichung zur letzten Referenz %ld Schritte (%.1f min auf dem Blatt),"
            " Dauer %.1f s%s\n",
            (long)abw, (double)(abw * g::SEK_JE_SCHRITT / 60.0f),
            (double)((millis() - homing_ab) / 1000.0f),
            betrieb.letzte_vergleichbar() ? "" : " - NICHT VERGLEICHBAR (keine Referenz davor)");
        verlust_melden("  ");
        // DIESE ZEILE IST DIE MESSVORSCHRIFT FUER DIE SCHWELLEN in geraet.h.
        // Sie stehen dort als gesetzte Startwerte, nicht als gemessene: was
        // hier als Spanne herauskommt, ist die tatsaechliche Gipfelhoehe des
        // verbauten Magneten in Millitesla.
        starwatch::konsole.printf("  Ruhelinie %.3f mT, Spanne %.2f mT %s\n", (double)e.ruhe_mt,
                      (double)e.spanne_mt,
                      (e.spanne_mt < g::HALL_SPANNE_MIN_MT)
                          ? "-> ZU WENIG, Magnetabstand verringern"
                          : ((e.spanne_mt > g::HALL_SPANNE_MAX_MT)
                                 ? "-> ZU VIEL, Kurve klippt, Abstand vergroessern"
                                 : "-> im Fenster, passt"));
        starwatch::konsole.printf("  (HALL_SPANNE_MIN_MT/MAX_MT in geraet.h stehen auf "
                      "%.1f / %.1f mT und sind noch GESETZT, nicht gemessen)\n",
                      (double)g::HALL_SPANNE_MIN_MT, (double)g::HALL_SPANNE_MAX_MT);
        // Beides aus der Feinfahrt (27.09.2026). Die Fahrt endet an der Kurve;
        // steht "bis" knapp ueber +150, war der Magnet schmal, und die
        // Mindeststrecke hat gegriffen - das ist gewollt.
        starwatch::konsole.printf(
            "  Feldbreite %ld Schritte (ueber %.2f mT), Feinfahrt %ld..%+ld um die Mitte,"
            " Tempo %lu/%lu\n",
            (long)e.fenster, (double)g::HALL_GROB_SCHWELLE_MT, (long)e.fein_von,
            (long)e.fein_bis, (unsigned long)hall.fein_tempo(),
            (unsigned long)hall.grob_tempo());
        starwatch::konsole.printf("  Offset %ld + Nachlauf %ld -> Zaehler %ld\n",
                      (long)betrieb.hall_offset(), (long)e.nachlauf, (long)e.zaehler);
        starwatch::konsole.println(
            "  Der Zeiger faehrt jetzt auf ZWOELF. Zeigt er dort nicht wirklich\n"
            "  auf zwoelf: mit 'v' nachstellen, dann 'o!' - und die Fahrt\n"
            "  wiederholen. Sitzt sie zweimal, ist der Offset eingemessen.");
      }
      return;
    }

    case 'o': {
      if (*rest == '!' || z[1] == '!') {
        const bool uhr_aus = betrieb.hier_ist_zwoelf();
        // Gespeichert - die Klinge blinkt dreimal gruen, auch wenn sie aus ist.
        licht.signal(starwatch::Licht::Ebene::zustand, 0x00FF00, starwatch::Muster::blinken,
                     -1.0f, 3000);
        starwatch::konsole.printf(
            "Gemerkt: der Zeiger steht auf ZWOELF. Offset %ld, in NVS gespeichert.\n",
            (long)betrieb.hall_offset());
        (void)uhr_aus;  // sie kommt gleich zurueck - siehe hier_ist_zwoelf()
        starwatch::konsole.println(
            "Zurueck zur Uhrzeit. Zur Kontrolle 'h' - landet der Zeiger auf zwoelf,\n"
            "ist es eingemessen.");
        return;
      }

      // GEPRUEFT, nicht atol: atol macht aus jeder Eingabe ohne Zahl
      // stillschweigend eine 0 - und eine 0 hier ueberschreibt den
      // eingemessenen Offset im NVS. In der Weboberflaeche stand das Feld
      // ausserdem fest auf 0, sodass ein Klick auf "setzen" den Wert
      // geloescht hat. Beides am 23.08.2026 abgestellt.
      if (*rest == '\0') {
        starwatch::konsole.printf(
            "Kein Wert. Der Offset steht auf %ld und bleibt es.\n"
            "Einmessen geht mit 'o!' - eine Zahl braucht es nur zum\n"
            "Zurueckschreiben eines notierten Werts: o <0..%ld>\n",
            (long)betrieb.hall_offset(), (long)g::REV - 1);
        return;
      }
      char *ende = nullptr;
      const long n = strtol(rest, &ende, 10);
      while (ende != nullptr && *ende == ' ')
        ende++;
      if (ende == rest || (ende != nullptr && *ende != '\0')) {
        starwatch::konsole.printf("'%s' ist keine Zahl. Offset bleibt %ld.\n", rest,
                      (long)betrieb.hall_offset());
        return;
      }
      if (n < 0 || n >= g::REV) {
        starwatch::konsole.printf("Offset muss 0 bis %ld sein. Er bleibt %ld.\n",
                      (long)g::REV - 1, (long)betrieb.hall_offset());
        return;
      }
      betrieb.hall_offset(static_cast<int32_t>(n));
      starwatch::konsole.printf("Hall-Offset %ld (in NVS gespeichert).\n",
                    (long)betrieb.hall_offset());
      return;
    }

    case 't': {
      // LAUFART. Vor der Dauer pruefen: 'tl' waere sonst "Timer 0 s".
      if (z[1] == 'l') {
        const char *w = rest + 1;
        while (*w == ' ')
          w++;
        if (*w == '0' || *w == '1') {
          betrieb.timerlauf(*w == '1' ? starwatch::Timerlauf::sekundentakt
                                      : starwatch::Timerlauf::gleitend);
          starwatch::konsole.printf("Timer laeuft %s.\n",
              starwatch::timerlauf_text(betrieb.timerlauf()));
          return;
        }
        starwatch::konsole.printf(
            "Timer-Laufart: %s\n"
            "  tl 0   gleitend - ein Zug, gleichmaessig\n"
            "  tl 1   Sekundentakt - steht, springt jede Sekunde\n",
            starwatch::timerlauf_text(betrieb.timerlauf()));
        return;
      }
      if (*rest == '-' || z[1] == '-') {
        betrieb.timer_abbrechen();
        starwatch::konsole.println("Timer abgebrochen.");
        return;
      }
      const long sek = atol(rest);
      if (sek <= 0) {
        starwatch::konsole.println("Timer in Sekunden, groesser 0. Beispiel: t 300");
        return;
      }
      // GERECHNET, nicht geraten: eine Umdrehung braucht Rampe und Tempo.
      // Vorher nahm die Firmware 't 1' widerspruchslos an und brauchte dann
      // ein Vielfaches - die Zusage war nicht zu halten.
      // Mit dem Tempo rechnen, das der TIMER benutzt (g::TEMPO_FAHRT), nicht
      // mit der Obergrenze der Kommandos - und mit der Rampe, die gerade
      // wirklich gilt.
      const uint32_t mindestens_ms = starwatch::timer_mindestdauer_ms(
          g::REV, antrieb.beschleunigung(), g::TEMPO_FAHRT);
      if (static_cast<uint32_t>(sek) * 1000u < mindestens_ms) {
        starwatch::konsole.printf(
            "Zu kurz. Eine Umdrehung braucht mindestens %.2f s - mit\n"
            "%lu Schritten/s Fahrtempo und %lu Schritten/s^2 Rampe.\n"
            "Mehr Rampe geht mit 'a <n>'.\n",
            (double)mindestens_ms / 1000.0, (unsigned long)g::TEMPO_FAHRT,
            (unsigned long)antrieb.beschleunigung());
        return;
      }
      // Unixzeit mitgeben, damit das ABSOLUTE Ende gemerkt wird und einen
      // Neustart uebersteht. Ohne Netzzeit ist sie 0 - dann laeuft der Timer
      // wie bisher nur im RAM.
      const uint32_t jetzt_epoch =
          betrieb.zeit_aus_netz() ? (uint32_t)time(nullptr) : 0u;
      if (betrieb.timer_starten(static_cast<uint32_t>(sek), jetzt_epoch)) {
        starwatch::konsole.printf("Timer %ld s. Faehrt erst auf zwoelf, dann eine Umdrehung.\n"
                      "Der Uhrbetrieb ist dafuer abgeschaltet.\n", sek);
      }
      return;
    }

    case 'w': {
      const float p = atof(rest);
      licht.pwm_direkt(p);
      starwatch::konsole.printf(
          "PWM direkt %.1f %% (ohne Kennlinie), LED %.2f W von hoechstens %.2f W.\n",
          (double)p, (double)licht.leistung_w(), (double)g::LED_MAX_W);
      return;
    }

    case 'e': {
      const bool ein = atoi(rest) != 0;
      antrieb.endstufe(ein);
      starwatch::konsole.printf("Endstufe %s.%s\n", ein ? "ein" : "AUS",
                    ein ? "" : " Freilauf - die Klinge kippt aus ihrer Lage.");
      return;
    }

    case 'l': {
      const char zweit = z[1];
      // Das Argument der ZWEIBUCHSTABIGEN Kommandos (lu, lf). `rest` beginnt
      // hinter dem ERSTEN Buchstaben und traegt den zweiten noch mit - aus
      // "lu 1" wird dort "u 1". Deshalb lief 'lu 0' / 'lu 1' bisher auf das
      // Umschalten hinaus statt auf den festen Wert (gefunden 25.09.2026).
      const char *arg2 = (zweit != '\0') ? z + 2 : z + 1;
      while (*arg2 == ' ')
        arg2++;
      if (zweit == 'a') {
        licht.an(true);
        betrieb.werte_sichern();
        starwatch::konsole.printf("Klinge an, %.0f %%.\n", (double)licht.soll_prozent());
      } else if (zweit == 'o') {
        licht.an(false);
        betrieb.werte_sichern();
        starwatch::konsole.println("Klinge aus.");
      } else if (zweit == 'u') {
        // Lichtautomatik (27.09.2026: gespeichert, alle Werte einstellbar).
        //   lu            umschalten        lu 0 / lu 1   aus / an
        //   lu n [0|1]    Nachtabschaltung
        //   lu min <%>    kleinste Helligkeit, Anteil des Reglers
        //   lu hell <lx>  ab hier voller Reglerwert
        //   lu aus <lx>   Nacht: darunter aus (an beim Sechsfachen)
        // Ungueltige Zahlen werden NICHT uebernommen - atof machte aus "abc"
        // eine 0, und die Grenze machte daraus still das Minimum.
        starwatch::Lichtautomatik a = betrieb.lichtautomatik();
        const char *arg = arg2;
        char wort[8] = {0};
        float zahl = 0.0f;
        const int n = sscanf(arg, "%7s %f", wort, &zahl);
        bool ok = true;
        if (n <= 0)
          a.an = !a.an;
        else if (strcmp(wort, "0") == 0 || strcmp(wort, "1") == 0)
          a.an = wort[0] == '1';
        else if (strcmp(wort, "n") == 0)
          a.nacht = (n == 2) ? (zahl != 0.0f) : !a.nacht;
        else if (strcmp(wort, "min") == 0 && n == 2)
          a.anteil_min = zahl / 100.0f;
        else if (strcmp(wort, "hell") == 0 && n == 2)
          a.lux_hell = zahl;
        else if (strcmp(wort, "aus") == 0 && n == 2)
          a.nacht_aus = zahl;
        else
          ok = false;
        if (!ok) {
          starwatch::konsole.println("lu [0|1] | lu n [0|1] | lu min <%> | lu hell <lx> | lu aus <lx>");
          return;
        }
        betrieb.lichtautomatik_setzen(a);
        const starwatch::Lichtautomatik &b = betrieb.lichtautomatik();
        starwatch::konsole.printf(
            "Lichtautomatik %s, Nachtabschaltung %s.\n"
            "  kleinste Helligkeit %.0f %% des Reglers unter %.1f lx, voll ab %.0f lx\n"
            "  Nacht: aus unter %.1f lx, an ueber %.1f lx (je %lu s Frist)\n",
            b.an ? "AN" : "AUS", b.nacht ? "AN" : "AUS", (double)(b.anteil_min * 100.0f),
            (double)g::LUX_DUNKEL, (double)b.lux_hell, (double)b.nacht_aus,
            (double)starwatch::nacht_an_lux(b), (unsigned long)(g::LUX_NACHT_VERZUG_MS / 1000));
        if (!lux.bereit())
          starwatch::konsole.printf("  Aber: %s - ohne Sensor wirkt sie nicht.\n",
                                    starwatch::luxgrund_text(lux.grund()));
        else if (lux.gemessen())
          starwatch::konsole.printf("  Jetzt %.1f lx.\n", (double)lux.lux());
      } else if (zweit == 'm') {
        // MELDUNG: die Klinge leuchtet in einer Farbe, bis quittiert wird oder
        // die Dauer abgelaufen ist. Gedacht fuer Home Assistant ("Pflanzen
        // giessen" = gelb), geht aber genauso von der Konsole.
        //   lm                          Zustand
        //   lm gelb                     gelb, ruhig, bis 'lm-'
        //   lm #00ff00 pulsieren 600 40 gruen, pulsierend, 10 min, 40 %
        //   lm-   oder   lm aus         quittieren
        const char *arg = arg2;
        using E = starwatch::Licht::Ebene;
        if (*arg == '-' || strcmp(arg, "aus") == 0) {
          const bool war = licht.signal_zustand(E::meldung).aktiv;
          licht.signal_aus(E::meldung);
          starwatch::konsole.println(war ? "Meldung quittiert." : "Keine Meldung aktiv.");
          return;
        }
        if (*arg == '\0') {
          const auto s = licht.signal_zustand(E::meldung);
          if (!s.aktiv)
            starwatch::konsole.println("Keine Meldung aktiv.");
          else if (s.rest_ms == 0)
            starwatch::konsole.printf("Meldung #%06lX, %s, %.0f %%, bis quittiert ('lm-').\n",
                                      (unsigned long)s.farbe, starwatch::muster_text(s.muster),
                                      (double)s.hell);
          else
            starwatch::konsole.printf("Meldung #%06lX, %s, %.0f %%, noch %lu s.\n",
                                      (unsigned long)s.farbe, starwatch::muster_text(s.muster),
                                      (double)s.hell, (unsigned long)(s.rest_ms / 1000u));
          return;
        }
        starwatch::Meldung m;
        if (!starwatch::meldung_lesen(arg, &m)) {
          starwatch::konsole.println(
              "Keine Meldung. 'lm <farbe> [leuchten|pulsieren|blinken] [dauer_s] [hell_%]'\n"
              "Farbe als Name (rot gruen blau gelb violett weiss orange tuerkis) oder Hex.");
          return;
        }
        licht.signal(E::meldung, m.farbe, m.muster, m.hell, m.dauer_s * 1000u);
        const auto s = licht.signal_zustand(E::meldung);
        starwatch::konsole.printf("Meldung #%06lX, %s, %.0f %%, %s.\n", (unsigned long)s.farbe,
                                  starwatch::muster_text(s.muster), (double)s.hell,
                                  m.dauer_s == 0 ? "bis quittiert ('lm-')" : "mit Ablauf");
      } else if (zweit == 'f') {
        // Farbe der Klinge. 'lf' zeigt sie, 'lf ff8800' oder 'lf 255 136 0'
        // setzt sie - MIT Zuendung, die von der alten zur neuen Farbe
        // gleitet. 'lfr <farbe>' setzt ohne: das schicken die Farbregler,
        // die beim Ziehen alle 250 ms einen Wert senden.
        const char *arg = arg2;
        bool mit_zuendung = true;
        if (arg[0] == 'r' && arg[1] == ' ') {
          mit_zuendung = false;
          arg += 2;
          while (*arg == ' ')
            arg++;
        }
        if (*arg == '\0') {
          starwatch::konsole.printf("Farbe #%06lX.\n", (unsigned long)licht.farbe());
          return;
        }
        uint32_t f = 0;
        if (!starwatch::farbe_lesen(arg, &f, betrieb.weiss())) {
          starwatch::konsole.println("Keine Farbe. 'lf ff8800' oder 'lf 255 136 0'.");
          return;
        }
        licht.farbe(f, mit_zuendung);
        betrieb.werte_sichern();
        float k[3];
        const float w = starwatch::kanal_anteile(
            starwatch::tastverhaeltnis(licht.soll_prozent(), licht.min_prozent()), f,
            g::LED_WATT_VOLL, g::LED_MAX_W, k);
        starwatch::konsole.printf(
            "Farbe #%06lX. Bei %.0f %% Regler: R %.1f %%  G %.1f %%  B %.1f %% "
            "Tastverhaeltnis, %.2f W (Grenze %.2f W).\n",
            (unsigned long)f, (double)licht.soll_prozent(), (double)(k[0] * 100.0f),
            (double)(k[1] * 100.0f), (double)(k[2] * 100.0f), (double)w,
            (double)g::LED_MAX_W);
      } else if (zweit == 'g' || zweit == 'p') {
        // UEBERGANG UND PULS (0.20.0). Die Woerter nach dem ersten Argument:
        //   a / o / z   an / aus / an mit Zuendung (nur lg)
        //   h <p>       Helligkeit in Prozent
        //   f <farbe>   Farbe wie bei 'lf'
        const char *arg = arg2;
        if (zweit == 'p' && (*arg == '-' || strncmp(arg, "aus", 3) == 0)) {
          const char *z2 = (*arg == '-') ? arg + 1 : arg + 3;
          const long ms = (*z2 != '\0') ? atol(z2) : 1000;
          const bool lief = licht.puls_aktiv();
          licht.puls_beenden(ms < 0 ? 0 : static_cast<uint32_t>(ms));
          betrieb.werte_sichern();
          starwatch::konsole.printf(lief ? "Puls beendet, %ld ms zurueck.\n"
                                         : "Kein Puls - trotzdem %ld ms zurueck.\n", ms);
          return;
        }
        if (zweit == 'p' && (strncmp(arg, "periode", 7) == 0 || strncmp(arg, "min", 3) == 0)) {
          const bool per = (arg[0] == 'p');
          const float zahl = atof(arg + (per ? 7 : 3));
          if (!(zahl > 0.0f)) {
            starwatch::konsole.println("lp periode <400..4000 ms> | lp min <1..50 %>");
            return;
          }
          if (per)
            betrieb.puls_setzen(static_cast<uint32_t>(zahl), licht.puls_min());
          else
            betrieb.puls_setzen(licht.puls_periode(), zahl / 100.0f);
          starwatch::konsole.printf("Puls: Periode %lu ms, Minimum %.0f %%.\n",
                                    (unsigned long)licht.puls_periode(),
                                    (double)(licht.puls_min() * 100.0f));
          return;
        }
        // Woerter lesen. Die Farbe darf Leerzeichen enthalten ('255 0 0'),
        // deshalb steht sie IMMER zuletzt und nimmt den ganzen Rest.
        long ms = -1;
        int an = -1;
        bool zuend = false;
        float hell = -1.0f;
        bool farbe_da = false;
        uint32_t farbe = 0;
        const char *p = arg;
        if (zweit == 'g') {
          char *ende = nullptr;
          ms = strtol(p, &ende, 10);
          if (ende == p || ms < 0) {
            starwatch::konsole.println("lg <ms> [a|o|z] [h <p>] [f <farbe>]");
            return;
          }
          p = ende;
        }
        while (*p != '\0') {
          while (*p == ' ')
            p++;
          if (*p == '\0')
            break;
          if ((p[0] == 'a' || p[0] == 'o' || p[0] == 'z') && (p[1] == ' ' || p[1] == '\0')) {
            an = (p[0] == 'o') ? 0 : 1;
            zuend = (p[0] == 'z');
            p++;
          } else if (p[0] == 'h' && p[1] == ' ') {
            char *ende = nullptr;
            hell = strtof(p + 2, &ende);
            p = ende;
          } else if (p[0] == 'f' && p[1] == ' ') {
            farbe_da = starwatch::farbe_lesen(p + 2, &farbe, betrieb.weiss());
            if (!farbe_da) {
              starwatch::konsole.println("Keine Farbe. 'f ff8800' oder 'f 255 136 0'.");
              return;
            }
            break;
          } else {
            starwatch::konsole.printf("Unbekannt: '%s'.\n", p);
            return;
          }
        }
        if (zweit == 'p') {
          licht.puls_starten(farbe_da, farbe, hell);
          starwatch::konsole.printf("Puls #%06lX bis %.0f %%, Minimum %.0f %%, Periode %lu ms.\n",
                                    (unsigned long)licht.puls_farbe(), (double)licht.puls_max(),
                                    (double)(licht.puls_min() * 100.0f),
                                    (unsigned long)licht.puls_periode());
          return;
        }
        if (zuend) {
          // Einschalten mit Zuendung: Werte vorher still setzen, die Zuendung
          // nimmt sie mit. Nur wenn die Klinge aus ist - sonst weich.
          starwatch::Licht::Weich w;
          w.hell = hell;
          w.farbe_da = farbe_da;
          w.farbe = farbe;
          if (!licht.ist_an() && !licht.puls_aktiv()) {
            licht.weich_setzen(w, 0);
            licht.zuenden();
          } else {
            w.an = 1;
            licht.weich_setzen(w, 500);
          }
        } else {
          starwatch::Licht::Weich w;
          w.an = an;
          w.hell = hell;
          w.farbe_da = farbe_da;
          w.farbe = farbe;
          licht.weich_setzen(w, static_cast<uint32_t>(ms));
        }
        betrieb.werte_sichern();
        starwatch::konsole.printf("Klinge %s, %.0f %%, #%06lX in %ld ms.\n",
                                  licht.ist_an() ? "an" : "aus", (double)licht.soll_prozent(),
                                  (unsigned long)licht.farbe(), ms < 0 ? 0L : ms);
      } else if (zweit == 'b') {
        const int n = atoi(arg2);
        licht.blitzen(n > 0 ? n : 1);
        starwatch::konsole.printf("%d Blitz(e).\n", n > 0 ? n : 1);
      } else if (zweit == 'z') {
        licht.zuenden();
        starwatch::konsole.println("Zuende die Roehre.");
      } else if (zweit == 'l') {
        licht.loeschen();
        starwatch::konsole.println("Loesche.");
      } else if (zweit == 's') {
        licht.stundeneffekt();
        starwatch::konsole.println("Stundeneffekt: 60 s auf 100 %.");
      } else if (zweit == 'r') {
        licht.rampe();
        starwatch::konsole.println("Rampe 0-20 % in halben Prozent, 700 ms je Stufe (28 s).\n"
                       "Gesucht: ab welchem Wert glimmt sie? -> Messprotokoll.");
      } else if (zweit == 'w') {
        // lw [farbe] - Weiss DIESER Uhr (27.10.2026), gespeichert im
        // Fertigungsbereich. Gilt fuer 'lf weiss', den Weiss-Knopf und die App.
        // Leuchtet die Klinge gerade weiss, bekommt sie das neue gleich mit.
        const char *arg = arg2;
        if (*arg != '\0') {
          uint32_t f = 0;
          if (!starwatch::farbe_lesen(arg, &f, betrieb.weiss())) {
            starwatch::konsole.println("Keine Farbe. 'lw 64ff96' oder 'lw 100 255 150'.");
            return;
          }
          const bool war_weiss = licht.farbe() == betrieb.weiss();
          betrieb.weiss(f);
          if (war_weiss) {
            licht.farbe(f);
            betrieb.werte_sichern();
          }
        }
        starwatch::konsole.printf("Weiss dieser Uhr #%06lX.\n", (unsigned long)betrieb.weiss());
      } else if (zweit == 't') {
        licht.treppe();
        starwatch::konsole.println("Treppe 0-100 % in Zehnerschritten, 3 s je Stufe (33 s).\n"
                       "Gesucht: laeuft die Helligkeit gleichmaessig?");
      } else {
        const float p = atof(rest);
        licht.soll_prozent(p);
        if (p > 0.0f)
          licht.an(true);
        // Nur die dauerhaften Groessen sichern - Zuenden, Loeschen und die
        // Effekte sind Vorgaenge, kein Zustand.
        betrieb.werte_sichern();
        starwatch::konsole.printf("Helligkeit %.0f %% -> Tastverhaeltnis %.3f\n", (double)p,
                      (double)starwatch::tastverhaeltnis(p, licht.min_prozent()));
      }
      return;
    }

    default:
      starwatch::konsole.printf("Unbekannt: '%c'. '?' zeigt die Kommandos.\n", k);
      return;
  }
}

void konsole_lesen() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (eingabe_len > 0) {
        eingabe[eingabe_len] = '\0';
        starwatch::konsole.println();
        if (eingabe_zu_lang)
          starwatch::konsole.printf(
              "Zu lang (hoechstens %u Zeichen). NICHT ausgefuehrt - ein\n"
              "gekuerztes Kommando tut etwas anderes als verlangt.\n",
              (unsigned)g::KOMMANDO_MAX - 1);
        else
          kommando(eingabe);
        eingabe_len = 0;
        eingabe_zu_lang = false;
        starwatch::konsole.print("> ");
      }
      continue;
    }
    if (c == 8 || c == 127) {  // Rueckschritt
      if (eingabe_len > 0) {
        eingabe_len--;
        starwatch::konsole.print("\b \b");
      }
      continue;
    }
    if (eingabe_len >= sizeof(eingabe) - 1) {
      eingabe_zu_lang = true;  // weiterlesen bis zum Zeilenende, dann melden
      continue;
    }
    {
      eingabe[eingabe_len++] = c;
      starwatch::konsole.print(c);  // Echo, damit man sieht, was man tippt
    }
  }
}

}  // namespace

// ===========================================================================
// setup und loop
// ===========================================================================

/// Ein sichtbares Zeichen mit der Klinge - blockierend.
///
/// ===========================================================================
/// WARUM DIE KONSOLE HIER NICHT REICHT
/// ===========================================================================
/// Der Werksreset ueber Stromzyklen ist ausdruecklich der Weg fuer jemanden,
/// der VOR dem Geraet steht und weder Konsole noch Token hat - "Strom ist der
/// Taster", siehe geraet.h. Seine Rueckmeldung ausschliesslich auf die
/// serielle Schnittstelle zu legen, ist derselbe Fehler wie beim
/// AP-Schluessel: der Weg ist fuer den gedacht, der die Rueckmeldung nicht
/// sehen kann.
///
/// BLOCKIEREND, und das ist hier richtig. Die Effektketten in lib/licht
/// laufen ueber `licht.tick()` aus der Hauptschleife - die gibt es an dieser
/// Stelle noch nicht, und beim Werksreset folgt unmittelbar der Neustart.
///
/// `pwm_direkt` statt `soll_prozent`: das hier ist ein Signal, keine
/// Helligkeit. Es soll bei jeder Reglerstellung gleich aussehen und nicht
/// durch die Kennlinie laufen.
void klinge_meldet(int male, uint32_t an_ms, uint32_t aus_ms) {
  if (male <= 0)
    return;
  for (int i = 0; i < male; i++) {
    licht.pwm_direkt(100.0f);
    delay(an_ms);
    licht.pwm_direkt(0.0f);
    delay(aus_ms);
  }
}

/// Netze suchen und als Liste fuer das Bluetooth-Merkmal aufbereiten.
String ble_netze() {
  const int n = testweb.netze_suchen();
  std::vector<starwatch::Funknetz> v;
  for (int i = 0; i < n; i++) {
    starwatch::Funknetz f;
    f.ssid = WiFi.SSID(i).c_str();
    f.rssi = WiFi.RSSI(i);
    f.offen = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    v.push_back(f);
  }
  WiFi.scanDelete();
  return starwatch::netze_json(v, 512).c_str();
}

/// EINRICHTUNG PER BLUETOOTH (23.10.2026) - laeuft genau so lange wie der
/// offene Zugangspunkt und nimmt dieselbe Angabe an (siehe lib/einrichtung).
void ble_einrichtung_tick() {
  static uint32_t ende_ab = 0;
  if (testweb.offener_ap() && !ble.laeuft() && !ble.beendet()) {
    const String name = String("StarWatch-") + betrieb.seriennummer().c_str();
    ble.starten(name, ble_netze());
    starwatch::konsole.printf("Einrichtung per Bluetooth: '%s' wirbt.\n", name.c_str());
  }
  if (!ble.laeuft())
    return;

  String daten;
  bool scan = false;
  if (ble.auftrag_abholen(&daten, &scan)) {
    std::string ssid, pass;
    if (scan) {
      ble.melden("{\"stand\":\"suche\"}");
      ble.netze(ble_netze());
      ble.melden("{\"stand\":\"bereit\"}");
    } else if (!starwatch::wlan_auftrag_lesen(daten.c_str(), &ssid, &pass)) {
      ble.melden("{\"stand\":\"fehler\",\"grund\":\"angabe\"}");
    } else {
      ble.melden("{\"stand\":\"verbinde\"}");
      starwatch::konsole.println("\n[ble] WLAN-Angabe erhalten.");  // nie das Passwort
      if (testweb.wlan_setzen(ssid.c_str(), pass.c_str()))
        ble.melden("{\"stand\":\"verbunden\",\"ip\":\"" + testweb.heimnetz_adresse() +
                   "\",\"token\":\"" + betrieb.token().c_str() + "\"}");
      else
        ble.melden("{\"stand\":\"fehler\",\"grund\":\"verbindung\"}");
    }
  }

  // Einrichtung vorbei - hier oder im Browser: eine Minute, damit die App
  // den Status noch lesen kann, dann Bluetooth ganz aus und Speicher frei.
  if (!testweb.offener_ap()) {
    if (ende_ab == 0)
      ende_ab = millis();
    else if (millis() - ende_ab > 60000) {
      ble.beenden();
      starwatch::konsole.println("Einrichtung per Bluetooth beendet.");
    }
  }
}

void setup() {
  Serial.begin(g::TMC_BAUD);
#if ARDUINO_USB_CDC_ON_BOOT
  // `Serial` ist das USB-Serial/JTAG des C3. Haengt ein Rechner
  // am Kabel, ohne dass ein Terminal liest, wartet der Kern bei vollem
  // Puffer je write() bis zu 20 x 100 ms (HWCDC.cpp, "host backpressure") -
  // zwei Sekunden, in denen die Hauptschleife steht: kein Uhrbetrieb, kein
  // Webserver, keine Rampe. Mit 0 verwirft er stattdessen sofort. Die Zeilen
  // stehen ohnehin auch im Netz-Log (/test/log).
  Serial.setTxTimeoutMs(0);
#endif

  // ===================================================================
  // STROMZYKLEN ZUERST ZAEHLEN - vor allem anderen, auch vor dem Warten auf
  // die Konsole. Wie bei einer Lampe: fuenfmal kurz aus und an. Die Uhr ist
  // dabei jedes Mal nur einen Augenblick an; gezaehlt wurde vorher erst nach
  // Sensoren und Treiber, also gut eine Sekunde spaeter - dann war sie
  // schon wieder aus (26.09.2026).
  //
  // BROWNOUT ZAEHLT MIT. Beim kurzen Abziehen haelt C1 die Schiene noch
  // einen Moment; die Spannung faellt nicht ganz auf null, und der Chip
  // meldet einen Einbruch statt eines Einschaltens. Eine Brownout-SCHLEIFE
  // kann trotzdem keinen Reset ausloesen: nach einem Brownout startet die
  // Uhr kein WLAN (siehe unten), und damit endet die Schleife nach einem Mal.
  // ===================================================================
  const esp_reset_reason_t start_grund = esp_reset_reason();
  const bool werksreset_faellig = betrieb.zyklus_zaehlen(
      start_grund == ESP_RST_POWERON || start_grund == ESP_RST_BROWNOUT);

  delay(400);

  starwatch::konsole.printf("\n\n=== Star Watch - Betriebsfirmware %s ===\n",
                            g::FIRMWARE_VERSION);

  // Grund des letzten Neustarts. AP 6.3 verlangt ihn ohnehin im
  // Diagnoseendpunkt - hier steht er, weil er beim Aufbau sofort hilft.
  //
  // Der Brownout ist der Fall, der diese Zeile eingebracht hat: er trat beim
  // Einschalten des Funkmoduls auf und legte das Geraet in eine
  // Neustartschleife, in der es sich nicht mehr flashen liess. Zu SEHEN war
  // ohne diese Meldung nur ein Geraet, das staendig von vorn anfing.
  const bool kam_von_brownout = (esp_reset_reason() == ESP_RST_BROWNOUT);

  // BROWNOUTS IN FOLGE ZAEHLEN (26.09.2026). Bisher sperrte schon EIN
  // Brownout das WLAN - und ein Brownout ist auch, wer die Uhr kurz aus- und
  // wieder einsteckt (C1 haelt die Schiene einen Moment). Die Uhr war danach
  // offline bis zum naechsten richtigen Einschalten. Jetzt: nach dem ersten
  // Brownout EIN Versuch; erst der zweite in Folge heisst "es war wirklich der
  // Funkstart" und sperrt - damit endet eine Schleife trotzdem nach zwei
  // Durchlaeufen. Zurueckgesetzt wird durch jeden anderen Start und nach einer
  // Minute ruhigen Laufs (loop()).
  {
    Preferences p;
    p.begin("sw_start", false);
    const uint32_t alt = p.getUInt("bo_folge", 0);
    brownout_folge = kam_von_brownout ? alt + 1 : 0;
    if (brownout_folge != alt)
      p.putUInt("bo_folge", brownout_folge);
    p.end();
  }
  const bool wlan_sperren = brownout_folge >= 2;
  // Nach EINEM Brownout mit gedrosselter Sendeleistung funken - siehe
  // SENDELEISTUNG in testweb.cpp. Gilt nur fuer diesen Start.
  if (brownout_folge >= 1)
    starwatch::Testweb::funk_drosseln();

  switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT:
      starwatch::konsole.println(
          "LETZTER NEUSTART: BROWNOUT - die Versorgungsspannung ist eingebrochen.\n"
          "Das passiert beim Einschalten des Funkmoduls, wenn die 3,3-V-Schiene\n"
          "die Stromspitze des Senders nicht traegt. Die Sendeleistung ist schon\n"
          "auf 11 dBm gedrosselt; bleibt es dabei, ist es die HARDWARE:\n"
          "  - USB-Kabel und Port: ein Ladekabel oder ein schwacher Hub genuegt nicht\n"
          "  - Elko nahe am Board (470 uF und mehr) an 3,3 V gegen Masse\n"
          "  - VM/Netzteil des Aufbaus pruefen - der Treiber schweigt dann auch\n"
          "Solange das anhaelt, laesst sich das Geraet kaum flashen: es startet\n"
          "mitten im Schreibvorgang neu.");
      break;
    case ESP_RST_PANIC:
      starwatch::konsole.println("LETZTER NEUSTART: Absturz (Panic). Backtrace im Log davor.");
      break;
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      starwatch::konsole.println("LETZTER NEUSTART: Watchdog - etwas hat die Schleife blockiert.");
      break;
    case ESP_RST_SW:
      starwatch::konsole.println("Letzter Neustart: Software (Reboot oder nach einem Brownout).");
      break;
    default:
      break;
  }
  starwatch::konsole.printf("1/%ld Mikroschritte, %ld je Umdrehung, %.3f s je Schritt\n",
                (long)g::MIKROSCHRITTE, (long)g::REV, (double)g::SEK_JE_SCHRITT);

  if (!licht.beginn())
    starwatch::konsole.printf(
        "FEHLER: LEDC an GPIO%u liess sich nicht einrichten (%lu Hz, PWM).\n"
        "Die Klinge bleibt dunkel. Das ist KEIN Fehler der Verdrahtung -\n"
        "Frequenz und Aufloesung passen nicht zum 80-MHz-Takt des LEDC.\n",
        g::PIN_LED_PWM, (unsigned long)g::LED_PWM_HZ);

  // DEN ZAEHLERSTAND ZEIGEN, sobald das Licht steht - nicht erst nach
  // Sensoren und Treiber. Wer fuenfmal aus- und einsteckt, zaehlt sonst im
  // Kopf mit und weiss nie, ob die Uhr mitgezaehlt hat. So blinkt sie bei
  // jedem Einschalten so oft, wie sie schon gezaehlt hat. Beim fuenften Mal
  // blinkt sie nicht hier, sondern sechsmal schnell beim Reset unten.
  if (betrieb.zyklen() > 0 && !werksreset_faellig)
    klinge_meldet(static_cast<int>(betrieb.zyklen()), 150, 200);
  // ===================================================================
  // I2C-BUS UND DIE BEIDEN SENSOREN DARAN
  //
  // Der Bus kommt VOR dem Antrieb: ohne Hall-Sensor kann die Uhr nicht
  // referenzieren, und das soll im Protokoll oben stehen und nicht
  // zwischen den Treibermeldungen untergehen.
  // ===================================================================
  if (!starwatch::i2cbus::beginn()) {
    starwatch::konsole.printf(
        "FEHLER: I2C an SDA=GPIO%u / SCL=GPIO%u liess sich nicht einrichten.\n",
        g::PIN_SDA, g::PIN_SCL);
  } else {
    // Absuchen und melden, was da ist. Zwei der drei Steckplaetze sind
    // optional, und ein Suchlauf, der sagt was er findet, spart jede
    // spaetere Ratestunde: steht hier nichts, liegt es an Stecker oder
    // Ader und nicht am Treiber.
    uint8_t gefunden[8];
    const size_t n = starwatch::i2cbus::absuchen(gefunden, sizeof(gefunden));
    starwatch::konsole.printf("I2C-Bus      %u Teilnehmer\n", (unsigned)n);
    for (size_t i = 0; i < n && i < sizeof(gefunden); i++) {
      const char *wer = starwatch::i2cbus::name(gefunden[i]);
      starwatch::konsole.printf("             0x%02X  %s\n", (unsigned)gefunden[i],
                    wer != nullptr ? wer : "UNBEKANNT - gehoert hier nicht hin");
    }
    if (n == 0)
      starwatch::konsole.println(
          "             Kein einziger. Die Pull-ups sitzen auf dem Brett\n"
          "             (R11/R12) - also Stecker J5 und Ader pruefen.");
  }

  if (!hall.beginn())
    starwatch::konsole.printf(
        "FEHLER: Hall-Sensor an J5 - %s.\n"
        "Die Uhr kann NICHT referenzieren und weiss nicht, wo der Zeiger steht.\n"
        "Alles andere laeuft; 'h' wird abbrechen, statt eine Umdrehung zu suchen.\n",
        starwatch::tmaggrund_text(hall.sensorgrund()));

  if (lux.beginn())
    starwatch::konsole.printf(
        "Umgebungslicht  VEML7700 an J8 gefunden. Automatik steht bereit.\n");
  else
    starwatch::konsole.printf(
        "Umgebungslicht  %s. Die Klinge bleibt auf dem eingestellten Wert,\n"
        "                ohne Nachtabschaltung. Das ist kein Fehler der Uhr.\n",
        starwatch::luxgrund_text(lux.grund()));

  starwatch::konsole.print("Antrieb ... ");
  const bool uart_ok = antrieb.beginn();
  starwatch::konsole.println(uart_ok ? "Treiber antwortet, Register gesetzt."
                         : "TREIBER ANTWORTET NICHT.");
  if (!uart_ok)
    starwatch::konsole.println(
        "Der Antrieb laeuft trotzdem - die Impulse kommen aus dem ESP. Die\n"
        "Register sind aber NICHT gesetzt, und VREF (U2.17) ist unbeschaltet:\n"
        "der Strom steht damit nicht fest. Nach oben begrenzt ihn R_SENSE\n"
        "0,47 Ohm auf 0,47 A RMS - unter dem Motornennstrom.\n"
        "'d' sagt, woran es liegen kann.");

  betrieb.beginn(&antrieb, &hall, &licht);

  // Den gemerkten Lichtzustand MERKEN, aber noch nicht anwenden. Angewandt
  // wird er in zuenden_wenn_bereit(), sobald die Referenz steht - siehe dort.
  // Ohne diese zwei Zeilen leuchtet die Klinge beim Start unabhaengig davon,
  // ob der Zeiger seine Lage kennt.
  klinge_wunsch_an = licht.ist_an();
  licht.an(false);

  // ===================================================================
  // WERKSRESET DURCH STROMZYKLEN - der Weg zurueck ohne Token.
  // Begruendung bei den Kennzahlen in geraet.h. Gezaehlt wurde ganz oben
  // in setup(); hier wird ausgefuehrt, weil der Werksreset den geoeffneten
  // Benutzerbereich (betrieb.beginn) braucht.
  // ===================================================================
  if (werksreset_faellig) {
    starwatch::konsole.printf(
        "\n*** WERKSRESET durch %lu Stromzyklen ***\n"
        "Benutzerwerte und WLAN werden geloescht. Der Hall-Offset BLEIBT -\n"
        "die Kalibrierung geht nicht verloren. Neustart in 2 s.\n",
        (unsigned long)g::RESET_ZYKLEN);
    betrieb.werksreset();
    testweb.wlan_setzen("", "");
    // ERST JETZT den Zaehler loeschen - der Reset ist geschehen. Wer vorher
    // abzieht, bekommt ihn beim naechsten Einschalten.
    betrieb.zyklen_loeschen();
    klinge_meldet(6, 120, 180);  // sechs schnelle Blitze: es ist passiert
    ESP.restart();
  }
  if (betrieb.zyklen() > 0)
    starwatch::konsole.printf(
        "Stromzyklen: %lu von %lu. Noch %lu mal kurz aus- und einschalten fuer\n"
        "den Werksreset - oder die Uhr laenger als %lu s laufen lassen, dann\n"
        "faellt der Zaehler zurueck.\n",
        (unsigned long)betrieb.zyklen(), (unsigned long)g::RESET_ZYKLEN,
        (unsigned long)(g::RESET_ZYKLEN - betrieb.zyklen()),
        (unsigned long)(g::RESET_FENSTER_MS / 1000u));

  // ===================================================================
  // GERAETEBLATT (AP 6.2). Beim ersten Start wuerfelt sich jede Uhr eigene
  // Passwoerter. Ohne sie AUFZUSCHREIBEN ist sie in zwei Jahren nicht mehr
  // erreichbar - deshalb steht das hier so auffaellig.
  // ===================================================================
  // NUR NACH SERIAL (AP 1.3): `konsole` schriebe es zusaetzlich in den
  // Ringpuffer, und den liefert `/test/log` OFFEN aus - das Token stuende dann
  // fuer jeden im Netz lesbar da, und der Schutz waere zum Fenster hinaus. Am
  // Kabel ist es richtig: dort sitzt jemand mit physischem Zugriff, und genau
  // der soll das Blatt abschreiben koennen.
  // WOHIN das Blatt geht, haengt an der Fassung - siehe ENTWICKLUNGSGERAET.
  if (g::ENTWICKLUNGSGERAET) {
    // Auch ins Netz-Log: an diesem Aufbau wird ohne Kabel gearbeitet, und ein
    // Token, das nur auf der seriellen Konsole steht, waere unerreichbar.
    starwatch::konsole.printf(
        "\n--- GERAETEBLATT - AUFSCHREIBEN ---\n%s"
        "-----------------------------------\n"
        "ENTWICKLUNGSFASSUNG: dieses Blatt steht auch im NETZ-Log. Fuer eine\n"
        "Uhr, die das Haus verlaesst, mit 'starwatch_produkt' bauen - dann\n"
        "gibt es Geheimnisse nur ueber Serial oder den geschuetzten Endpunkt.\n",
        betrieb.geraeteblatt().c_str());
  } else {
    Serial.printf("\n--- GERAETEBLATT - AUFSCHREIBEN ---\n%s"
                  "-----------------------------------\n"
                  "Gilt NUR fuer dieses Geraet, ueberlebt Werksreset und Update.\n",
                  betrieb.geraeteblatt().c_str());
    starwatch::konsole.println(
        "\nGeraeteblatt auf der seriellen Konsole ausgegeben - Passwoerter und\n"
        "Token stehen absichtlich NICHT im Netz-Log. In der Oberflaeche unter\n"
        "'Geraet'; dort verlangt es das Token.");
  }

  // Waehrend das Update laedt, bedient der Webserver niemanden - sonst
  // konkurrieren Abfragen der Oberflaeche mit dem Datenstrom.
  aktualisierung.ruhe_hook([](bool an) { starwatch::Testweb::stumm(an); });
  aktualisierung.beginn(g::FIRMWARE_VERSION);
  // Nur fuer den Diagnosebericht - die Oberflaeche laeuft auch ohne.
  testweb.aktualisierung_anbinden(&aktualisierung);
  if (aktualisierung.auf_bewaehrung())
    starwatch::konsole.println(
        "\nDIESES IMAGE LAEUFT AUF BEWAEHRUNG. Es wird erst bestaetigt, wenn\n"
        "WLAN steht, die Zeit gueltig ist UND ein Homing gelaufen ist.\n"
        "Bis dahin faehrt der Bootloader bei einem Neustart die vorige Fassung.");
  starwatch::konsole.printf("Hall-Offset aus NVS: %ld\n", (long)betrieb.hall_offset());

  // Weboberflaeche ZULETZT: sie braucht alle Bibliotheken, und wenn das WLAN
  // nicht steht, soll der Rest trotzdem schon gelaufen sein.
  //
  // NACH ZWEI BROWNOUTS IN FOLGE WIRD SIE NICHT GESTARTET (siehe oben). Das
  // Einschalten des Funkmoduls war dann die Ursache des Einbruchs; wuerde die
  // Firmware es stur wieder versuchen, braeche sie sofort wieder ein - genau
  // die Neustartschleife, in der sich das Geraet nicht mehr flashen liess.
  //
  // Das ist keine Notloesung fuer den Aufbau, sondern eine Regel, die auch
  // fuer das Produkt gilt: ein Geraet darf sich nicht selbst in eine
  // Neustartschleife bringen. Es soll erreichbar bleiben - und sei es nur
  // ueber das Kabel -, damit man es reparieren kann.
  if (kam_von_brownout && !wlan_sperren)
    starwatch::konsole.println(
        "\nLetzter Neustart war ein Brownout - WLAN wird EINMAL versucht. Bricht\n"
        "die Versorgung dabei wieder ein, bleibt es beim naechsten Start aus.");
  if (wlan_sperren) {
    starwatch::konsole.printf(
        "\nWLAN WIRD NICHT GESTARTET: %lu Brownouts in Folge - der Funkstart\n"
        "bringt die Versorgung zum Einbruch. Alles andere laeuft: Motor, Hall,\n"
        "Licht, Uhr. Mit 'n' kann das WLAN von Hand versucht werden; ein\n"
        "normales Aus- und Einschalten hebt die Sperre auf.\n",
        (unsigned long)brownout_folge);
  } else {
    testweb.beginn(&antrieb, &hall, &licht, &lux, &betrieb, kommando);
    // MQTT NACH dem WLAN. `beginn` verbindet noch nicht - es merkt sich nur
    // die Zeiger; der erste Versuch faellt in die Hauptschleife, sobald ein
    // Broker eingetragen und der Haken gesetzt ist.
    mqtt.beginn(&betrieb, &licht, &antrieb, &lux, kommando);
  }

  // =========================================================================
  // EINSCHALTEN: Klinge zuenden, dann Referenzfahrt
  // =========================================================================
  //
  // WARUM DIE REFERENZFAHRT HIER UND NICHT AUF ZURUF: nach einem Neustart ist
  // die Position VERLOREN. Der Zaehler steht auf 0, aber der Zeiger steht
  // irgendwo - und eine Uhr, die eine Zeit anzeigt, die sie nicht kennt, ist
  // schlimmer als eine, die steht. Das Briefing sagt es als Regel: "Nach
  // Reboot ist die Position verloren -> Homing laeuft vor allem anderen." Bis
  // 23.08.2026 war das eine Regel ohne Umsetzung.
  //
  // ===================================================================
  // DIE KLINGE BLEIBT HIER DUNKEL. Geaendert am 24.09.2026.
  // ===================================================================
  // Hier stand `licht.zuenden()`, mit der Begruendung, die Zuendkette laufe
  // ueber `pumpe()` waehrend der Referenzfahrt weiter. DAS WAR NICHT WAHR:
  // `Betrieb::referenzfahrt()` ruft in seinen ersten Zeilen
  // `effekt_abbrechen()` und `an(false)`. Der Effekt kam nie ueber die
  // zweite Stufe hinaus - uebrig blieb ein Zucken.
  //
  // Schlimmer war die Folge am Ende: `referenzfahrt()` stellt die Klinge
  // wieder her, "AUCH wenn die Fahrt abgebrochen ist". Eine Uhr ohne
  // Hall-Sensor leuchtete also nach dem Start, ohne je gefahren zu sein -
  // sie leuchtete und zeigte nichts.
  //
  // Jetzt zuendet sie erst, wenn sie wirklich etwas zu zeigen hat: nach
  // erfolgreicher Referenzfahrt UND mit gueltiger Zeit. Siehe
  // `zuenden_wenn_bereit()`. Damit ist die leuchtende Klinge eine
  // Betriebsmeldung und keine Startanimation - und eine dunkle Klinge sagt
  // "ich weiss nicht, wo ich stehe" oder "ich kenne die Zeit nicht".

  // Zeit holen, BEVOR die Referenzfahrt blockiert: SNTP braucht ein paar
  // Sekunden, und die laufen dann waehrend der Referenzfahrt ab statt danach.
  ntp_starten();

  starwatch::konsole.println("\nHoming beim Start. 's' bricht ab.");
  starwatch::Homingergebnis start_e;
  {
    Langlauf sperre;
    start_e = betrieb.referenzfahrt(pumpe, starwatch::Hominganlass::start);
  }
  starwatch::konsole.printf("%s\n", starwatch::hominggrund_text(start_e.grund));
  if (!start_e.ok())
    starwatch::konsole.println(
        "Ohne Referenz kennt die Uhr ihre Lage nicht - sie zeigt erst nach\n"
        "einem erfolgreichen 'h' die richtige Zeit. Ursachen nennt 'd'.\n"
        "DIE KLINGE BLEIBT SOLANGE DUNKEL. Das ist kein Fehler des Lichts,\n"
        "sondern die Aussage, dass die Uhr ihre Lage nicht kennt.");
  abbruch_gewuenscht = false;

  starwatch::konsole.printf(
      "\nEinstellungen aus NVS: Tempo %lu/s, Rampe %lu, %ld Schwungkreise,\n"
      "Licht %.0f %%, Zeitmodus %s.\n",
      (unsigned long)betrieb.fahrtempo(), (unsigned long)antrieb.beschleunigung(),
      (long)betrieb.effekt_kreise(), (double)licht.soll_prozent(),
      starwatch::zeitmodus_text(betrieb.modus_nach_zeit()));

  if (ntp_gestartet)
    starwatch::konsole.println(
        "\nDie Zeit kommt aus dem Netz - sobald sie steht, laeuft der gemerkte\n"
        "Zeitmodus von selbst an. Nichts weiter zu tun.");
  else
    starwatch::konsole.println(
        "\nKEINE ZEIT AUS DEM NETZ (kein Heimnetz oder eigener Zugangspunkt).\n"
        "Dann von Hand:\n"
        "  z 14:35:00   Uhrzeit stellen (im Browser: Knopf 'Browserzeit')\n"
        "Danach laeuft der gemerkte Zeitmodus an.");
  starwatch::konsole.println("'?' zeigt alle Kommandos.");
  if (testweb.verbunden())
    starwatch::konsole.printf("Oberflaeche: http://%s/\n", testweb.adresse().c_str());
  starwatch::konsole.print("\n> ");
}

// ===========================================================================
// SELBSTHEILUNG DER REFERENZFAHRT
// ===========================================================================
// Begruendung bei den Kennzahlen in geraet.h. Beide Faelle laufen ueber
// dieselbe Funktion, weil beide dieselben Vorbedingungen haben: es darf
// nichts anderes den Zeiger brauchen.

/// Eine Referenzfahrt aus der Hauptschleife heraus, mit Sperre und Meldung.
void referenzfahrt_nachholen(const char *warum, starwatch::Hominganlass anlass) {
  starwatch::konsole.printf("\n[auto] Homing: %s\n", warum);
  homing_letzter_versuch_ms = millis();
  starwatch::Homingergebnis e;
  {
    Langlauf sperre;
    e = betrieb.referenzfahrt(pumpe, anlass);
  }
  abbruch_gewuenscht = false;
  if (e.ok()) {
    homing_fehlschlaege = 0;
    if (betrieb.letzte_vergleichbar())
      starwatch::konsole.printf("[auto] OK, Abweichung %ld Schritte.\n",
                                (long)betrieb.letzte_abweichung());
    else
      starwatch::konsole.printf("[auto] OK.\n");
    verlust_melden("[auto] ");
    starwatch::konsole.print("> ");
  } else {
    homing_fehlschlaege++;
    starwatch::konsole.printf(
        "[auto] %s (%lu. Fehlschlag). Naechster Versuch in %lu min.\n"
        "       Haeufigste Ursache: der Magnet sitzt nicht mehr richtig.\n> ",
        starwatch::hominggrund_text(e.grund), (unsigned long)homing_fehlschlaege,
        (unsigned long)((homing_fehlschlaege >= g::HOMING_VERSUCHE_KURZ
                             ? g::HOMING_WIEDERHOLUNG_LANG_MS
                             : g::HOMING_WIEDERHOLUNG_MS) / 60000u));
  }
}

void referenzfahrt_pflege() {
  // Nichts anfassen, solange etwas anderes den Zeiger braucht.
  if (langlauf || betrieb.timer_laeuft() || antrieb.laeuft())
    return;
  // Und nicht waehrend des Einmessens - dort haelt der Bediener die Lage
  // absichtlich fest.
  if (betrieb.rueckfall_gesperrt())
    return;

  // --- 1. Nach einem Fehlschlag wiederholen ---
  if (!betrieb.letztes_homing().ok()) {
    const uint32_t frist = homing_fehlschlaege >= g::HOMING_VERSUCHE_KURZ
                               ? g::HOMING_WIEDERHOLUNG_LANG_MS
                               : g::HOMING_WIEDERHOLUNG_MS;
    if (homing_letzter_versuch_ms != 0 &&
        millis() - homing_letzter_versuch_ms < frist)
      return;
    referenzfahrt_nachholen("letzte war nicht erfolgreich",
                            starwatch::Hominganlass::nachholen);
    return;
  }

  // --- 2. Einmal taeglich ---
  struct tm t;
  if (!getLocalTime(&t, 0))
    return;  // ohne Zeit kein Tagesrhythmus
  // DIE MARKE MUSS AUSSERHALB DER STUNDE ZURUECKFALLEN. Stand sie erst einmal
  // auf der Auslesestunde, sperrte sie am naechsten Tag genauso - aus
  // "taeglich" waere "einmal, nie wieder" geworden. Beim Durchdenken der
  // Tageslogik am 24.08.2026 gefunden, bevor es je aufgefallen waere: der
  // Fehler zeigt sich erst am ZWEITEN Tag.
  if (t.tm_hour != g::HOMING_STUNDE) {
    homing_letzte_stunde = -1;
    return;
  }
  if (t.tm_hour == homing_letzte_stunde)
    return;  // in dieser Stunde schon gelaufen
  homing_letzte_stunde = t.tm_hour;
  referenzfahrt_nachholen("taeglich", starwatch::Hominganlass::taeglich);
}

namespace {

/// Hat die Klinge seit dem Einschalten schon einmal gezuendet?
///
/// EINMAL JE START, und das ist wichtig: spaeter schaltet der Benutzer das
/// Licht selbst, und ein zweites Zuenden wuerde ihm dazwischenfunken. Auch
/// eine von Hand ausgeloeste Referenzfahrt darf es nicht erneut ausloesen.
bool klinge_gezuendet = false;


/// Seit wann steht die Referenz? 0 = noch nicht. Bezugspunkt fuer die Frist,
/// die die Klinge der Zeit einraeumt.
uint32_t referenz_seit_ms = 0;

/// Zuenden, sobald die Uhr wirklich die Zeit zeigt.
///
/// EINE HARTE BEDINGUNG UND EINE FRIST:
///
///   HART: die Referenzfahrt war erfolgreich. Ohne Referenz kennt der Zeiger
///   seine Lage nicht, und eine leuchtende Klinge ueber einer geratenen
///   Position ist eine Falschaussage. Ohne sie zuendet sie nie.
///
///   FRIST: danach bekommt die Zeit ZUEND_FRIST_OHNE_ZEIT_MS, um einzutreffen.
///   Steht sie, zuendet die Klinge sofort - der Zeiger zeigt dann im selben
///   Augenblick das Richtige. Steht sie nach der Frist immer noch nicht,
///   zuendet sie trotzdem.
///
/// WARUM DIE FRIST UND KEINE ZWEITE HARTE BEDINGUNG: hier stand bis zum
/// Abend des 24.09.2026 "ohne Zeit bleibt sie dunkel". Das war zu streng.
/// Dieses Geraet ist mehr als eine Uhr - Timer, Licht, Home Assistant und die
/// Sensoren laufen ohne Zeit weiter, nur der Zeiger nicht. Aus einer
/// fehlenden Zeitanzeige ein totes Geraet zu machen, war ein Denkfehler: die
/// Klinge ist das LICHT, nicht die Zeitanzeige.
///
/// GEPRUEFT WIRD `zeit_gueltig()`, NICHT `zeit_aus_netz()`. Der zweite ist
/// nur wahr, wenn die Zeit aus dem NETZ kam; wer sie von Hand stellt
/// ('z 14:30'), setzt den ersten. Mit der falschen Abfrage waere die Klinge
/// bei von Hand gestellter Zeit nie angegangen.
void zuenden_wenn_bereit() {
  if (klinge_gezuendet)
    return;
  if (!betrieb.letztes_homing().ok())
    return;  // ohne Referenz gar nicht - das ist die harte Bedingung

  if (referenz_seit_ms == 0)
    referenz_seit_ms = millis();

  const bool zeit_da = betrieb.zeit_gueltig();
  if (!zeit_da && millis() - referenz_seit_ms < g::ZUEND_FRIST_OHNE_ZEIT_MS)
    return;  // der Zeit ihre Frist lassen

  klinge_gezuendet = true;

  // Wollte der Benutzer sie aus, bleibt sie aus. Die Referenz ist die
  // Bedingung dafuer, dass sie an sein DARF - keine Anweisung, dass sie an
  // sein SOLL.
  if (!klinge_wunsch_an) {
    starwatch::konsole.println(
        "\n[start] Referenz steht. Die Klinge bleibt aus, weil sie beim "
        "letzten Mal aus war.");
    return;
  }

  // licht.zuenden() fragt den Schalter "Zuend- und Einfahreffekt" selbst ab:
  // an = Lichtschwert-Zuendung, aus = ruhiges Einblenden (28.09.2026).
  licht.zuenden();
  if (zeit_da)
    starwatch::konsole.println(
        "\n[start] Referenz steht und die Zeit ist da - Klinge an.");
  else
    starwatch::konsole.printf(
        "\n[start] Referenz steht, aber nach %lu s keine Zeit - Klinge trotzdem\n"
        "        an. Timer, Licht und Home Assistant laufen; der Zeiger bleibt\n"
        "        auf zwoelf stehen, bis die Zeit kommt.\n",
        (unsigned long)(g::ZUEND_FRIST_OHNE_ZEIT_MS / 1000u));
}

/// Umgebungslicht messen und anwenden. Aus der Hauptschleife.
void licht_automatik_tick() {
  lux.tick();

  // Ohne Sensor oder mit abgeschalteter Automatik: Faktor auf 1 und nichts
  // weiter. Der Aufruf ist billig und macht den Rueckweg eindeutig - sonst
  // bliebe der zuletzt gesetzte Faktor stehen, wenn der Stecker an J8 im
  // Betrieb herausfaellt.
  const starwatch::Lichtautomatik &a = betrieb.lichtautomatik();

  // Hat die NACHT die Klinge ausgeschaltet und wird die Nachtabschaltung
  // (oder die ganze Automatik) jetzt abgestellt, geht sie wieder an - sonst
  // bliebe sie dunkel, bis jemand von Hand schaltet, und niemand wuesste
  // warum. Nur dann: eine von Hand ausgeschaltete Klinge bleibt aus.
  static bool nacht_hat_aus = false;
  static bool erster_lauf = true;
  static bool letzter_automat = true;
  if (nacht_hat_aus && (!a.nacht || !lux.bereit())) {
    nacht_hat_aus = false;
    if (!licht.ist_an())
      licht.zuenden();
    // Den Schalter auf "an" zuruecksetzen: sonst stuende er noch auf "Nacht",
    // und nach dem Wiedereinschalten der Nachtabschaltung bliebe die Klinge
    // in einem dunklen Zimmer an, bis es einmal hell und wieder dunkel war.
    const starwatch::Nachtschalter &alt = betrieb.nachtschalter();
    betrieb.nachtschalter() =
        starwatch::Nachtschalter(alt.aus_lux(), alt.an_lux(), g::LUX_NACHT_VERZUG_MS,
                                 g::LUX_NACHT_AN_VERZUG_MS);
    letzter_automat = true;
  }

  const bool messwert = lux.bereit() && lux.gemessen();

  // 1. Dimmen - WEICH (27.09.2026).
  //
  // Das ZIEL kommt aus dem Messwert, oder ist 1, wenn die Automatik aus ist
  // oder der Sensor fehlt. Der FAKTOR laeuft ihm im Takt LUX_BLENDE_TAKT_MS
  // exponentiell hinterher. Vorher wurde er direkt gesetzt, und weil der
  // Sensor nur alle 2 s misst, sprang die Klinge in sichtbaren Stufen.
  //
  // NUR IM TAKT SCHREIBEN, und nur bei einer Aenderung: licht.dimmfaktor()
  // rechnet die Kennlinie neu und schreibt das Tastverhaeltnis in den LEDC,
  // diese Funktion laeuft aber in JEDEM Schleifendurchlauf. Steht der Faktor
  // am Ziel, liefert dimm_nachfuehren genau das Ziel, und es wird nichts
  // mehr geschrieben.
  {
    const float ziel = (a.an && messwert)
        ? starwatch::licht_anteil(lux.lux(), g::LUX_DUNKEL, a.lux_hell, a.anteil_min)
        : 1.0f;
    static uint32_t blende_ms = 0;
    // ZWEI STUFEN hintereinander (28.09.2026): die erste folgt dem Ziel, der
    // Faktor folgt der ersten. Eine einzelne Stufe springt zu Beginn am
    // schnellsten los und kriecht dann aus - das sah man als Ruck. Zwei
    // ergeben eine S-Kurve: sanft los, sanft an. Und etwas langsamer: 95 %
    // nach rund 4,7 statt 3 Zeitkonstanten.
    static float stufe = -1.0f;
    if (stufe < 0.0f)
      stufe = licht.dimmfaktor();
    const uint32_t jetzt = millis();
    if (jetzt - blende_ms >= g::LUX_BLENDE_TAKT_MS) {
      // Nach einer Pause (erster Lauf, langer Block) nicht mit der ganzen
      // Pause rechnen - sonst waere der erste Schritt wieder ein Sprung.
      uint32_t dt = jetzt - blende_ms;
      if (blende_ms == 0 || dt > 4u * g::LUX_BLENDE_TAKT_MS)
        dt = g::LUX_BLENDE_TAKT_MS;
      blende_ms = jetzt;
      stufe = starwatch::dimm_nachfuehren(stufe, ziel, dt, g::LUX_BLENDE_MS);
      const float neu = starwatch::dimm_nachfuehren(licht.dimmfaktor(), stufe, dt,
                                                    g::LUX_BLENDE_MS);
      if (neu != licht.dimmfaktor())
        licht.dimmfaktor(neu);
    }
  }
  if (!a.nacht || !messwert)
    return;

  const float hell = lux.lux();

  // 2. Nachtabschaltung.
  //
  // NUR BEI EINER FLANKE ZUGREIFEN, nie dauernd. Das ist der ganze Trick,
  // mit dem die Automatik neben allem anderen bestehen kann, das die Klinge
  // schaltet - dem Kommando 'l'/'lo', der Weboberflaeche, Home Assistant
  // und vor allem lib/betrieb, das waehrend der Referenzfahrt und der
  // Leerfahrt selbst ein- und ausschaltet.
  //
  // Wuerde hier in jedem Durchlauf licht.an(soll_an) stehen, kaempfte die
  // Automatik mit jedem von ihnen: von Hand ausgeschaltet waere die Klinge
  // zwei Sekunden spaeter wieder an, und die Referenzfahrt koennte sie nicht
  // mehr dunkel halten.
  //
  // So greift die Automatik genau zweimal am Tag zu - wenn es dunkel wird
  // und wenn es hell wird. Dazwischen gehoert die Klinge dem, der sie
  // zuletzt angefasst hat.
  const bool soll_an = betrieb.nachtschalter().aktualisiere(hell, millis());

  if (erster_lauf) {
    // Beim Start NICHT zugreifen: der Schalter faengt auf "an" an, und ein
    // Zugriff hier setzte die Klinge auf einen Wert, den niemand verlangt
    // hat. Erst die erste echte Umschaltung zaehlt.
    erster_lauf = false;
    letzter_automat = soll_an;
    return;
  }
  if (soll_an == letzter_automat)
    return;

  letzter_automat = soll_an;
  // MIT ANIMATION (28.09.2026): hell geworden -> die Klinge zuendet, dunkel
  // geworden -> sie faehrt ein. Vorher ging sie einfach aus bzw. an.
  if (soll_an)
    licht.zuenden();
  else
    licht.loeschen();
  nacht_hat_aus = !soll_an;
  starwatch::konsole.printf("\n[licht] %.1f lx -> Klinge %s.\n> ", (double)hell,
                soll_an ? "an" : "aus (Nacht)");
}

}  // namespace

void loop() {
  konsole_lesen();
  testweb.tick();
  // Eine Minute ruhig gelaufen: die Brownouts davor waren keine Folge mehr.
  // Einmal schreiben, nicht in jedem Durchlauf.
  if (brownout_folge > 0 && millis() > 60000) {
    Preferences p;
    p.begin("sw_start", false);
    p.putUInt("bo_folge", 0);
    p.end();
    brownout_folge = 0;
  }
  // Erst hier, nicht in setup(): kam das WLAN spaeter (Brownout-Fall, 'n',
  // 'ns'), soll die Zeit trotzdem noch geholt werden.
  ntp_starten();
  ntp_nachstellen();
  betrieb.tick();
  ble_einrichtung_tick();

  // EINRICHTUNG SICHTBAR (23.10.2026): solange der offene Zugangspunkt steht,
  // blinkt die Klinge blau - sonst sieht man nicht, ob der Werksreset gegriffen
  // hat. Endet von selbst, sobald das Heimnetz verbunden ist.
  static bool einrichtung_blinkt = false;
  if (testweb.offener_ap() != einrichtung_blinkt) {
    einrichtung_blinkt = testweb.offener_ap();
    if (einrichtung_blinkt)
      licht.signal(starwatch::Licht::Ebene::zustand, 0x0000FF, starwatch::Muster::blinken,
                   -1.0f, 0);
    else
      licht.signal_aus(starwatch::Licht::Ebene::zustand);
  }

  // ---------------------------------------------------------------------
  // Bewaehrung: ein frisch eingespieltes Image bestaetigen - aber STRENG.
  //
  // Eine Firmware, die bootet, aber deren Antrieb nicht anlaeuft, wuerde sich
  // sonst selbst freigeben, und das Rollback waere wertlos. Deshalb alle drei
  // Bedingungen, so wie im Umsetzungsplan AP 3.7.1 gefordert.
  // ---------------------------------------------------------------------
  if (aktualisierung.auf_bewaehrung() && millis() > g::UPDATE_BEWAEHRUNG_MS &&
      testweb.verbunden() && betrieb.zeit_aus_netz() &&
      betrieb.letztes_homing().ok()) {
    aktualisierung.bewaehrt_melden();
    if (!aktualisierung.auf_bewaehrung())
      starwatch::konsole.printf(
          "\n[update] Fassung %s bestaetigt - WLAN, Zeit und Homing\n"
          "         stimmen. Kein Rollback mehr.\n> ", g::FIRMWARE_VERSION);
  }

  betrieb.zyklus_tick();
  mqtt.tick();
  referenzfahrt_pflege();
  zuenden_wenn_bereit();
  licht_automatik_tick();

  // Taegliches Nachsehen. Nur wenn nichts anderes den Zeiger braucht.
  {
    struct tm t;
    const int stunde = getLocalTime(&t, 0) ? t.tm_hour : -1;
    const bool ruhig = !betrieb.timer_laeuft() && !antrieb.laeuft() && !langlauf;
    aktualisierung.tick(stunde, ruhig);
  }

  // Der Rueckfall geschieht von selbst - dann soll man auch lesen koennen,
  // dass er geschehen ist, und nicht raten, warum der Zeiger losfaehrt.
  if (betrieb.rueckfall_abholen())
    starwatch::konsole.printf("\n[auto] %.0f s ohne Handlung -> Uhrbetrieb (%s).\n> ",
                  (double)g::RUECKFALL_MS / 1000.0,
                  starwatch::zeitmodus_text(betrieb.zeitmodus()));
}
