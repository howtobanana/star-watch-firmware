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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <aktualisierung.h>
#include <antrieb.h>
#include <betrieb.h>
#include <geraet.h>
#include <hall.h>
#include <licht.h>
#include <testweb.h>
#include <zeiger.h>

namespace g = starwatch::geraet;

starwatch::Antrieb antrieb;
starwatch::Hall hall;
starwatch::Licht licht;
starwatch::Betrieb betrieb;
starwatch::Aktualisierung aktualisierung;
starwatch::Testweb testweb;

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
      "  u <ziel>    anfahren: Stunde 0..12 ODER Uhrzeit HH:MM[:SS]\n"
      "              u 12   u 4.5   u 15:00   u 21:30   (24 h aufs Zwoelferblatt)\n"
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
      "  h           1. Referenzfahrt: der Zeiger sucht seine Nulllage\n"
      "  v <n>       2. auf zwoelf nachstellen, falls er daneben steht\n"
      "  o!          3. \"das ist zwoelf\" - rechnet den Offset selbst aus\n"
      "              danach 'h' zur Kontrolle. Landet er wieder auf zwoelf,\n"
      "              sitzt es.\n"
      "  o <n>       Offset von Hand zurueckschreiben (0..3199). NUR fuer\n"
      "              notierte Werte, nicht zum Einmessen\n"
      "TIMER\n"
      "  t <sek>     Timer starten (eine Umdrehung in dieser Zeit)\n"
      "  t-          Timer abbrechen\n"
      "LICHT\n"
      "  l <p>       Helligkeit in Prozent (0 = aus)\n"
      "  la / lo     Klinge an / aus\n"
      "  lz / ll     zuenden / loeschen\n"
      "  ls          Stundeneffekt (60 s auf 100 %)\n"
      "  lr / lt     Rampe 0-20 % / Treppe 0-100 % (Messhilfen AP 0.5)\n"
      "  w <p>       PWM direkt in Prozent, ohne Kennlinie\n"
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
      "  na          eigenen Zugangspunkt abschalten (nur mit Heimnetz)\n"
      "  nr          Verbindung neu aufbauen (prueft den Ausfallpfad)\n"
      "  nx          gespeicherte Zugangsdaten vergessen\n"
      "GERAET\n"
      "  y           Firmware und Updates: Stand, Modus, URL\n"
      "  y 0|1|2     Updates aus / manuell / automatisch (taeglich, nachts)\n"
      "  yp / yi     jetzt nachsehen / gefundene Fassung einspielen\n"
      "  b           Neustart\n"
      "  b werksreset  Benutzerwerte und WLAN loeschen; Hall-Offset BLEIBT\n"
      "\nEinstellungen (Tempo, Rampe, Kreise, Zeitmodus, Helligkeit, Offset)\n"
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
    case starwatch::Fuehrung::homing: wer = "Referenzfahrt"; break;
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
    starwatch::konsole.printf("             Schwerpunkt %.2f, Nachlauf %ld, Spanne %.0f mV\n",
                  (double)betrieb.letztes_homing().schwerpunkt,
                  (long)betrieb.letztes_homing().nachlauf,
                  (double)(betrieb.letztes_homing().spanne_v * 1000.0f));
  starwatch::konsole.printf("Hall-Offset  %ld Schritte\n", (long)betrieb.hall_offset());

  starwatch::konsole.printf("Hall         D0 %s, A0 %.3f V, Ruhe %.3f V, Spanne %.0f mV\n",
                hall.magnet_erkannt() ? "MAGNET" : "frei", (double)hall.spannung_gefiltert(),
                (double)hall.ruhe_v(), (double)(hall.spanne_v() * 1000.0f));
  starwatch::konsole.printf("             Flanken %lu, Fenster %ld Schritte%s\n",
                (unsigned long)hall.flanken(), (long)hall.fenster_schritte(),
                hall.fenster_schritte() == 0 ? " (noch nicht gemessen)" : "");

  starwatch::konsole.printf("Licht        %s, Regler %.0f %%, Ausgabe %.1f %%, Effekt %s\n",
                licht.ist_an() ? "an" : "aus", (double)licht.soll_prozent(),
                (double)licht.ausgabe_prozent(),
                starwatch::effekt_text(licht.effekt()));
  starwatch::konsole.printf("Timer        %s (Rest %s)\n",
                betrieb.timer_zustand_text(),
                betrieb.timer_restzeit().c_str());
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
        "Keine Antwort. Ursachen in dieser Reihenfolge:\n"
        " 1 VM fehlt. Ohne Motorspannung schweigt der Treiber, auch wenn VDD\n"
        "   anliegt. Das ist die haeufigste Ursache.\n"
        " 2 TX und RX liegen auf ZWEI verschiedenen PDN-Pads. Sie gehoeren auf\n"
        "   DASSELBE.\n"
        " 3 1-kOhm-Widerstand in Reihe TX fehlt, oder er sitzt in der RX-Ader.\n"
        " 4 Adresse. Steht in geraet.h auf 0x00, weil MS1/MS2 unbeschaltet\n"
        "   sind. Sind sie inzwischen auf 3,3 V gelegt (AP 0.6), gehoert dort\n"
        "   0x03 hin.");
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
        "Laeuft gerade (Referenzfahrt, Grenzsuche oder Pendellauf).\n"
        "Jetzt gehen nur 's' (Abbruch), 'i' (Zustand) und '?'. '%c' ignoriert.\n", k);
    return;
  }

  switch (k) {
    case '?':
      hilfe();
      return;

    case 'i':
      zustand();
      return;

    case 'd':
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
      testweb.beginn(&antrieb, &hall, &licht, &betrieb, kommando);
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
      const long n = atol(rest);
      const bool uhr_aus = betrieb.fahre_auf(static_cast<int32_t>(n));
      starwatch::konsole.printf("Fahre auf Schritt %ld (vorwaerts, Ziel %ld).\n", n,
                    (long)antrieb.ziel());
      uhr_ging_aus(uhr_aus);
      return;
    }

    case 'u': {
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
      if (zweit == 'p') {  // yp = jetzt nachsehen
        Langlauf sperre;
        starwatch::konsole.println("Sehe nach, ob es eine neuere Fassung gibt ...");
        const bool neu_da = aktualisierung.pruefen();
        starwatch::konsole.printf("%s\n", aktualisierung.meldung().c_str());
        if (neu_da)
          starwatch::konsole.println("Einspielen mit 'yi'. Die Uhr steht dabei und\n"
                         "braucht danach eine Referenzfahrt.");
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
              "und danach eine Referenzfahrt braucht. Nie waehrend eines Timers.\n",
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
      abbruch_gewuenscht = false;
      starwatch::konsole.println("\nReferenzfahrt. 's' bricht ab.");
      Langlauf sperre;
      const starwatch::Homingergebnis e = betrieb.referenzfahrt(pumpe);
      starwatch::konsole.printf("%s\n", starwatch::hominggrund_text(e.grund));
      if (e.ok()) {
        starwatch::konsole.printf("  Messpunkte %u, Schwelle %.0f mV (%.0f %% des Gipfels)\n",
                      (unsigned)e.punkte, (double)(e.spanne_v * g::HALL_SCHWELLE_ANTEIL * 1000.0f),
                      (double)(g::HALL_SCHWELLE_ANTEIL * 100.0f));
        starwatch::konsole.printf("  Schwerpunkt %.2f, Flanke bei %ld, Versatz %.2f\n",
                      (double)e.schwerpunkt, (long)e.pos_flanke,
                      (double)(e.schwerpunkt - e.pos_flanke));
        starwatch::konsole.printf("  Ruhelinie %.3f V, Spanne %.0f mV %s\n", (double)e.ruhe_v,
                      (double)(e.spanne_v * 1000.0f),
                      (e.spanne_v * 1000.0f < g::HALL_SPANNE_MIN_V * 1000.0f)
                          ? "-> ZU WENIG, Magnetabstand verringern"
                          : ((e.spanne_v * 1000.0f > g::HALL_SPANNE_MAX_V * 1000.0f)
                                 ? "-> ZU VIEL, Kurve klippt, Abstand vergroessern"
                                 : "-> im Fenster, passt"));
        if (e.fenster > 0) {
          starwatch::konsole.printf("  D0-Fenster %ld Schritte", (long)e.fenster);
          if (e.fenster >= g::HALL_RUECKZUG_SCHRITTE)
            starwatch::konsole.printf(" -> GROESSER ALS DER RUECKZUG (%ld)! "
                          "HALL_RUECKZUG_SCHRITTE in geraet.h erhoehen.\n",
                          (long)g::HALL_RUECKZUG_SCHRITTE);
          else
            starwatch::konsole.printf(" (Rueckzug %ld, passt)\n", (long)g::HALL_RUECKZUG_SCHRITTE);
        }
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
        starwatch::konsole.printf(
            "Gemerkt: der Zeiger steht auf ZWOELF. Offset %ld, in NVS gespeichert.\n",
            (long)betrieb.hall_offset());
        uhr_ging_aus(uhr_aus);
        starwatch::konsole.println(
            "Zur Kontrolle 'h' - landet der Zeiger wieder auf zwoelf, ist es eingemessen.");
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
      starwatch::konsole.printf("PWM direkt %.1f %% (ohne Kennlinie).\n", (double)p);
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
      if (zweit == 'a') {
        licht.an(true);
        betrieb.werte_sichern();
        starwatch::konsole.printf("Klinge an, %.0f %%.\n", (double)licht.soll_prozent());
      } else if (zweit == 'o') {
        licht.an(false);
        betrieb.werte_sichern();
        starwatch::konsole.println("Klinge aus.");
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

void setup() {
  Serial.begin(g::TMC_BAUD);
  delay(400);

  starwatch::konsole.println("\n\n=== Star Watch - Betriebsfirmware, AP 2 ===");

  // Grund des letzten Neustarts. AP 6.3 verlangt ihn ohnehin im
  // Diagnoseendpunkt - hier steht er, weil er beim Aufbau sofort hilft.
  //
  // Der Brownout ist der Fall, der diese Zeile eingebracht hat: er trat beim
  // Einschalten des Funkmoduls auf und legte das Geraet in eine
  // Neustartschleife, in der es sich nicht mehr flashen liess. Zu SEHEN war
  // ohne diese Meldung nur ein Geraet, das staendig von vorn anfing.
  const bool kam_von_brownout = (esp_reset_reason() == ESP_RST_BROWNOUT);

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
  hall.beginn();

  starwatch::konsole.print("Antrieb ... ");
  const bool uart_ok = antrieb.beginn();
  starwatch::konsole.println(uart_ok ? "Treiber antwortet, Register gesetzt."
                         : "TREIBER ANTWORTET NICHT.");
  if (!uart_ok)
    starwatch::konsole.println(
        "Der Antrieb laeuft trotzdem - die Impulse kommen aus dem ESP32, und\n"
        "der Strom ist durch Vref 0,55 V auch ohne UART begrenzt. Aber die\n"
        "Register sind NICHT gesetzt, also gilt der Werkszustand: der Strom\n"
        "kommt vom Trimmpoti. 'd' sagt, woran es liegen kann.");

  betrieb.beginn(&antrieb, &hall, &licht);

  // ===================================================================
  // WERKSRESET DURCH STROMZYKLEN - der Weg zurueck ohne Token.
  // Begruendung bei den Kennzahlen in geraet.h. NUR echtes Einschalten
  // zaehlt; ein Neustart per Software oder ein Update duerfen die Uhr
  // nicht loeschen.
  // ===================================================================
  if (betrieb.zyklus_zaehlen(esp_reset_reason() == ESP_RST_POWERON)) {
    starwatch::konsole.printf(
        "\n*** WERKSRESET durch %lu Stromzyklen ***\n"
        "Benutzerwerte und WLAN werden geloescht. Der Hall-Offset BLEIBT -\n"
        "die Kalibrierung geht nicht verloren. Neustart in 2 s.\n",
        (unsigned long)g::RESET_ZYKLEN);
    betrieb.werksreset();
    testweb.wlan_setzen("", "");
    delay(2000);
    ESP.restart();
  }
  if (betrieb.zyklen() > 0)
    starwatch::konsole.printf(
        "Stromzyklen: %lu von %lu. Noch %lu mal aus- und einschalten fuer den\n"
        "Werksreset - oder die Uhr laenger als %lu s laufen lassen, dann faellt\n"
        "der Zaehler zurueck.\n",
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

  aktualisierung.beginn(g::FIRMWARE_VERSION);
  // Nur fuer den Diagnosebericht - die Oberflaeche laeuft auch ohne.
  testweb.aktualisierung_anbinden(&aktualisierung);
  if (aktualisierung.auf_bewaehrung())
    starwatch::konsole.println(
        "\nDIESES IMAGE LAEUFT AUF BEWAEHRUNG. Es wird erst bestaetigt, wenn\n"
        "WLAN steht, die Zeit gueltig ist UND eine Referenzfahrt gelaufen ist.\n"
        "Bis dahin faehrt der Bootloader bei einem Neustart die vorige Fassung.");
  starwatch::konsole.printf("Hall-Offset aus NVS: %ld\n", (long)betrieb.hall_offset());

  // Weboberflaeche ZULETZT: sie braucht alle Bibliotheken, und wenn das WLAN
  // nicht steht, soll der Rest trotzdem schon gelaufen sein.
  //
  // NACH EINEM BROWNOUT WIRD SIE NICHT GESTARTET. Das Einschalten des
  // Funkmoduls war die Ursache des Einbruchs; wuerde die Firmware es stur
  // wieder versuchen, brauecke sie sofort wieder ein - genau die
  // Neustartschleife, in der sich das Geraet nicht mehr flashen liess.
  //
  // Das ist keine Notloesung fuer den Aufbau, sondern eine Regel, die auch
  // fuer das Produkt gilt: ein Geraet darf sich nicht selbst in eine
  // Neustartschleife bringen. Es soll erreichbar bleiben - und sei es nur
  // ueber das Kabel -, damit man es reparieren kann.
  if (kam_von_brownout) {
    starwatch::konsole.println(
        "\nWLAN WIRD NICHT GESTARTET, weil der letzte Neustart ein Brownout war.\n"
        "Sonst bricht die Versorgung sofort wieder ein und das Geraet haengt in\n"
        "einer Neustartschleife. Alles andere laeuft: Motor, Hall, Licht, Uhr.\n"
        "Mit 'n' kann das WLAN von Hand versucht werden.");
  } else {
    testweb.beginn(&antrieb, &hall, &licht, &betrieb, kommando);
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
  // DIE REIHENFOLGE IST ABSICHT. Zuenden zuerst: die Zuendkette ist eine
  // Kette mit Zeitmarken und laeuft in `licht.tick()` weiter - und `pumpe()`,
  // das die Referenzfahrt am Leben haelt, ruft genau dieses tick(). Die
  // Klinge zuendet also, WAEHREND der Zeiger seine Null sucht, statt danach.
  licht.zuenden();

  // Zeit holen, BEVOR die Referenzfahrt blockiert: SNTP braucht ein paar
  // Sekunden, und die laufen dann waehrend der Referenzfahrt ab statt danach.
  ntp_starten();

  starwatch::konsole.println("\nReferenzfahrt beim Start. 's' bricht ab.");
  starwatch::Homingergebnis start_e;
  {
    Langlauf sperre;
    start_e = betrieb.referenzfahrt(pumpe);
  }
  starwatch::konsole.printf("%s\n", starwatch::hominggrund_text(start_e.grund));
  if (!start_e.ok())
    starwatch::konsole.println(
        "Ohne Referenz kennt die Uhr ihre Lage nicht - sie zeigt erst nach\n"
        "einem erfolgreichen 'h' die richtige Zeit. Ursachen nennt 'd'.");
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
void referenzfahrt_nachholen(const char *warum) {
  starwatch::konsole.printf("\n[auto] Referenzfahrt: %s\n", warum);
  homing_letzter_versuch_ms = millis();
  starwatch::Homingergebnis e;
  {
    Langlauf sperre;
    e = betrieb.referenzfahrt(pumpe);
  }
  abbruch_gewuenscht = false;
  if (e.ok()) {
    homing_fehlschlaege = 0;
    starwatch::konsole.printf("[auto] OK.\n> ");
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
    referenzfahrt_nachholen("letzte war nicht erfolgreich");
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
  referenzfahrt_nachholen("taeglich");
}

void loop() {
  konsole_lesen();
  testweb.tick();
  // Erst hier, nicht in setup(): kam das WLAN spaeter (Brownout-Fall, 'n',
  // 'ns'), soll die Zeit trotzdem noch geholt werden.
  ntp_starten();
  ntp_nachstellen();
  betrieb.tick();

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
          "\n[update] Fassung %s bestaetigt - WLAN, Zeit und Referenzfahrt\n"
          "         stimmen. Kein Rollback mehr.\n> ", g::FIRMWARE_VERSION);
  }

  betrieb.zyklus_tick();
  referenzfahrt_pflege();

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
