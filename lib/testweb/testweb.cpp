#include "testweb.h"

#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <esp_system.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <geraet.h>
#include <zeiger.h>

// Zugangsdaten sind optional: fehlt die Datei, laeuft das Geraet als eigener
// Zugangspunkt. __has_include statt eines harten #include, damit ein frisch
// geklontes Repository ohne weiteres Zutun baut.
#if __has_include("zugangsdaten.h")
#include "zugangsdaten.h"
#define STARWATCH_HAT_WLAN 1
#else
#define STARWATCH_HAT_WLAN 0
#define STARWATCH_AP_SSID "StarWatch-Test"
#define STARWATCH_AP_PASSWORT "unbenutzt-siehe-AP-6.2"
#endif

namespace starwatch {

namespace g = geraet;

Doppelausgabe konsole;

namespace {
WebServer server(80);
Testweb *aktiv = nullptr;

/// Zeiger auf den Betrieb fuer die freien Funktionen (Befugnispruefung). Die
/// Routen-Lambdas sind keine Member und kommen sonst nicht an das Token.
Betrieb *betrieb_zeiger = nullptr;

constexpr uint32_t WLAN_TIMEOUT_MS = 12000;

/// Zugangsdaten in NVS. Eigener Bereich, nicht der von lib/betrieb: nach
/// AP 3.1 wird der Speicher in "Fertigung" und "Benutzer" geteilt, und das
/// WLAN gehoert dem Benutzer.
const char *NVS_BEREICH = "sw_netz";
const char *NVS_SSID = "ssid";
const char *NVS_PASS = "pass";
// Feste Adresse (26.09.2026). Fehlt NVS_FEST oder ist es false: DHCP.
const char *NVS_FEST = "ip_fest";
const char *NVS_IP = "ip";
const char *NVS_MASKE = "maske";
const char *NVS_GW = "gw";
const char *NVS_DNS = "dns";
Preferences nvs;

/// Den WLAN-Speicher oeffnen, falls noch zu - von JEDER Stelle, die ihn
/// braucht, nicht nur aus beginn().
///
/// GEFUNDEN AM 27.09.2026 AM GERAET: der Werksreset ueber Stromzyklen laeuft
/// in setup() VOR testweb.beginn(). Er rief wlan_setzen("", "") auf einen
/// noch geschlossenen Speicher - Preferences::remove() gibt dann still false
/// zurueck. Die Uhr blitzte sechsmal, loeschte Helligkeit und Farbe, BEHIELT
/// ABER DAS WLAN und kam wieder im gesicherten Rettungsmodus hoch. Seit
/// 24.08.2026 so.
bool nvs_offen = false;
void nvs_oeffnen() {
  if (!nvs_offen)
    nvs_offen = nvs.begin(NVS_BEREICH, false);
}

/// Eine Netzmaske ist eine lueckenlose Reihe von Einsen von links - sonst ist
/// es keine. 0.0.0.0 und 255.255.255.255 sind fuer eine Uhr im Heimnetz
/// ebenfalls sinnlos.
bool maske_gueltig(const IPAddress &m) {
  const uint32_t w = (uint32_t(m[0]) << 24) | (uint32_t(m[1]) << 16) |
                     (uint32_t(m[2]) << 8) | uint32_t(m[3]);
  if (w == 0 || w == 0xFFFFFFFFu)
    return false;
  const uint32_t inv = ~w;
  return (inv & (inv + 1)) == 0;  // nur Einsen links, nur Nullen rechts
}

uint32_t als_zahl(const IPAddress &a) {
  return (uint32_t(a[0]) << 24) | (uint32_t(a[1]) << 16) | (uint32_t(a[2]) << 8) |
         uint32_t(a[3]);
}

/// Die gespeicherte Adresseinstellung anwenden - VOR jedem WiFi.begin().
/// Bei DHCP ausdruecklich zuruecksetzen: eine vorher gesetzte feste Adresse
/// bliebe sonst im Treiber stehen, bis zum naechsten Neustart.
void adresse_anwenden() {
  IPAddress ip, maske, gw, dns;
  if (nvs.getBool(NVS_FEST, false) && ip.fromString(nvs.getString(NVS_IP, "")) &&
      maske.fromString(nvs.getString(NVS_MASKE, "")) &&
      gw.fromString(nvs.getString(NVS_GW, ""))) {
    if (!dns.fromString(nvs.getString(NVS_DNS, "")))
      dns = gw;
    // DNS ist PFLICHT bei fester Adresse - am Pruefstand gelernt: ohne ihn
    // bleibt die NTP-Zeit ungueltig (siehe zugangsdaten.h).
    WiFi.config(ip, gw, maske, dns);
    konsole.printf("Feste Adresse %s / %s, Gateway %s, DNS %s\n", ip.toString().c_str(),
                   maske.toString().c_str(), gw.toString().c_str(), dns.toString().c_str());
  } else {
    WiFi.config(IPAddress(uint32_t(0)), IPAddress(uint32_t(0)), IPAddress(uint32_t(0)));
  }
}

// Das feste OTA-Passwort ist am 24.08.2026 entfallen (AP 6.2). Es kommt jetzt
// aus `betrieb_->ota_passwort()` und wird beim ersten Start je Geraet
// gewuerfelt - siehe die Begruendung bei der Geraeteidentitaet in betrieb.h.

// ===========================================================================
// WLAN-AUFSICHT (24.08.2026)
// ===========================================================================
// Der Arduino-Kern verbindet von SELBST wieder: `_autoReconnect` steht im
// Konstruktor auf true, und `_is_staReconnectableReason()` zaehlt sowohl
// BEACON_TIMEOUT als auch NO_AP_FOUND als Grund zum Wiederholen - ein
// Router-Neustart wird also aufgeholt, ohne dass diese Firmware etwas tut.
// GEPRUEFT im Kern unter libraries/WiFi/src/STA.cpp, nicht angenommen.
//
// WAS TROTZDEM FEHLTE, sind drei Dinge, die der Kern nicht tun kann:
//
//   1 SICHTBARKEIT. `verbunden_` wurde beim Verbinden gesetzt und nie wieder
//     geloescht. Fiel das WLAN aus, behauptete die Anzeige weiter, alles sei
//     in Ordnung - und niemand haette gemerkt, dass die Uhr seit Tagen ohne
//     Zeitkorrektur laeuft.
//   2 NTP NACH DER RUECKKEHR. SNTP stellt nur in seinem eigenen Takt nach
//     (Vorgabe eine Stunde). Nach einem langen Ausfall soll die Zeit SOFORT
//     wieder stimmen, nicht irgendwann.
//   3 EIN NACHSTOSS. Falls der Kern doch aufgibt - ein Grund ausserhalb
//     seiner Liste -, waere das Geraet bis zum Neustart offline. Alle 30 s
//     ein `WiFi.reconnect()` kostet nichts und deckt diesen Fall ab.
//
// NICHT ANGETASTET wird `verbunden_` selbst: der eigene Zugangspunkt laeuft
// unter demselben Schalter, und der Verlust des Heimnetzes darf nicht den
// Server am Zugangspunkt mitnehmen.

/// 0 = Heimnetz steht. Sonst der Zeitpunkt, seit dem es fehlt.
uint32_t wlan_weg_seit = 0;
uint32_t wlan_ausfaelle = 0;  ///< wie oft das Heimnetz seit dem Start weg war
uint32_t wlan_letzter_nachstoss = 0;
bool wlan_wieder_da = false;
constexpr uint32_t WLAN_NACHSTOSS_MS = 30000;

/// Laeuft gerade ein Update? Dann bedient der Webserver NIEMANDEN.
///
/// AM GERAET GEMESSEN, 25.08.2026: mit geoeffneter Oberflaeche brach das OTA
/// dreimal hintereinander mit "Empfang abgebrochen" ab; mit geschlossener
/// Seite lief es auf Anhieb durch. Die Oberflaeche fragt im Sekundentakt
/// `/test/zustand` ab - jede dieser Anfragen belegt einen Socket und Speicher
/// neben dem Update-Strom, und der ESP32 hat von beidem wenig.
///
/// DAS TRIFFT AUCH DIE AUSGELIEFERTE UHR: das naechtliche Update laedt 1,3 MB
/// ueber HTTPS. Steht dabei irgendwo ein Browser auf der Seite offen, kaeme
/// dasselbe heraus - nur wuerde es niemand sehen, weil es nachts passiert.
bool update_laeuft = false;
/// Was vor einem Entwickler-OTA lief - damit ein ABGEBROCHENES Update die
/// Uhr nicht dunkel und stehend zuruecklaesst (28.09.2026, am Geraet so
/// passiert: drei abgerissene Uploads, danach Klinge aus und kein
/// Uhrbetrieb; die Lichtautomatik schaltet nur bei einem Wechsel hell/dunkel
/// und sprang deshalb nicht ein).
bool ota_licht_war_an = false;
Zeitmodus ota_modus_vorher = Zeitmodus::aus;

/// Verzoegerter Neustart. WARUM VERZOEGERT: `ESP.restart()` im Handler bricht
/// die HTTP-Antwort ab, und der Browser zeigt einen Fehler statt der Meldung.
/// Erst antworten, dann in `tick()` neu starten.
uint32_t neustart_ab_ms = 0;
bool neustart_mit_reset = false;
constexpr uint32_t NEUSTART_FRIST_MS = 2000;

/// Log ueber TCP. Ein Zuhoerer genuegt.
WiFiServer log_server(Testweb::LOG_PORT);
WiFiClient log_client;
size_t log_gesendet = 0;
/// Hat sich der Zuhoerer am Log-Port mit dem Token angemeldet? Faellt mit
/// jeder neuen Verbindung zurueck.
bool log_befugt = false;

// ===========================================================================
// EIGENER NAME AM ZUGANGSPUNKT: http://star.watch
// ===========================================================================
// So macht es AVM mit fritz.repeater, und es funktioniert aus demselben
// Grund: **im eigenen WLAN ist das Geraet selbst der DNS-Server.** Es ist
// DHCP-Server, gibt sich als DNS an, und beantwortet jede Anfrage mit seiner
// eigenen Adresse.
//
// WICHTIG UND EHRLICH: das gilt NUR am eigenen Zugangspunkt. Im Heimnetz ist
// der Router der DNS-Server, und dem kann man von hier keinen Namen
// beibringen - fritz.box klappt nur, weil AVM auch den Router stellt. Fuer
// das Heimnetz gibt es deshalb mDNS: starwatch.local.
//
// Der Catch-all-DNS hat einen Preis, den ein Captive Portal immer hat: ein
// Geraet, das an diesem Zugangspunkt haengt, kommt NICHT ins Internet. Das
// ist erwartetes Verhalten und der Grund, warum der Zugangspunkt nach dem
// Einrichten verschwindet.
const char *EIGENER_NAME = "star.watch";
const char *MDNS_NAME = "starwatch";  // ergibt starwatch.local
DNSServer dns;
bool dns_laeuft = false;

/// Sendeleistung. VOLL (19,5 dBm) seit 28.09.2026.
///
/// Die 11 dBm davor stammten von der Alpha B (22.08.2026): dort loeste der
/// Brownout-Detektor beim Einschalten des Funkmoduls aus - acht Neustarts in
/// acht Sekunden, und der Chip liess sich in der Schleife nicht mehr flashen.
/// Die Ursache war deren Versorgung. Auf der heutigen Platine ist das nie
/// gemessen worden, und 11 dBm schwaechen genau die Richtung Uhr -> AP, die
/// OTA-Uploads traegt.
///
/// DIE SICHERUNG BLEIBT, NUR ABGESTUFT: startet die Uhr nach einem Brownout,
/// ruft setup() funk_drosseln(), und dieser Start funkt mit 11 dBm. Erst ein
/// ZWEITER Brownout in Folge sperrt das WLAN ganz (brownout_folge in main.cpp).
/// So bleibt die Uhr per OTA erreichbar, selbst wenn die volle Leistung auf
/// einem Exemplar doch zu viel waere.
constexpr wifi_power_t SENDELEISTUNG_VOLL = WIFI_POWER_19_5dBm;
constexpr wifi_power_t SENDELEISTUNG_GEDROSSELT = WIFI_POWER_11dBm;

// ---------------------------------------------------------------------------
// FUNK-STROMSPAREN, DER MITTELWEG (01.10.2026)
// ---------------------------------------------------------------------------
// Ohne Stromsparen haelt der Funk rund 80 mA dauernd - ohne Licht neben dem
// Haltestrom der groesste Verbraucher. MIT Stromsparen (Modem-Sleep) brachen
// am 28.09.2026 OTA-Uploads bei 40 % ab, und die Oberflaeche wurde traege.
// Deshalb: wach, solange jemand da ist - jede Anfrage, ein Telnet-Client,
// jedes Update -, und erst FUNK_WACH_MS nach der letzten Bedienung schlafen.
// MQTT und NTP laufen im Schlaf weiter, nur mit etwas mehr Verzoegerung.
constexpr uint32_t FUNK_WACH_MS = 5u * 60u * 1000u;
uint32_t letzte_bedienung_ms = 0;
bool funk_schlaeft = false;
// Fuer die Diagnose: wie lange er geschlafen hat. Von aussen ist das nicht zu
// sehen - jede Anfrage weckt ihn, bevor sie beantwortet wird.
uint32_t funk_schlaf_ab_ms = 0;
uint32_t funk_letzter_schlaf_ms = 0;
uint64_t funk_schlaf_summe_ms = 0;

void funk_wach(bool wach) {
  if (wach != funk_schlaeft)
    return;  // ist schon so
  WiFi.setSleep(!wach);  // true = WIFI_PS_MIN_MODEM
  funk_schlaeft = !wach;
  if (!wach) {
    funk_schlaf_ab_ms = millis();
  } else {
    funk_letzter_schlaf_ms = millis() - funk_schlaf_ab_ms;
    funk_schlaf_summe_ms += funk_letzter_schlaf_ms;
  }
}
wifi_power_t SENDELEISTUNG = SENDELEISTUNG_VOLL;

/// Funk einstellen - VOR jedem WiFi.begin(), und zwar alles an einer Stelle.
///
/// DEN STAERKSTEN ZUGANGSPUNKT NEHMEN (28.09.2026). Die Arduino-Vorgabe ist
/// WIFI_FAST_SCAN: verbunden wird mit dem ERSTEN Zugangspunkt, der den
/// Netznamen traegt - nicht mit dem naechsten. Am Geraet gemessen: der AP
/// neben der Uhr kam mit -36 dBm an, verbunden war sie mit einem entfernten
/// bei -79 dBm, und dort blieb sie. Die Folge waren Aussetzer von einigen
/// Sekunden und zaehe OTA-Uploads. WIFI_ALL_CHANNEL_SCAN sucht alle Kanaele
/// ab, WIFI_CONNECT_AP_BY_SIGNAL waehlt danach den staerksten. Die Einstellung
/// steht im Treiber und gilt damit auch fuer jedes spaetere reconnect().
void funk_einstellen() {
  WiFi.setTxPower(SENDELEISTUNG);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  // KEIN STROMSPAREN DES FUNKTEILS - auf JEDEM Weg (28.09.2026). Bis dahin
  // stand setSleep(false) nur im Zweig fuer zugangsdaten.h; eine Uhr mit
  // WLAN aus der Oberflaeche (NVS) schlief zwischen den Beacons. Gemessen:
  // Ping Mac->Uhr 27-100 ms bei -38 dBm, Mac->Router 4 ms - und OTA-Uploads
  // rissen bei 40 % ab ("Empfang abgebrochen"). Die Uhr haengt am Netzteil,
  // Strom zu sparen hat hier keinen Wert.
  // 01.10.2026: Doch - aber nur, wenn niemand da ist (funk_wach, oben). Hier,
  // beim Verbinden, immer wach.
  WiFi.setSleep(false);
  funk_schlaeft = false;
}

/// Die Oberflaeche. Eine Seite, aus dem Flash.
///
/// Die Gruppierung ist die des Pruefstands, weil sie sich dort bewaehrt hat:
/// Notbremsen oben, dann Licht, Uhr, Timer, Referenzfahrt, Hall, Motor,
/// Diagnose. Der Umsetzungsplan nennt sie ausdruecklich als brauchbare
/// Vorlage fuer AP 3.5.
const char SEITE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Star Watch</title><style>
/* DAS AUSSEHEN FOLGT DEN VORLAGEN IN 04_Firmware/UI:UX Design.
   Fuenf Regeln, und jede davon wurde vorher verletzt:

     1  SCHWARZ UND WEISS, KEINE GRAUSTUFEN. Was dunkler wirkt, ist Weiss mit
        weniger Deckkraft. Eine graue Flaeche bliebe eine Flaeche.
     2  NUR UMRISSE. Karten, Knoepfe, Schalter und Regler haben keine
        Fuellung - in den Vorlagen gibt es keine gefuellten Felder.
     3  LINIEN LEUCHTEN. Was aktiv ist, bekommt einen Schein, keine Farbe.
     4  EIN KNOPFSTIL. Zustand zeigt der Schein, nicht eine zweite Farbe.
     5  EIN REGLER IST EINE LINIE MIT TEILSTRICHEN UND EINEM PUNKT, kein
        Balken mit gefuellter Bahn.

   Vom shadcn-Preset b27GcrRo bleibt damit die Idee, nicht die Leiter: dessen
   neutrale Reihe (#0a0a0a bis #fafafa) waere genau die Graustufenleiter, die
   Regel 1 ausschliesst. Uebernommen ist der Grundgedanke - fast schwarzer
   Grund, Monospace, sehr zurueckhaltende Rahmen. Die Ecken sind eckig statt
   0,625rem: die Vorlagen haben keine Rundungen.

   FARBE IST ZUSTAND, nicht Schmuck. Gruen, Orange und Rot sagen etwas ueber
   die Uhr; die Bedienung selbst ist weiss. Das Rot der Klinge ist dasselbe
   Rot wie das der Fehler - es ist dieselbe Klinge. */
:root{
 /* KEINE GRAUSTUFEN. Was dunkler wirkt, ist Weiss mit weniger Deckkraft. */
 --bg:#000;
 --w1:#fff;                    /* volle Linie, Text */
 --w2:rgba(255,255,255,.78);   /* zweite Ebene */
 --w3:rgba(255,255,255,.55);   /* Beschriftungen */
 --w4:rgba(255,255,255,.34);   /* Rahmen */
 --w5:rgba(255,255,255,.20);   /* Trennlinien */
 /* Akzente NUR fuer Zustaende, nie fuer Bedienelemente. */
 --ok:#2bff88; --warn:#ff9d2b; --err:#ff2f2f; --info:#3ad4ff;
 --gl:0 0 6px rgba(255,255,255,.45);

 /* ===================================================================
    DIE ABSTANDSLEITER. VIER WERTE, SONST KEINE.
    Jede Luecke auf dieser Seite ist einer davon - es gibt keine
    Sonderfaelle, keine 9, 11, 13 oder 17 px. Wer einen Abstand aendern
    will, aendert hier eine Zahl und die ganze Seite folgt.
       s1  innerhalb einer Zeile, zwischen zusammengehoerigen Dingen
       s2  zwischen Bedienelementen einer Zeile
       s3  DER GRUNDABSTAND: Kartenpolster, Zeilenabstand, Kachelluecke
       s4  zwischen Abschnitten
    =================================================================== */
 --s1:4px; --s2:8px; --s3:16px; --s4:24px;
}
*{box-sizing:border-box}
/* Alles, was von aussen Abstand mitbringt, wird zurueckgesetzt. Sonst
   addiert sich der Eigenabstand eines Knopfes oder Feldes zum Zeilenabstand
   und nichts steht mehr auf der Leiter. */
h1,h2,p,pre,table,input,select,button,label,svg{margin:0}
/* Das SVG darf ueber seinen viewBox hinaus malen - sonst schneidet es den
   Hof der Klinge an der eigenen Kante ab. */
svg{overflow:visible}
/* Der Abstand des Zifferblatts steht im Inline-Stil des SVG, nicht hier:
   das Element traegt dort bereits margin fuer die Zentrierung, und Inline
   schlaegt jeden Stilblock. Genau diese Falle hat hier schon einmal eine
   halbe Stunde gekostet.
   Dort steht margin:auto, und zwar RINGSUM. Die Kachel spannt ueber zwei
   Rasterzeilen und ist darum hoeher als ihr Inhalt; ohne die beiden
   senkrechten auto nahm die letzte Zeile den ganzen Ueberschuss und die Uhr
   sass sichtbar zu hoch. Mit auto oben UND unten ist der Abstand ueber der
   Uhr immer so gross wie der darunter - unabhaengig davon, wie stark die
   Kachel gestreckt wird. Ein fester Wert kann das nicht leisten. */

body{margin:0;padding:var(--s3);background:var(--bg);color:var(--w1);
font:12px/1.55 "Geist Mono",ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,
"Liberation Mono",monospace;letter-spacing:.03em}
/* KEIN PUNKTRASTER IM HINTERGRUND. Hier lag ein Muster aus 0,5-px-Punkten
   im 4-px-Abstand - entfernt am 25.09.2026: der Grund ist schwarz, sonst
   nichts (Regel 1). */
h1{font-size:14px;letter-spacing:.24em;text-transform:uppercase;font-weight:400;
 text-shadow:var(--gl)}
h2{font-size:10px;text-transform:uppercase;letter-spacing:.26em;color:var(--w3);
 font-weight:400}
/* Eckige Klammern um jede Kartenueberschrift - dasselbe Zeichen wie bei den
   Kennungen, und das Erkennungsmerkmal der Vorlagen. */
.k h2::before,details.gross > summary::before{content:"[ "}
.k h2::after,details.gross > summary::after{content:" ]"}
.top{display:flex;justify-content:space-between;align-items:baseline;
 gap:var(--s2);flex-wrap:wrap;margin-bottom:var(--s3)}
.net{color:var(--w3);font-size:10px;letter-spacing:.14em;text-transform:uppercase}
/* Eckige Klammern um Kennungen, als ::before/::after - beim Kopieren wandern
   sie dann nicht mit. */
.kl{color:var(--w2)}
.kl::before{content:"[ ";color:var(--w3)}
.kl::after{content:" ]";color:var(--w3)}
/* Position und Zustand gehoeren zusammen und stehen enger beieinander als die
   uebrigen Zeilen. Die UEBERSCHRIFT steht ausserhalb - zu ihr gilt der
   normale Kartenabstand wie in jeder anderen Karte auch. */
.kopf{display:flex;flex-direction:column;gap:var(--s1)}
/* Dieselbe Groesse wie die Ueberschrift darueber - die Zahl ist eine Angabe,
   keine Schlagzeile. */
.zahl{font-size:10px;letter-spacing:.16em;color:var(--w1);
 text-shadow:0 0 8px rgba(255,255,255,.35);font-variant-numeric:tabular-nums}
/* Was die Uhr GERADE tut. Dieselbe Groesse wie die Beschriftungen, damit die
   Zahl darueber die Hauptsache bleibt. */
.lage{font-size:10px;text-transform:uppercase;letter-spacing:.16em;color:var(--w3)}

.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:var(--s3)}
/* Karten: kein Hintergrund, nur ein Rahmen - und RINGSUM derselbe Abstand. */
.k{background:transparent;border:1px solid var(--w4);padding:var(--s3);
 display:flex;flex-direction:column;gap:var(--s3)}
/* NUR DIE LETZTE ZEILE DEHNT SICH. Kacheln einer Rasterzeile sind gleich
   hoch - der Ueberschuss muss irgendwo hin und geht ueber die letzte Zeile,
   nicht unter sie.
   Der Grundabstand kommt aus dem gap der Karte und bleibt davon unberuehrt:
   frueher trug jedes Element seinen eigenen margin-top, den auto beim letzten
   ueberschrieb - ohne Ueberschuss wurde daraus null, und die Trennlinie klebte
   am Text darueber. */
.k > *:last-child{margin-top:auto}
/* ZIFFERBLATT GENAU IN DER MITTE DER KARTE (27.09.2026), waagrecht und
   senkrecht - nicht mehr im Rest zwischen Kopf und Legende. Der Kopf ist
   hoeher als die Legende, und die Uhr sass deshalb sichtbar zu tief.
   Absolut gesetzt, damit Kopf und Legende sie nicht verschieben. Die GROESSE
   folgt der Karte (Container-Einheiten): der Strichkranz (Radius 96 von 200)
   nimmt rund 57 % der Kartenhoehe ein und nie mehr als 86 % der Breite.
   Die Karte misst dafuer ihren Inhalt nicht mit (container-type:size) - ihre
   Hoehe kommt am Desktop aus der rechten Spalte (sie spannt zwei Zeilen), auf
   dem Handy aus dem Seitenverhaeltnis. 3:4 laesst ueber und unter der Uhr
   genug Platz fuer Kopf und Legende.
   Vorher stand am Desktop "> .k:first-child > svg{max-width:340px}" - das
   erste Kind ist aber die Ueberschrift, die Regel griff also nie. */
.k.blatt{position:relative;container-type:size;aspect-ratio:3/4}
#uhr{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);
 display:block;width:min(63cqh,86cqw)}
@media(min-width:760px){
 .g.uhr{grid-template-columns:1fr 1fr}
 .g.uhr > .k:first-child{grid-row:span 2}
 /* min-height faengt eine kurze rechte Spalte ab. */
 .g.uhr > .k.blatt{aspect-ratio:auto;min-height:460px}
}

/* EIN Knopfstil. Zustand zeigt der Schein, nicht Farbe oder Flaeche. */
button{background:transparent;color:var(--w2);border:1px solid var(--w4);border-radius:0;
 padding:var(--s2) var(--s3);font:inherit;font-size:10px;letter-spacing:.16em;
 text-transform:uppercase;cursor:pointer;
 transition:color .12s,border-color .12s,box-shadow .12s}
/* KEINE BESCHRIFTUNG UEBER DEN RAND (27.09.2026). Ein einzelnes langes Wort
   wie "Werkseinstellungen" kann nicht umbrechen und lief im halben Knopf ueber
   den Rahmen hinaus. In drei Stufen, die erste steht bei .grp:
     1 in schmalen Gruppen enger setzen (Buchstabenabstand, Polster),
     2 an einer Silbe trennen (hyphens, lang="de"),
     3 erst dann an beliebiger Stelle umbrechen - nie ueber den Rand.
   Gilt fuer JEDEN Knopf und fuer den Dateiknopf (.dat), der wie einer
   aussieht. */
button,.dat{min-width:0;overflow-wrap:break-word;-webkit-hyphens:auto;hyphens:auto}
button:hover{color:var(--w1);border-color:var(--w1);box-shadow:var(--gl)}
button:active{transform:translateY(1px)}
button.gruen,button.gruen .tr{color:var(--ok);border-color:var(--ok)}
button.an{color:var(--w1);border-color:var(--w1);box-shadow:var(--gl);text-shadow:var(--gl)}
/* Dateiknopf: das native <input type=file> bringt eine eigene Flaeche und
   Schrift mit und laesst sich nicht formen - also versteckt, Label davor. */
/* MITTIG WIE EIN KNOPF (27.10.2026): ein Label richtet seinen Text sonst
   links oben aus - "Waehlen" sass zwischen Export und Import schief. */
.dat{display:inline-flex;align-items:center;justify-content:center;text-align:center;
 box-sizing:border-box;border:1px solid var(--w4);padding:var(--s2) var(--s3);
 font-size:10px;letter-spacing:.16em;text-transform:uppercase;color:var(--w2);
 cursor:pointer;transition:color .12s,border-color .12s,box-shadow .12s}
.dat:hover{color:var(--w1);border-color:var(--w1);box-shadow:var(--gl)}
.dat input{display:none}
a{text-decoration:none}

/* :not([type=range]) - der Regler ist ein Eingabefeld und bekaeme sonst die
   Unterkante der Textfelder mit. Genau die stand als zweiter Strich unter
   jedem Regler. */
input:not([type=range]),select{background:transparent;color:var(--w1);border:0;
 border-bottom:1px solid var(--w4);border-radius:0;padding:var(--s2) 0;
 font:inherit;font-size:11px;width:80px}
/* Der Pfeil des Betriebssystems sitzt hart am Rand und laesst sich nicht
   verschieben. Deshalb appearance:none und ein eigener Pfeil als Hintergrund
   - so ist sein Abstand nach rechts genau derselbe wie oben und unten. */
select{border:1px solid var(--w4);padding:var(--s2);
 padding-right:calc(var(--s2)*2 + 10px);
 -webkit-appearance:none;appearance:none;
 background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 6'%3E%3Cpath d='M1 1l4 4 4-4' fill='none' stroke='%23ffffff' stroke-opacity='.55' stroke-width='1.3'/%3E%3C/svg%3E");
 background-repeat:no-repeat;background-position:right var(--s2) center;
 background-size:10px 6px}
/* :not([type=range]) - ein Regler hat keine Unterkante, die sich hervorheben
   liesse. Ohne diese Ausnahme zog der Fokus eine weisse Linie unter jeden
   Regler, sobald man ihn anfasste. */
input:not([type=range]):focus,select:focus{outline:none;border-color:var(--w1);
 box-shadow:0 1px 0 0 var(--w1)}

table{width:100%;border-collapse:collapse;font-size:11px}
td{padding:var(--s1) 0;vertical-align:top}
td:first-child{color:var(--w3);white-space:nowrap;padding-right:var(--s3);
 text-transform:uppercase;letter-spacing:.14em;font-size:10px}
.v{font-variant-numeric:tabular-nums}
.ok{color:var(--ok);text-shadow:0 0 7px rgba(43,255,136,.6)}
.warn{color:var(--warn);text-shadow:0 0 7px rgba(255,157,43,.6)}
.err{color:var(--err);text-shadow:0 0 7px rgba(255,47,47,.6)}
pre{background:transparent;border:1px solid var(--w5);padding:var(--s3);
 font:inherit;font-size:10px;line-height:1.5;max-height:260px;overflow:auto;
 white-space:pre-wrap;color:var(--w3)}
.hint{color:var(--w3);font-size:10px;line-height:1.7;letter-spacing:.04em}
.hint b{color:var(--w2);font-weight:400}

/* Zeilen: EIN Abstand nach oben, EIN Abstand zwischen den Dingen darin.
   Erste und letzte Zeile stossen buendig an das Kartenpolster. */
.row{display:flex;align-items:center;gap:var(--s2);flex-wrap:wrap}
/* Knoepfe in einer Karte teilen sich die volle Breite - wie die Regler
   darueber. Ein einzelner Knopf nimmt sie ganz, zwei je die Haelfte. In der
   Leiste NICHT: dort stehen sie neben Schalter, Regler und Uhrzeit. */
.k .row button,.k .row .dat{flex:1}
/* Auswahlfelder und Eingaben stehen wie die Regler in einer eigenen Zeile:
   Beschriftung darueber, das Feld darunter ueber die ganze Breite. */
.k .sl select,.k .sl input:not([type=range]){width:100%;max-width:none}
.k .sl{width:100%}
.row label{color:var(--w3);min-width:80px;font-size:10px;text-transform:uppercase;
 letter-spacing:.16em}
details{border-top:1px solid var(--w5);padding-top:var(--s3)}
/* DER INHALT EINES AUFGEKLAPPTEN BEREICHS bekommt denselben Zeilenabstand
   wie die Karte (28.09.2026). Die Karte verteilt ihre Kinder mit gap - in
   ein <details> hinein reicht das nicht, und Ueberschrift, Regler und
   Hinweis klebten aneinander ("Schwellen" in der Automatik). */
details > :not(summary){margin-top:var(--s3)}
summary{color:var(--w3);font-size:10px;cursor:pointer;text-transform:uppercase;
 letter-spacing:.14em}
summary:hover{color:var(--w1);text-shadow:var(--gl)}
details.gross{border:1px solid var(--w4);padding:var(--s3);background:transparent;
 margin-top:var(--s3)}
details.gross > summary{font-size:10px;letter-spacing:.26em;color:var(--w3)}

/* ---- Leiste ---------------------------------------------------------- */
/* Die drei Meldekaesten haben dasselbe Polster und denselben Abstand wie
   jede Karte - nur die Randfarbe unterscheidet sie. */
.kasten{border:1px solid var(--w4);padding:var(--s3);margin-bottom:var(--s3)}
.bar{position:sticky;top:0;z-index:20;background:var(--bg);border:1px solid var(--w4);
 padding:var(--s3);margin-bottom:var(--s3);display:flex;flex-wrap:wrap;
 gap:var(--s2) var(--s4);align-items:center}

/* In der Leiste darf die Lichtgruppe wachsen - Uhrzeit und Reiter stehen
   dahinter mit margin-left:auto und werden davon nicht verschoben. Vorher war
   der Regler auf 120 px festgenagelt und wirkte abgeschnitten. */
.bar .sl{flex:1;min-width:200px}
/* FARBE: drei Regler, einer je Kanal. Dieselbe Linie mit
   Teilstrichen wie jeder andere Regler - nur der PUNKT traegt die Farbe
   seines Kanals. Hier ist die Farbe der Inhalt, nicht Schmuck; Linie,
   Striche und Beschriftung bleiben weiss wie ueberall. */
#k_farbe .sl input[type=range]::-webkit-slider-thumb{background:var(--pk);
 box-shadow:0 0 8px var(--pk)}
#k_farbe .sl input[type=range]::-moz-range-thumb{background:var(--pk);
 box-shadow:0 0 8px var(--pk)}
/* Farbpunkt in den Vorwahlknoepfen und vor dem Farbwert: der Knopf bleibt
   ein Umriss, nur der Punkt traegt die Farbe. */
.fp{display:inline-block;width:7px;height:7px;border-radius:50%;
 margin-right:var(--s1);vertical-align:1px}
/* Vier Knoepfe in einer Zeile auch auf dem Handy: Punkt und Wort bleiben
   zusammen, das Polster wird schmaler statt dass der Punkt ueber das Wort
   rutscht. */
#k_farbe .grp button{white-space:nowrap;padding:var(--s2) var(--s1)}
/* SECHS Vorwahlen in EINER Reihe (27.09.2026, vorher 3 x 2). Damit auf einer
   schmalen Karte kein Knopf aus der Karte ragt, faellt dort das Wort weg und
   der Punkt wird groesser - die Farbe IST die Beschriftung. Gemessen wird die
   KARTE, nicht das Fenster: auf dem Desktop kann sie genauso schmal sein. */
#k_farbe{container-type:inline-size}
#k_farbe .grp.fa5{grid-auto-flow:row;grid-template-columns:repeat(6,1fr)}
@container (max-width:540px){
 #k_farbe .grp.fa5 .fw{display:none}
 #k_farbe .grp.fa5 .fp{width:12px;height:12px;margin-right:0;vertical-align:-1px}
}
.uhrzeit{font-size:15px;letter-spacing:.12em;font-variant-numeric:tabular-nums;
 text-shadow:0 0 10px rgba(255,255,255,.35)}

/* Schalter und Regler tragen ihre Beschriftung selbst und in gleicher Breite.
   Beide Zeilenformen sind gleich gebaut: Beschriftung, Wert, Bedienelement. */
.tg,.sl{display:flex;align-items:center;gap:var(--s2);font-size:10px;
 color:var(--w3);text-transform:uppercase;letter-spacing:.16em}
/* IN DEN KARTEN steht der Regler in einer eigenen Zeile: darueber
   Beschriftung und Wert, getrennt durch //, darunter die Linie ueber die
   ganze Breite. In der Leiste bleibt alles nebeneinander - dort ist fuer
   zwei Zeilen kein Platz. */
.k .sl{flex-direction:column;align-items:stretch;gap:var(--s2)}
/* Die Kopfzeile gilt ueberall - in der Karte ueber dem Regler, in der Leiste
   davor. Nur die Anordnung unterscheidet sich. */
.sl .kz{display:flex;align-items:baseline;gap:var(--s2)}
.sl .kz .tr{color:var(--w4)}
button .tr{color:var(--w4)}
button .ok,button .warn,button .err{text-shadow:none}
/* In der Kopfzeile steht der Wert direkt hinter dem Trennzeichen. Feste
   Breite und Rechtsbuendigkeit stammen aus der alten, einzeiligen Form und
   rissen dort eine Luecke auf. */
.k .sl .kz .w{min-width:0;text-align:left}
/* Die Reglerzeile nimmt die ganze Kartenbreite - erst dadurch kann der
   Regler darin den Rest fuellen. */
.sl{flex:1}
.row > .sl{width:100%}
.tg{cursor:pointer;user-select:none}
/* Feste Breiten NUR in den Karten - dort stehen die Regler dadurch
   untereinander buendig. In der Leiste sind sie falsch: sie reissen die
   Gruppen auseinander, und "Betrieb" gehoert an seinen Schalter. */
/* KEINE feste Breite. Der Regler soll nicht untereinander buendig stehen,
   sondern denselben ABSTAND zu seiner Beschriftung haben wie ueberall - eine
   Mindestbreite riss bei kurzen Woertern eine Luecke auf. Die Regler sind
   dadurch verschieden lang, und das ist richtig so. */
.tg .lb,.sl .lb{color:var(--w3)}
.k .sl .w{min-width:66px}
/* Der Schalter steht VOR seinem Text - dadurch stehen die Schalter zweier
   Zeilen ohnehin untereinander, ganz gleich wie lang die Woerter sind. Die
   Beschriftung braucht keine feste Breite mehr. */
.row > .tg{flex:1}
/* Der Wert steht RECHTSBUENDIG an der Kartenkante. Links ausgerichtet endeten
   die Werte je nach Laenge an verschiedenen Stellen und die rechte Kante war
   nirgends abzulesen. */
.sl .w{color:var(--w1);font-variant-numeric:tabular-nums;letter-spacing:.1em;
 text-align:right}
.tg input{position:absolute;opacity:0;width:0;height:0}
/* Der Schalter: eine Linie mit zwei Endmarken und einem Punkt darauf. */
/* DER SCHALTER HAT DAS GEWICHT EINES KNOPFES. Vorher war er eine nackte
   Linie und ging neben den umrandeten Knoepfen unter - jetzt steht er in
   einem Kasten mit demselben Rahmen und derselben Hoehe. Darin bleibt die
   Sprache der Vorlagen: eine Linie mit zwei Endmarken und ein Punkt darauf. */
.tg .sw{position:relative;flex:none;width:58px;height:34px;
 border:1px solid var(--w4);
 background:linear-gradient(var(--w4),var(--w4))
  center/calc(100% - 2*var(--s2)) 1px no-repeat;
 transition:border-color .16s,box-shadow .16s}
.tg .sw::before,.tg .sw::after{content:"";position:absolute;top:11px;width:1px;
 height:11px;background:var(--w4)}
.tg .sw::before{left:var(--s2)}
.tg .sw::after{right:var(--s2)}
/* DER PUNKT SITZT AUF DER LINIE, nicht daneben: top:50% mit translateY(-50%)
   trifft die Mitte unabhaengig von Rahmen und Hoehe. Feste Pixelwerte waren
   um einen Punkt daneben, weil der Rahmen zaehlt und der Hintergrund nicht.
   UND ER IST UNDURCHSICHTIG: var(--w3) ist Weiss mit 55 % Deckkraft, die
   Linie schien hindurch. Der eigene schwarze Grund plus derselbe Ton als
   Innenschatten ergibt denselben Farbton, aber deckend. */
.tg .pt{position:absolute;top:50%;left:var(--s2);width:8px;height:8px;
 border-radius:50%;background:var(--bg);box-shadow:inset 0 0 0 4px var(--w3);
 transform:translateY(-50%);
 transition:transform .16s,box-shadow .16s}
/* DER RAHMEN BLEIBT IMMER GRAU. Gruen wird nur der Punkt - er ist die
   Aussage, nicht der Kasten. */
.tg input:checked ~ .sw .pt{transform:translate(32px,-50%);
 box-shadow:inset 0 0 0 4px var(--ok),0 0 9px var(--ok)}
.tg input:focus-visible ~ .sw{outline:1px solid var(--w1);outline-offset:2px}
/* Der Regler: dieselbe Hoehe wie der Schalter, damit beide Zeilenformen
   gleich hoch sind - sonst sitzt der eine hoeher als der andere. */
/* DIE TEILSTRICHE MUESSEN AUF DEN RASTEN SITZEN, auf denen der Punkt
   einrastet - sonst steht "3" neben der Marke statt darauf.
   Der Punkt wandert nicht ueber die volle Breite: sein Mittelpunkt laeuft von
   4 px bis Breite minus 4 px, weil er selbst 8 px breit ist. Das Strichmuster
   bekommt deshalb dieselbe Spanne (background-size 100% - 8px, um 4 px nach
   rechts gesetzt) und als Teilung --n, die Zahl der Stufen des Reglers.
   Erster und letzter Strich sind damit zugleich die Endmarken - vorher fehlte
   die rechte, und die Linie lief ins Leere. */
/* DER REGLER FUELLT DIE ZEILE. Er bekommt keine feste Breite mehr, sondern
   den Platz, der nach Beschriftung und Wert uebrig bleibt - in jeder Karte
   also so viel, wie dort ist.

   DIE MARKEN, und warum das nicht trivial ist: der Punkt ist 8 px breit,
   sein Mittelpunkt laeuft daher nur von 4 px bis Breite minus 4 px. Alle
   Marken muessen auf DIESER Strecke liegen, nicht auf der vollen Breite.
     - Die beiden Endmarken werden ausdruecklich gezeichnet. Ueberliesse man
       sie dem Wiederholungsmuster, fiele die rechte aus: sie begaenne genau
       an der Kante des bemalten Bereichs und haette keine Breite mehr.
     - Das Muster zeichnet deshalb NUR die Marken dazwischen. Es beginnt eine
       Teilung spaeter und endet eine frueher - sonst laege es auf den
       Endmarken und die waeren doppelt so dick wie der Rest. */
.sl input[type=range]{-webkit-appearance:none;appearance:none;flex:1;width:auto;
 min-width:60px;height:var(--s3);display:block;cursor:pointer;border:0;--n:4;
 --sp:calc(100% - 8px);            /* Strecke, die der Punkt zuruecklegt */
 background:
  /* Grundlinie ueber die Strecke des Punktes */
  linear-gradient(var(--w4),var(--w4)) 4px center/var(--sp) 1px no-repeat,
  /* Endmarken. Die RECHTE muss ausdruecklich sein: im Muster darunter faellt
     sie aus, weil sie genau an der Kante der Verlaufsflaeche begaenne und
     dort keine Breite mehr hat. Die linke liegt deckungsgleich auf dem ersten
     Strich des Musters - zwei 1-px-Linien an derselben Stelle sind eine. */
  linear-gradient(var(--w4),var(--w4)) 4px center/1px 11px no-repeat,
  linear-gradient(var(--w4),var(--w4)) calc(100% - 5px) center/1px 11px no-repeat,
  /* Teilstriche, einer je Stufe.
     PROZENTE IN EINEM VERLAUF RECHNEN GEGEN DIE VERLAUFSFLAECHE, nicht gegen
     das Element. Vorher stand hier eine Teilung calc((100% - 8px)/n) in einem
     Verlauf, dessen Flaeche selbst nur calc(60% - 4,8px) mass - bei 215 px
     Regler ergab das 23,2 px statt 41,4 px, und die Striche standen weder
     gleichmaessig noch auf den Rasten. Jetzt deckt der Verlauf genau die
     Strecke des Punktes ab, und die Teilung ist 100%/n DIESER Flaeche. */
  repeating-linear-gradient(90deg,var(--w4) 0 1px,
   transparent 1px calc(100%/var(--n)))
   4px center/var(--sp) 11px no-repeat}
.sl input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:8px;
 height:8px;border-radius:50%;background:var(--w1);
 box-shadow:0 0 8px rgba(255,255,255,.8);cursor:pointer}
.sl input[type=range]::-moz-range-thumb{width:8px;height:8px;border:0;
 border-radius:50%;background:var(--w1);box-shadow:0 0 8px rgba(255,255,255,.8);
 cursor:pointer}
.sl input[type=range]:focus{outline:none}

/* Reiter und Segmentwahl sind auch nur Knoepfe. Zusammengeschoben waren sie
   die einzigen ohne Abstand - genau das fiel als Unregelmaessigkeit auf.
   Jetzt derselbe Abstand wie ueberall; welcher gilt, zeigt der Schein. */
.seg{display:inline-flex;gap:var(--s2)}
/* SINNBILDER: nur Umriss, dieselbe Strichstaerke wie jede andere Linie -
   1 px, unabhaengig von der Groesse (non-scaling-stroke). Keine Flaechen. */
button.ic{padding:var(--s2);line-height:0;flex:none}
button.ic svg{width:16px;height:16px;display:block;fill:none;
 stroke:currentColor;stroke-width:1;vector-effect:non-scaling-stroke;
 stroke-linecap:round}
h1 a{color:inherit;text-decoration:none}
h1 a:hover{text-shadow:0 0 10px rgba(255,255,255,.7)}
/* Segmentwahl und Auswahlfeld enden am selben Rand - sonst sieht die eine
   Zeile schmaler aus als die darunter. */
.breit{flex:1;width:auto;max-width:none}
.seg.breit button{flex:1}
/* Zwei Zeilen, die zusammengehoeren: Plus oben, Minus unten. Der Abstand
   dazwischen ist s2 - sie sind EIN Bedienelement, keine zwei Zeilen der
   Karte. Die feste Knopfbreite haelt die Spalten untereinander. */
/* Steht ein Block ueber zwei Zeilen, gehoert die Beschriftung DARUEBER und
   nicht daneben - sonst schwebt sie neben zwei Zeilen und zeigt auf keine. */
/* Steht eine Beschriftung ueber ihren Bedienelementen, gilt fuer die Zeile
   dasselbe wie fuer eine Reglerzeile: Kopf oben, darunter das Element ueber
   die GANZE Breite. Darum stretch statt flex-start - mit flex-start schrumpft
   jedes Kind auf seinen Inhalt und die Zeile endete mitten in der Karte. */
.row.hoch{flex-direction:column;align-items:stretch;gap:var(--s2)}
.row.hoch > label{min-width:0}
/* Mehrere Knoepfe oder Felder NEBENEINANDER innerhalb so einer Zeile. Ohne
   die Klammer wuerden sie in der Spalte untereinander stehen.
   RASTER, nicht flex: mit flex:1 ist die Untergrenze eines Elements sein
   Polster plus Rahmen, und der Rest wird erst DANACH gleich verteilt. Ein
   Knopf (2x16 Polster) kam so 34 px breiter heraus als ein Eingabefeld ohne
   seitliches Polster - dieselbe Zeile, verschieden breite Elemente. Gleich
   grosse Rasterspalten kennen das Problem nicht. */
.grp{display:grid;grid-auto-flow:column;grid-auto-columns:1fr;
 gap:var(--s2);width:100%;container-type:inline-size}
/* Stufe 1 gegen Beschriftungen ueber den Rand (siehe button): in einer
   schmalen Gruppe - praktisch auf dem Handy - weniger Buchstabenabstand und
   seitliches Polster. "Browserzeit" riss sonst als "Browserz | eit". */
@container (max-width:360px){
 .grp > button,.grp > .dat{letter-spacing:.06em;padding-left:var(--s1);
  padding-right:var(--s1)}
}
/* Eingabefelder sind ersetzbare Elemente: im Raster dehnen sie sich NICHT
   von selbst auf ihre Spalte, sondern bleiben auf ihrer Eigenbreite stehen.
   Darum ausdruecklich beides. */
.grp > *{min-width:0;width:100%;max-width:none;justify-self:stretch}
/* Die Grundregel fuer Felder heisst input:not([type=range]),select und traegt
   damit MEHR Gewicht als ".grp > *" - ihre 80 px gewannen. Darum hier
   dieselbe Form noch einmal, statt sich auf die Reihenfolge zu verlassen. */
.grp > input:not([type=range]),.grp > select{width:100%;max-width:none}
/* Werte statt Bedienelemente: nebeneinander in ihrer Eigenbreite, links. */
.grp.frei{grid-auto-columns:max-content;justify-content:start}
.grp.frei > *{width:auto}
/* Zwei Zeilen, die zusammengehoeren: Plus oben, Minus unten. Der Abstand
   dazwischen ist s2 - sie sind EIN Bedienelement, keine zwei Zeilen. */
.paar{display:flex;flex-direction:column;gap:var(--s2);width:100%}
.paar > span{display:grid;grid-auto-flow:column;grid-auto-columns:1fr;
 gap:var(--s2)}
.paar button{text-align:center}
</style></head><body>
<div class="top"><h1><a href="#" onclick="reiter('bed');return false">Star Watch</a></h1>
<div class="net"><span id="kopf_id" class="kl">&mdash;</span>&nbsp;
<span id="net">&nbsp;</span></div></div>
<div id="einricht" class="kasten" style="display:none">
<b>Diese Uhr ist noch nicht eingerichtet.</b>
<p class="hint">Sie kennt noch kein WLAN und spannt deshalb ein eigenes Netz
auf &mdash; an dem h&auml;ngst du gerade. W&auml;hle unten dein Heimnetz aus
und gib das Passwort ein. Mehr ist nicht zu tun.</p>
<p class="hint">Danach verschwindet dieses Netz, die Uhr ist unter ihrer
Adresse im Heimnetz erreichbar, und die &uuml;brige Bedienung erscheint.</p>
</div>
<div id="sperre" class="kasten" style="display:none;background:rgba(255,59,59,.06);border-color:var(--err);box-shadow:0 0 18px rgba(255,59,59,.15)">
<b style="color:var(--err)">Dieser Schritt verlangt das Ger&auml;te-Token</b>
<p class="hint"><b>Die normale Bedienung ist davon nicht betroffen</b> &mdash;
Licht, Timer, Zeit und Zeigerfahrten gehen im Heimnetz ohne Token. Verlangt
wird es nur f&uuml;r Eingriffe, die sich nicht zur&uuml;cknehmen lassen:
Netzeinstellungen, Firmware, Werksreset, Neustart, Offset, Endstufe &mdash;
und f&uuml;r das Ger&auml;teblatt selbst.</p>
<p class="hint">Der Browser merkt es sich <b>pro Adresse</b>. Wer die Uhr
vorher &uuml;ber einen anderen Namen oder eine andere IP ge&ouml;ffnet hat,
steht hier ohne da.</p>
<p class="hint">H&auml;ngst du gerade am <b>offenen Einrichtungsnetz</b>, ist das
richtig so: dort ist nur die Karte <b>Netz</b> freigeschaltet. Trag das WLAN ein
&mdash; sobald die Uhr im Heimnetz h&auml;ngt, steht das Token im Ger&auml;teblatt
und alles andere geht wieder.</p>
<div class="row"><span class="grp"><input id="sperrf" placeholder="Token einf&uuml;gen">
<button class="p" onclick="sperreSetzen()">merken</button></span></div>
<div class="row"><button id="sperrb" style="display:none" onclick="tokAusLog()">aus dem Log holen</button></div>
<p class="hint" id="sperrm">Es steht im Ger&auml;teblatt.</p>
</div>
<div id="warte" class="kasten" style="display:none;background:rgba(255,157,43,.06);border-color:var(--warn);box-shadow:0 0 18px rgba(255,157,43,.15)">
<b style="color:var(--warn)">Uhr ist gerade nicht erreichbar</b>
<p class="hint" id="warte_txt">Vermutlich Neustart oder Update. <b>Die Seite meldet sich von
selbst</b>, sobald die Uhr wieder antwortet &mdash; auch unter einer neuen Adresse.</p></div>
<div id="neu" class="kasten" style="display:none;background:rgba(43,255,136,.06);border-color:var(--ok);box-shadow:0 0 18px rgba(43,255,136,.15)">
<b style="color:var(--ok)">Im Heimnetz erreichbar</b><br>
<span style="font-size:15px">&rarr; <a id="neu_ip" href="#" style="color:var(--info)"></a>
&nbsp;oder&nbsp; <a id="neu_nm" href="#" style="color:var(--info)"></a></span>
<p class="hint" id="neu_txt">Diese Seite wechselt <b>von selbst</b> dorthin, sobald dein
Ger&auml;t im Heimnetz ist. Der eigene Zugangspunkt der Uhr l&auml;uft bis zum n&auml;chsten
Neustart weiter, damit die Adresse ablesbar bleibt.</p></div>

<!-- ANGEPINNTE LEISTE. Was man im Betrieb wirklich anfasst, und zwar aus jedem
     Reiter heraus: Not-Aus, Klinge, Motor, Helligkeit, Homing. -->
<div class="bar">
<label class="tg" title="Aus: anhalten, Klinge aus, Motor stromlos - die Klinge kippt dann aus ihrer Lage. Ein: Motor, Homing, Klinge."><input
 type="checkbox" id="tg_alles" onchange="c('q '+(this.checked?1:0))">
<span class="sw"><i class="pt"></i></span><span class="lb2" id="tg_alles_lb">Betrieb</span></label>
<span class="sl"><span class="kz"><span class="lb">Licht</span>
<span class="tr">//</span><span class="w" id="sl_hell_w">&mdash;</span></span>
<input type="range" id="sl_hell" min="0" max="100" step="1" style="--n:4"
 oninput="$('sl_hell_w').textContent=this.value+' %'"
 onchange="c('l '+this.value)"></span>
<span class="uhrzeit" id="bar_uhr" style="margin-left:auto">--:--</span>
<!-- EIN Knopf statt zweier Reiter. Er zeigt, WOHIN er fuehrt: im Uhrbild das
     Zahnrad, in den Einstellungen die Uhr. Zurueck geht es auch ueber
     "Star Watch" oben links. -->
<button id="rt_um" class="ic" title="Einstellungen" onclick="umschalten()">
<svg id="ic_zahn" viewBox="0 0 20 20"><circle cx="10" cy="10" r="3.2"/>
<circle cx="10" cy="10" r="7"/><path d="M10 .8v2.2M10 17v2.2M.8 10h2.2M17 10h2.2
M3.5 3.5l1.6 1.6M14.9 14.9l1.6 1.6M16.5 3.5l-1.6 1.6M5.1 14.9l-1.6 1.6"/></svg>
<svg id="ic_uhr" viewBox="0 0 20 20" style="display:none"><circle cx="10" cy="10" r="7.6"/>
<path d="M10 5.2V10l3.2 2.1"/></svg>
</button>
</div>

<!-- ===================== UHR ===================== -->
<div id="bl_bed"><div class="g uhr">

<div class="k blatt"><h2>Zifferblatt</h2>
<div class="kopf">
<div class="zahl" id="u_text">--:--</div>
<div class="lage" id="u_lage">&mdash;</div></div>
<!-- vector-effect="non-scaling-stroke" auf ALLEN Strichen: der viewBox ist
     200 Einheiten breit und wird auf rund 260 Bildpunkte gezeichnet, also
     skaliert jeder Strich um 1,3 mit - bei groesserer Karte um mehr. Eine
     Linie mit stroke-width="1" waere daneben doppelt so dick wie ein
     CSS-Rahmen mit 1px. Mit non-scaling-stroke zaehlt die Angabe in
     BILDPUNKTEN, und Zifferblatt, Knopf und Regler tragen dieselbe Staerke. -->
<!-- DER SCHEIN DARF UEBER DEN RAND HINAUS. Die Klinge reicht bis Radius 90,
     ihr Hof traegt bei voller Helligkeit rund 30 Einheiten weiter - das SVG
     schnitt ihn an seiner eigenen Kante ab.
     Den viewBox zu vergroessern war der falsche Weg: er blaest die
     Zeichenflaeche auf, und weil die Pixelbreite gleich blieb, schrumpfte das
     Zifferblatt auf 77 %. Stattdessen bleibt der viewBox bei 200, und das SVG
     darf ueberstehen (overflow:visible) - der Hof malt dann in das Polster der
     Karte hinein, das ohnehin leer ist. -->
<svg id="uhr" viewBox="0 0 200 200" stroke-width="1" vector-effect="non-scaling-stroke">
 <defs>
  <!-- filterUnits userSpaceOnUse und nicht die uebliche Prozentangabe: die
       liefe auf den Rahmen des LINIENZUGS, und der ist bei senkrechtem Zeiger
       nur wenige Einheiten breit - der Schein waere seitlich abgeschnitten. -->
  <filter id="u_glf" filterUnits="userSpaceOnUse" x="-40" y="-40" width="280" height="280">
   <feGaussianBlur id="u_glb" stdDeviation="6"/>
  </filter>
  <filter id="u_glf2" filterUnits="userSpaceOnUse" x="-40" y="-40" width="280" height="280">
   <feGaussianBlur id="u_glb2" stdDeviation="2"/>
  </filter>
  <!-- Der Schein der WEISSEN Linien. In den Vorlagen leuchtet nicht alles -
       aber die gezeichneten Linien tun es, und genau davon lebt das Bild. -->
  <filter id="u_wglow" filterUnits="userSpaceOnUse" x="-40" y="-40" width="280" height="280">
   <feGaussianBlur stdDeviation="1.1" result="b"/>
   <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
 </defs>
 <g id="u_striche" filter="url(#u_wglow)"></g>
 <!-- Nullpunkt und Sensorfenster sind LINIEN wie alles andere - gleiche
      Strichbreite, durchgehend, ohne Punkt am Ende. Ein gefuellter Kreis und
      ein 5 px breiter Bogen waren Flaechen. -->
 <path id="u_fenster" fill="none" stroke="var(--warn)" stroke-width="1"
       vector-effect="non-scaling-stroke" opacity="0"/>
 <line id="u_null" x1="100" y1="100" x2="100" y2="14" stroke="var(--warn)"
       stroke-width="1" vector-effect="non-scaling-stroke"/>
 <line id="u_ziel" x1="100" y1="100" x2="100" y2="20" stroke="var(--ok)"
       stroke-width="1" vector-effect="non-scaling-stroke" opacity="0"/>
 <!-- ZWEI HOEFE, nicht einer: der weite traegt weit, der enge macht die
      Klinge hell. Ein einzelner muesste beides und kann nur eins. -->
 <line id="u_glow" x1="100" y1="89" x2="100" y2="28" stroke="var(--err)"
       stroke-width="14" stroke-linecap="round" filter="url(#u_glf)" opacity="0"/>
 <line id="u_glow2" x1="100" y1="89" x2="100" y2="28" stroke="var(--err)"
       stroke-width="6" stroke-linecap="round" filter="url(#u_glf2)" opacity="0"/>
 <line id="u_zeiger" x1="100" y1="89" x2="100" y2="28" stroke="var(--w1)"
       stroke-width="3.4" stroke-linecap="round"/>
 <!-- DER GRIFF LIEGT UEBER DER KLINGE UND IST GEFUELLT - mit der
      Hintergrundfarbe, nicht mit einer sichtbaren Flaeche. Ohne das schien der
      Hof der Klinge durch den offenen Griff hindurch und das Stabende ragte
      sichtbar hinein. Ein Griff ist undurchsichtiges Metall; dass er die
      Striche hinter sich verdeckt, ist richtig. -->
 <!-- DER GRIFF IST DAS GRIFFSTUECK VON VORN (23.10.2026), nach
      01_CAD/Export/v2.10/Einzelteile/fusion_Griffstueck.3mf ausgemessen:
      51,5 mm, Drehpunkt 24,7 mm vom Knaufende, auf r -14,4..+15 gelegt
      (0,571 je mm) - laengs UND quer, also im echten Verhaeltnis (Fabian:
      das betonte war zu breit). Knauf mit Fasen und Rippen, drei Rillen,
      Muendung schraeg - aus ihr tritt die Klinge. Dieselben Zahlen stehen in
      app/.../Zifferblatt.swift. Umriss
      wie alles andere; gefuellt mit der Hintergrundfarbe, damit der Hof der
      Klinge nicht durch den Griff scheint. -->
 <g id="u_griff" fill="none" stroke-width="1">
  <path d="M97.5 114.4 L102.5 114.4 L103.14 113.7 L103.14 106.3 L102.5 105.6 L102.97 105.6 L102.97 88.1 L97.03 85 L97.03 105.6 L97.5 105.6 L96.86 106.3 L96.86 113.7 Z"
        fill="var(--bg)" stroke="var(--w3)" vector-effect="non-scaling-stroke"/>
  <path d="M96.86 113.7 H103.14 M96.86 106.3 H103.14 M98.8 113.2 V106.8 M101.2 113.2 V106.8 M97.03 94.2 H102.97 M97.03 93.2 H102.97 M97.03 92.2 H102.97"
        stroke="var(--w4)" vector-effect="non-scaling-stroke"/>
 </g>
 <!-- KEIN KREIS MEHR IN DER MITTE (23.10.2026): der Drehpunkt liegt im
      Griff, und der Griff zeigt ihn - Kreis und Rechteck darueber waren
      Fremdkoerper. -->
</svg>
<p class="hint" id="u_hint">&mdash;</p></div>

<div class="k" id="k_farbe"><h2>Farbe</h2>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Rot</span><span class="tr">//</span><span class="w" id="fr_w">&mdash;</span></span>
<input type="range" id="fr" min="0" max="255" step="1" style="--n:4;--pk:#ff2f2f" oninput="farbRegler()"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Gr&uuml;n</span><span class="tr">//</span><span class="w" id="fg_w">&mdash;</span></span>
<input type="range" id="fg" min="0" max="255" step="1" style="--n:4;--pk:#2bff88" oninput="farbRegler()"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Blau</span><span class="tr">//</span><span class="w" id="fb_w">&mdash;</span></span>
<input type="range" id="fb" min="0" max="255" step="1" style="--n:4;--pk:#3a7bff" oninput="farbRegler()"></span></div>
<div class="row"><span class="grp fa5">
<button id="fa_ff0000" title="Rot" aria-label="Rot" onclick="farbeSetzen('ff0000')"><span class="fp" style="background:#ff0000"></span><span class="fw">Rot</span></button>
<button id="fa_00ff00" title="Gr&uuml;n" aria-label="Gr&uuml;n" onclick="farbeSetzen('00ff00')"><span class="fp" style="background:#00ff00"></span><span class="fw">Gr&uuml;n</span></button>
<button id="fa_0014ff" title="Blau" aria-label="Blau" onclick="farbeSetzen('0014ff')"><span class="fp" style="background:#0014ff"></span><span class="fw">Blau</span></button>
<button id="fa_ff6e00" title="Gelb" aria-label="Gelb" onclick="farbeSetzen('ff6e00')"><span class="fp" style="background:#ff6e00"></span><span class="fw">Gelb</span></button>
<button id="fa_9b00ff" title="Violett" aria-label="Violett" onclick="farbeSetzen('9b00ff')"><span class="fp" style="background:#9b00ff"></span><span class="fw">Violett</span></button>
<button id="fa_weiss" title="Wei&szlig;" aria-label="Wei&szlig;" onclick="farbeSetzen(WEISS)"><span class="fp" style="background:#ffd764"></span><span class="fw">Wei&szlig;</span></button>
</span></div>
<div class="row"><button onclick="weissMerken()">als Wei&szlig; dieser Uhr merken</button></div>
<div class="row" id="fa_meld" style="display:none"><span class="grp">
<span class="lage" style="align-self:center"><span class="fp" id="fa_mpt"></span><span id="fa_mtx">&mdash;</span></span>
<button onclick="c('lm-')">quittieren</button></span></div>
</div>

<div class="k"><h2>Timer</h2>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Dauer</span><span class="tr">//</span><span class="w" id="sl_timer_w">5 min</span></span>
<input type="range" id="sl_timer" min="0" max="11" step="1" value="5" style="--n:11"
 oninput="$('sl_timer_w').innerHTML=timerText(this.value)"></span></div>
<div class="row"><button class="p" id="t_btn" onclick="timerKnopf()">starten</button></div>
<div class="row hoch"><label>Lauf</label><span class="grp">
<button id="tl_0" onclick="c('tl 0')">Gleitend</button>
<button id="tl_1" onclick="c('tl 1')">Sekundentakt</button></span></div>
<p class="hint">Springt auf zw&ouml;lf, l&auml;uft ab, blinkt. Minimum <b id="tmin">&mdash;</b>.
<b>Gleitend</b> dreht gleichm&auml;&szlig;ig, <b>Sekundentakt</b> springt jede Sekunde
wie ein Quarzwerk &mdash; auch mitten im Lauf umschaltbar.</p>
<details><summary>freie Dauer in Sekunden</summary>
<div class="row"><span class="grp"><input id="ts" placeholder="Sek.">
<button onclick="c('t '+v('ts'))">starten</button></span></div></details></div>

<div class="k" id="k_auto"><h2>Automatik</h2>
<div class="kopf"><div class="lage"><span id="la_jetzt">&mdash;</span></div></div>
<div class="row"><label class="tg"><input
 type="checkbox" id="tg_la" onchange="c('lu '+(this.checked?1:0))">
<span class="sw"><i class="pt"></i></span><span class="lb2">Nach Umgebungslicht dimmen</span></label></div>
<div class="row"><label class="tg"><input
 type="checkbox" id="tg_ln" onchange="c('lu n '+(this.checked?1:0))">
<span class="sw"><i class="pt"></i></span><span class="lb2">Nachts ausschalten</span></label></div>
<details><summary>Schwellen</summary>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Gemessen</span><span class="tr">//</span><span class="w" id="la_lux">&mdash;</span></span></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Kleinste</span><span class="tr">//</span><span class="w" id="sl_lamin_w">&mdash;</span></span>
<input type="range" id="sl_lamin" min="5" max="100" step="5" style="--n:19"
 oninput="$('sl_lamin_w').textContent=this.value+' % des Reglers'"
 onchange="c('lu min '+this.value)"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Voll ab</span><span class="tr">//</span><span class="w" id="sl_lahell_w">&mdash;</span></span>
<input type="range" id="sl_lahell" min="0" max="8" step="1" style="--n:8"
 oninput="$('sl_lahell_w').textContent=luxText(LA_HELL[this.value])"
 onchange="c('lu hell '+LA_HELL[this.value])"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Aus unter</span><span class="tr">//</span><span class="w" id="sl_laaus_w">&mdash;</span></span>
<input type="range" id="sl_laaus" min="0" max="8" step="1" style="--n:8"
 oninput="$('sl_laaus_w').textContent=luxText(LA_AUS[this.value])"
 onchange="c('lu aus '+LA_AUS[this.value])"></span></div>
<p class="hint">Kerze in 1&nbsp;m &asymp; 1&nbsp;lx, Wohnzimmer abends 50&ndash;150&nbsp;lx.
Ausgeschaltet wird nach 30&nbsp;s Dunkelheit, eingeschaltet sofort.</p></details></div>

<div class="k"><h2>Zeit</h2>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Zeitzone</span></span>
<select id="tzs" onchange="tzWahl()">
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Mitteleuropa &middot; Wien, Berlin</option>
<option value="WET0WEST,M3.5.0/1,M10.5.0">Westeuropa &middot; London, Lissabon</option>
<option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Osteuropa &middot; Athen, Helsinki</option>
<option value="MSK-3">Moskau</option>
<option value="EST5EDT,M3.2.0,M11.1.0">New York</option>
<option value="CST6CDT,M3.2.0,M11.1.0">Chicago</option>
<option value="MST7MDT,M3.2.0,M11.1.0">Denver</option>
<option value="PST8PDT,M3.2.0,M11.1.0">Los Angeles</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney</option>
<option value="JST-9">Tokio</option>
<option value="UTC0">UTC</option>
<option value="">eigene &hellip;</option></select></span></div>
<div class="row" id="tz_eigen" style="display:none"><span class="grp"><input id="tz">
<button onclick="tzEigen()">setzen</button></span></div>
<p class="hint">Zeit und Sommerzeit aus dem Netz. Nach einer Fahrt von Hand
&uuml;bernimmt die Uhr nach zehn Sekunden wieder.</p>
<details><summary>Zeit manuell stellen</summary>
<div class="row"><span class="grp"><input id="zt" placeholder="14:35:00">
<button onclick="c('z '+v('zt'))">stellen</button>
<button onclick="jetzt()">Browserzeit</button></span></div></details></div>

<div class="k"><h2>Effekte</h2>
<div class="row"><label class="tg"><input
 type="checkbox" id="tg_ez" onchange="c('jz '+(this.checked?1:0))">
<span class="sw"><i class="pt"></i></span><span class="lb2">Z&uuml;nd- und Einfahreffekt</span></label></div>
<div class="row"><label class="tg"><input
 type="checkbox" id="tg_es" onchange="c('js '+(this.checked?1:0))">
<span class="sw"><i class="pt"></i></span><span class="lb2">Stundeneffekt zur vollen Stunde</span></label></div>
<p class="hint">Z&uuml;nd- und Einfahreffekt: beim Einschalten, beim Farbwechsel und bei
der Nachtabschaltung &mdash; aus hei&szlig;t sanft &uuml;berblenden. Stundeneffekt: zur
vollen Stunde eine Minute auf volle Helligkeit, nur wenn die Klinge brennt und kein
Timer l&auml;uft.</p>
</div>

<div class="k"><h2>Bewegung</h2>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Tempo</span><span class="tr">//</span><span class="w" id="sl_tempo_w">&mdash;</span></span>
<input type="range" id="sl_tempo" min="1" max="6" step="1" style="--n:5"
 oninput="$('sl_tempo_w').textContent=this.value+' U/s'"
 onchange="c('f '+(this.value*3200))"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Anlauf</span><span class="tr">//</span><span class="w" id="sl_rampe_w">&mdash;</span></span>
<input type="range" id="sl_rampe" min="0" max="4" step="1" style="--n:4"
 oninput="$('sl_rampe_w').textContent=RAMPEN[this.value][1]"
 onchange="c('a '+RAMPEN[this.value][0])"></span></div>
<p class="hint" id="ramp_h">&mdash;</p>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Schwung</span><span class="tr">//</span><span class="w" id="sl_kreise_w">&mdash;</span></span>
<input type="range" id="sl_kreise" min="0" max="5" step="1" style="--n:5"
 oninput="$('sl_kreise_w').textContent=(this.value=='0'?'aus':this.value+'&times;')"
 onchange="c('k '+this.value)"></span></div>
<p class="hint">Schwung: volle Kreise vor dem Ziel. Fahrt dauert <b id="keff">&mdash;</b>.</p></div>

</div></div>

<!-- ===================== EINSTELLUNGEN ===================== -->
<div id="bl_wrk"><div class="g">

<div class="k"><h2>Zustand</h2>
<table id="z"></table></div>

<div class="k"><h2>Zw&ouml;lf einmessen</h2>
<div class="row"><button onclick="c('h')">Homing <span class="tr">//</span>
<span id="ez">&mdash;</span></button></div>
<div class="row hoch"><label>Verstellen</label><span class="paar">
<span><button onclick="c('v 1')">+1</button><button onclick="c('v 10')">+10</button>
<button onclick="c('v 50')">+50</button></span>
<span><button onclick="c('v -1')">&minus;1</button><button onclick="c('v -10')">&minus;10</button>
<button onclick="c('v -50')">&minus;50</button></span></span></div>
<div class="row"><button id="b12" onclick="zwoelfHalten()">Auf zw&ouml;lf <span class="tr">//</span>
<span id="b12w">h&auml;lt bis best&auml;tigt</span></button></div>
<div class="row"><button id="bhz" onclick="hierZwoelf()">Hier ist zw&ouml;lf <span class="tr">//</span>
<span id="eo">&mdash;</span><span id="bhzw"></span></button></div>
<p class="hint"><b>1.</b> Homing &nbsp;<b>2.</b> auf zw&ouml;lf, verstellen
&nbsp;<b>3.</b> best&auml;tigen &ndash; Klinge blinkt gr&uuml;n, dann zur&uuml;ck zur Uhrzeit.</p>
<details><summary>Offset direkt setzen</summary>
<div class="row"><span class="grp"><input id="oo">
<button onclick="c('o '+v('oo'))">setzen</button></span></div></details></div>

<div class="k" id="k_netz"><h2>Netz</h2>
<!-- DAS AKTUELLE NETZ zuerst: wo haengt die Uhr, unter welcher Adresse, wie
     gut. Vorher zeigte die Karte nur Eingabefelder, und was gerade gilt,
     stand nirgends (26.09.2026). -->
<div class="kopf"><div class="lage" id="wl_jetzt">&mdash;</div></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Netz</span></span>
<select id="wn"></select></span></div>
<div class="row"><button onclick="scan()">Netze suchen</button></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Passwort</span></span>
<input id="wp" type="password" placeholder="leer = gespeichertes behalten"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Adresse</span></span>
<select id="wa" onchange="wlArt()">
<option value="dhcp">automatisch (DHCP)</option>
<option value="fest">manuell (feste IP)</option></select></span></div>
<div id="wl_fest" style="display:none;flex-direction:column;gap:var(--s3)">
<div class="row"><span class="grp"><input id="wl_ip" placeholder="IP, z. B. 192.168.1.50">
<input id="wl_maske" placeholder="Maske 255.255.255.0"></span></div>
<div class="row"><span class="grp"><input id="wl_gw" placeholder="Gateway (Router)">
<input id="wl_dns" placeholder="DNS (leer = Gateway)"></span></div></div>
<div class="row"><span class="grp"><button onclick="wlan()">&uuml;bernehmen</button>
<button onclick="if(confirm('Gespeicherte Zugangsdaten und Adresse loeschen?'))
 post('/test/wlan',{ssid:'',pass:''}).then(t)">vergessen</button></span></div>
<div class="row"><span id="ns" class="hint" style="margin-top:0"></span></div>
<p class="hint">Gleiches Netz, leeres Passwort: das gespeicherte gilt weiter &mdash; so
l&auml;sst sich die Adresse &auml;ndern, ohne das WLAN-Passwort zu tippen. Klappt die
Verbindung nicht, kommt die alte Einstellung zur&uuml;ck. Eine feste Adresse muss
au&szlig;erhalb des DHCP-Bereichs des Routers liegen.</p></div>

<div class="k"><h2>MQTT</h2>
<div class="row"><label class="tg"><input type="checkbox" id="mq_an"
 onchange="mqttSetzen()">
<span class="sw"><i class="pt"></i></span><span class="lb2">An Home Assistant melden</span></label></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Broker</span></span>
<input id="mq_host" placeholder="192.168.1.2" oninput="mq_beruehrt=true"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Port</span></span>
<input id="mq_port" type="number" min="1" max="65535" oninput="mq_beruehrt=true"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Benutzer</span></span>
<input id="mq_user" oninput="mq_beruehrt=true"></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Passwort</span></span>
<input id="mq_pw" type="password" oninput="mq_beruehrt=true"></span></div>
<div class="row"><button class="p" onclick="mqttSetzen()">speichern</button></div>
<p class="hint" id="mq_hint">Braucht einen Broker im Heimnetz &mdash; in Home
Assistant das Zusatzprogramm Mosquitto. Das Passwortfeld <b>leer lassen
hei&szlig;t unver&auml;ndert</b>; es wird nie zur&uuml;ckgegeben.</p></div>

<div class="k" id="k_fw"><h2>Firmware</h2>
<div class="kopf"><div class="lage"><span id="fw_jetzt">&mdash;</span></div></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Kanal</span></span>
<select id="fw_kanal" onchange="c('yk '+this.value)">
<option value="s">stabil</option>
<option value="b">beta &middot; Vorabfassungen</option>
<option value="e" disabled>eigene Adresse</option></select></span></div>
<div class="row"><span class="sl"><span class="kz"><span class="lb">Updates</span></span>
<select id="fw_modus" onchange="c('y '+this.value)">
<option value="0">aus</option>
<option value="1">nur von Hand</option>
<option value="2">automatisch, nachts</option></select></span></div>
<div class="row"><span class="grp"><button onclick="fwSuchen()">Nach Update suchen</button>
<button class="p" id="fw_ein" style="display:none" onclick="fwEinspielen()">Jetzt aktualisieren</button></span></div>
<p class="hint" id="fw_m">&mdash;</p></div>

<div class="k"><h2>Ger&auml;t</h2>
<div class="row hoch"><label>Name</label><span class="grp">
<input id="al" maxlength="24" placeholder="z. B. Wohnzimmer" oninput="al_beruehrt=true"
 onkeydown="if(event.key==='Enter')aliasSetzen()">
<button class="p" onclick="aliasSetzen()">speichern</button></span></div>
<div class="row hoch"><label>Kennung</label><span class="grp frei"><span class="v kl" id="gb_name">&mdash;</span>
<span class="v kl" id="gb_serie">&mdash;</span></span></div>
<div class="row hoch"><label>Sichern</label><span class="grp">
<button onclick="exportieren()">Export</button>
<!-- Eigener Dateiknopf: das Feld ist versteckt, sichtbar ist ein Knopf wie
     jeder andere. Das native Feld bringt einen grauen Kasten mit eigener
     Schrift mit und laesst sich nicht formen. -->
<label class="dat">W&auml;hlen<input type="file" id="imp" accept=".json,application/json"
 onchange="$('impn').textContent=this.files.length?this.files[0].name:'keine Datei'"></label>
<button onclick="importieren()">Import</button></span></div>
<div class="row"><span class="hint" id="impn" style="margin-top:0">keine Datei</span></div>
<p class="hint" id="impm">&mdash;</p>
<div class="row hoch"><label>Ger&auml;t</label><span class="grp">
<button onclick="neustart()">Neustart</button>
<button onclick="werksreset()">Werkseinstellungen</button></span>
<span class="grp"><button onclick="window.open('/test/diagnose','_blank')">Diagnose</button></span></div>

<details><summary><b>Ger&auml;teblatt</b> &mdash; aufschreiben, gilt nur f&uuml;r diese Uhr</summary>
<pre id="gb_blatt" style="max-height:none">&mdash;</pre>
<p class="hint">Beim ersten Start gew&uuml;rfelt, danach unver&auml;nderlich &mdash; sie
&uuml;berleben Werksreset und Update.</p></details>

<details><summary><b>Zugriffstoken</b></summary>
<div class="row"><span class="grp"><input id="tokf" placeholder="Token einf&uuml;gen">
<button onclick="tokSetzen()">merken</button>
<button onclick="tokLoeschen()">vergessen</button></span></div>
<p class="hint" id="tokm">Der Browser merkt es sich, <b>pro Adresse</b>. Am
<b>gesicherten</b> Zugangspunkt der Uhr wird keines verlangt &mdash; am offenen
Einrichtungsnetz dagegen schon: dort ist nur das Eintragen des WLANs frei.</p></details></div>

</div>

<!-- Der Pruefstand ist zugeklappt und bleibt es, bis jemand ihn braucht.
     Hier steht nichts, was eine haengende Uhr wieder zum Laufen bringt -
     das steht darueber. Hier wird gemessen. -->
<details class="gross"><summary>Pr&uuml;fstand &mdash; messen, pr&uuml;fen, Kennlinien</summary>
<div class="g">

<div class="k"><h2>Zeiger anfahren</h2>
<div class="row hoch"><label>Uhrzeit</label>
<span class="grp"><button onclick="c('u 12:00')">12:00</button><button onclick="c('u 15:00')">15:00</button>
<button onclick="c('u 18:00')">18:00</button><button onclick="c('u 21:00')">21:00</button></span>
<span class="grp"><input id="ph" placeholder="7:20">
<button class="p" onclick="c('u '+v('ph'))">anfahren</button></span></div>
<div class="row hoch"><label>Position</label><span class="grp"><input id="pp" placeholder="Schritt">
<button onclick="c('p '+v('pp'))">anfahren</button></span></div>
<p class="hint">Zw&ouml;lferblatt: 15:00 f&auml;hrt wie 3:00.</p></div>

<div class="k"><h2>Pr&uuml;flauf: Pendeln</h2>
<div class="row"><span class="grp"><input id="xn" placeholder="Anzahl">
<input id="xa" placeholder="12:00">
<input id="xb" placeholder="18:00"></span></div>
<div class="row"><span class="grp"><button class="p" onclick="c('x '+v('xn')+' '+v('xa')+' '+v('xb'))">starten</button>
<button class="s" onclick="c('s')">abbrechen</button></span></div>
<div class="row"><button onclick="c('x 10 12:00 18:00')">10&times; 12&harr;18</button>
<button onclick="c('x 20 12:00 15:00')">20&times; 12&harr;15</button>
<button onclick="c('x 5 12:00 12:30')">5&times; kurz</button></div>
<p class="hint">Pendelt und z&auml;hlt die Hall-Flanken mit &mdash; so f&auml;llt ein
Schrittverlust auf.</p></div>

<div class="k"><h2>Licht: Messhilfen</h2>
<div class="row hoch"><label>Kennlinie</label><span class="grp">
<button onclick="c('lr')">Rampe 0-20 %</button>
<button onclick="c('lt')">Treppe 0-100 %</button></span></div>
<p class="hint">AP 0.5: ab welchem Wert glimmt die LED, l&auml;uft sie gleichm&auml;&szlig;ig?
Die Rampe dauert <b>28 s</b>, die Treppe <b>33 s</b>.</p>
<details><summary>PWM-Wert direkt setzen</summary>
<div class="row"><span class="grp"><input id="lw">
<button onclick="c('w '+v('lw'))">setzen</button></span></div>
<p class="hint">Am Regler vorbei, roh in den Z&auml;hler. Nur, um zu sehen, ob die
Regelung &uuml;berschwingt.</p></details></div>

<div class="k"><h2>Hall-Sensor</h2>
<div class="row"><button onclick="c('c')">Spitzenwerte zur&uuml;cksetzen</button></div>
<p class="hint">Sollwert der Spanne: 250 bis 550 mV. Istwerte im Zustand.</p></div>

<div class="k"><h2>Motor und Diagnose</h2>
<div class="row hoch"><label>Treiber</label><span class="grp">
<button onclick="c('d')">auslesen</button>
<button onclick="post('/test/log').then(t)">Log leeren</button></span></div>
<div class="row hoch"><label>Konsole</label>
<span class="grp"><input id="cx"
 placeholder="Kommando" onkeydown="if(event.key==='Enter')c(v('cx'))">
<button onclick="c(v('cx'))">senden</button></span>
<span class="grp"><button onclick="c('?')">alle Kommandos</button></span></div>
<p class="hint">Grenzsuche <b>g</b>, Register <b>r</b>, Tempo <b>f</b>, Rampe <b>a</b> &mdash;
alles &uuml;ber die Konsole. <b>?</b> zeigt die Liste.</p>
</div>

<div class="k" style="grid-column:1/-1"><h2>Log</h2><pre id="log">&mdash;</pre>
<div class="row"><span class="grp"><button onclick="logKopieren()">Kopieren</button>
<button onclick="logLaden()">Herunterladen</button></span></div>
<p class="hint" id="logm">Solange im Log etwas markiert ist, h&auml;lt es an.</p></div>

</div></details></div>

<script>
const $=i=>document.getElementById(i), v=i=>$(i).value.trim();

// --- Zugriffstoken (AP 1.3) ---------------------------------------------
// Einmal einfuegen, danach merkt es der Browser. Am eigenen Zugangspunkt der
// Uhr wird es nicht verlangt - das ist der Kopplungsmoment.
let tok = localStorage.getItem('sw_tok') || '';
// X-Starwatch IMMER: ohne sie weist die Uhr jede Aenderung ab (Schutz vor
// fremden Seiten, siehe waechter() in testweb.cpp). Das Token nur, wenn da.
function kopf(){const k={'X-Starwatch':'1'};if(tok)k['X-Starwatch-Token']=tok;return k}
// JEDE AENDERUNG GEHT PER POST, die Werte im Rumpf statt in der Adresse -
// so landen weder Token noch WLAN-Passwort im Browserverlauf.
function post(pfad,daten){return hol(pfad,{method:'POST',
 headers:{'Content-Type':'application/x-www-form-urlencoded'},
 body:new URLSearchParams(daten||{}).toString()})}
// EIN 401 HOLT ERST DAS TOKEN (27.10.2026): die Uhr gibt es im Heimnetz
// selbst heraus (/test/token), danach geht derselbe Zugriff noch einmal.
// Den roten Kasten gibt es nur noch, wenn auch das scheitert.
function hol(pfad,opt,nochmal){opt=opt||{};
 const o=Object.assign({},opt,{headers:Object.assign({},opt.headers||{},kopf())});
 return fetch(pfad,o).then(r=>{if(r.status!==401)return r;
  if(nochmal||document.body.classList.contains('setup')){tokFehlt();return r}
  return tokHolen().then(ok=>ok?hol(pfad,opt,true):(tokFehlt(),r))})}
let tok_holt=null;
function tokHolen(){if(tok_holt)return tok_holt;
 tok_holt=fetch('/test/token',{method:'POST',headers:{'X-Starwatch':'1'}})
  .then(r=>r.ok?r.text():'').then(x=>{x=x.trim();if(!x)return false;
   if(x!==tok){tok=x;localStorage.setItem('sw_tok',tok);tokDa('Von der Uhr \u00fcbernommen.')}return true})
  .catch(()=>false).finally(()=>{tok_holt=null});return tok_holt}
// DASSELBE, ABER OHNE ALARM. Fuer Abfragen, die von sich aus laufen und
// scheitern DUERFEN - allen voran das Geraeteblatt, das die Seite beim Laden
// holt. Wer nur die Uhrzeit ansehen will, hat mit einem roten Balken nichts
// zu schaffen; das Blatt sagt dann eben "Token noetig" in seiner eigenen
// Karte, wo es hingehoert.
function hol_still(pfad,opt){opt=opt||{};opt.headers=Object.assign({},opt.headers||{},kopf());
 return fetch(pfad,opt)}
// EIN 401 MUSS MAN SEHEN (25.08.2026). Vorher faerbte er nur einen Hinweis in
// einer ZUGEKLAPPTEN Karte - die Uhr sah dann aus wie haengend, nicht wie
// gesperrt. Das hat einmal einen ganzen Abend gekostet.
// WAEHREND DER ERSTEINRICHTUNG NICHT. Dort ist der Balken nicht nur
// nutzlos, sondern irrefuehrend: am offenen Zugangspunkt IST kein Token
// vorgesehen, und wer gerade sein WLAN eintragen will, kann mit "Token
// fehlt" nichts anfangen. Er erschien dort erst, seit befugt() am offenen
// Netz nicht mehr gilt - also als Nebenwirkung einer Absicherung, nicht als
// Absicht.
function tokFehlt(){if(document.body.classList.contains('setup'))return;
 $('sperre').style.display='block';
 $('tokm').textContent='Abgewiesen - Token fehlt oder stimmt nicht.'}
// DAS GEGENSTUECK ZU tokFehlt(), UND ES MUSS EINS GEBEN. Weg blendete den
// roten Kasten bisher nur, wer ihn selbst ausgefuellt hat. Uebernahm die
// Seite das Token beim Koppeln von allein (blatt_holen), blieb er stehen -
// die Uhr sah gesperrt aus, obwohl jede Bedienung durchging. Eine Anzeige,
// die faelschlich sperrt, ist schlimmer als gar keine.
function tokDa(m){$('sperre').style.display='none';
 $('tokm').textContent=m||'Gemerkt. Wird ab jetzt mitgeschickt.'}
function sperreSetzen(){const w=v('sperrf');if(!w)return;
 tok=w;localStorage.setItem('sw_tok',tok);
 $('sperrm').textContent='Gemerkt. Wird ab jetzt mitgeschickt.';
 tokDa();blatt_holen();z()}
// SELBSTHILFE, NUR IN DER ENTWICKLUNGSFASSUNG: dort schreibt die Uhr ihr
// Geraeteblatt in den Ringpuffer, und /test/log ist offen. Bei der
// Auslieferung steht es dort NICHT - deshalb bleibt der Knopf dann verborgen,
// statt einen Weg anzubieten, den es nicht gibt.
function tokAusLog(){fetch('/test/log').then(r=>r.text()).then(x=>{
 const m=x.match(/Zugriffstoken\s+(\S+)/);
 if(!m){$('sperrm').textContent='Nicht im Log - vermutlich aus dem Ringpuffer '+
  'gerollt. Einmal Strom aus und an (NICHT fuenfmal), dann erneut.';return}
 $('sperrf').value=m[1];sperreSetzen();
 $('sperrm').textContent='Aus dem Log geholt und gemerkt.'})
 .catch(()=>{$('sperrm').textContent='Log nicht erreichbar.'})}
function tokSetzen(){tok=v('tokf');localStorage.setItem('sw_tok',tok);
 tokDa();blatt_holen()}
function tokLoeschen(){tok='';localStorage.removeItem('sw_tok');
 $('tokm').textContent='Vergessen.'}

function c(k){post('/test/cmd',{k:k}).then(t)}
// EINMESSEN: "Auf zwoelf" haelt den Zeiger, bis "Hier ist zwoelf" kommt.
// Gruen wird "Hier ist zwoelf", und erst, wenn die Uhr das o! angenommen
// hat - nicht beim Klick. Danach faehrt die Uhr von selbst zur Uhrzeit.
function zwoelfMarke(ok){$('bhz').classList.toggle('gruen',ok);
 $('bhzw').textContent=ok?' \u00b7 gespeichert':''}
function zwoelfHalten(){zwoelfMarke(false);c('uz')}
function hierZwoelf(){post('/test/cmd',{k:'o!'}).then(r=>{t();zwoelfMarke(r.ok)})}
// MQTT-Zugangsdaten. Eigener Endpunkt statt eines Kommandos: Adresse, Benutzer
// und Passwort duerfen Leerzeichen und Sonderzeichen tragen, und die Konsole
// trennt am Leerzeichen.
// WER HIER TIPPT, DEM GEHOEREN DIE FELDER - bis gespeichert ist. `stelle()`
// allein genuegt nicht: es schuetzt nur das Feld mit dem Fokus. Wer den Broker
// eintraegt und dann ins Portfeld wechselt, saehe den Broker eine Sekunde
// spaeter zurueckgesetzt und speicherte die alte Adresse mit dem neuen Port -
// lautlos. Derselbe Fehler wie beim Zeitzonenfeld, dieselbe Abhilfe.
let mq_beruehrt=false;
// NAME DER UHR. Dasselbe Muster wie beim Broker: waehrend getippt wird,
// schreibt der Sekundentakt das Feld nicht zurueck. Leer heisst loeschen.
let al_beruehrt=false;
function aliasSetzen(){const n=v('al').trim();
 c('alias '+(n===''?'-':n)); al_beruehrt=false;}
function mqttSetzen(){
 post('/test/mqtt',{an:$('mq_an').checked?1:0,host:v('mq_host'),port:v('mq_port'),
  user:v('mq_user'),pw:$('mq_pw').value}).then(r=>r.text()).then(x=>{
  $('mq_hint').textContent=x; $('mq_pw').value='';
  // Gespeichert ist gespeichert: ab hier fuehrt der Takt die Felder wieder.
  mq_beruehrt=false; z()})}
function t(){setTimeout(()=>{z();lg()},120)}
function jetzt(){const d=new Date();$('zt').value=[d.getHours(),d.getMinutes(),d.getSeconds()]
 .map(n=>String(n).padStart(2,'0')).join(':');c('z '+$('zt').value)}
function kl(s){return s.startsWith('OK')?'ok':(s.startsWith('ABBRUCH')||s.includes('NICHT')?'err':'')}

// --- Zifferblatt ---------------------------------------------------------
// Blatt 0 ist zwoelf und zeigt nach OBEN. SVG rechnet 0 Grad nach rechts,
// also -90 Grad Versatz. Gedreht wird IM Uhrzeigersinn, weil der Zeiger das
// auch tut - sonst waere die Zeichnung gespiegelt und damit falscher als
// keine.
function pkt(schritte, rev, radius){
 const w = (schritte/rev)*2*Math.PI - Math.PI/2;
 return [100 + radius*Math.cos(w), 100 + radius*Math.sin(w)];
}
function strichen(){
 // DAS ECHTE ZIFFERBLATT. Die Zahlen sind nicht geschaetzt, sondern am
 // Entwurf ausgemessen (Blender-Vorlage des Blatts, 4724 px):
 //
 //   Stunde       alle 30 Grad, Radius 60,0 % ... 75,0 % der halben Breite
 //   halbe Stunde um 15 Grad versetzt, 53,8 % ... 62,0 %
 //
 // Der Reiz liegt darin, dass die kurzen Striche WEITER INNEN sitzen und
 // knapp hinter dem Beginn der langen enden - daher der gestaffelte Kranz.
 // Hier um 1,28 vergroessert, damit der Kranz das Feld ausfuellt; die
 // Verhaeltnisse bleiben dabei genau die des Entwurfs.
 let h='';
 for(let i=0;i<24;i++){
  const stunde=(i%2===0);
  const ri=stunde?76.8:68.9, ra=stunde?96.0:79.4;
  const a=pkt(i*(3200/24),3200,ra), b=pkt(i*(3200/24),3200,ri);
  // ALLE Striche voll weiss und gleich stark. Was Stunde von halber Stunde
  // unterscheidet, ist die LAENGE - so steht es im Entwurf. Eine gedimmte
  // Linie waere wieder ein Grauwert.
  h+='<line x1="'+a[0].toFixed(1)+'" y1="'+a[1].toFixed(1)+'" x2="'+b[0].toFixed(1)+
     '" y2="'+b[1].toFixed(1)+'" stroke="#fff" stroke-width="1"'+
     ' vector-effect="non-scaling-stroke" stroke-linecap="butt"/>';
 }
 $('u_striche').innerHTML=h;
}
function bogen(von, bis, rev, r){
 const a=pkt(von,rev,r), b=pkt(bis,rev,r);
 const gross = ((bis-von+rev)%rev) > rev/2 ? 1 : 0;
 return 'M'+a[0].toFixed(1)+' '+a[1].toFixed(1)+' A'+r+' '+r+' 0 '+gross+' 1 '+
        b[0].toFixed(1)+' '+b[1].toFixed(1);
}
function blatt(d){
 const rev=d.rev;
 // DER ZEIGER IST EIN LICHTSCHWERT, kein Strich. Am Aufbau sitzt der
 // Drehpunkt IM GRIFF - der Griff steht also auf der Gegenseite, mit
 // negativem Radius, und die Klinge tritt bei 11 aus ihm heraus. Sie endet
 // bei 72 und damit vor den Stundenstrichen (ab 76,8): sie zeigt auf sie,
 // statt sie zu ueberdecken.
 // DIE RADIEN SIND GERECHNET, NICHT GEWAEHLT. Aus dem Entwurf kommen drei
 // Verhaeltnisse: Griff 34 % der Gesamtlaenge, Klinge 66 %, und der Kreis
 // sitzt bei 49 % des Griffs. Der Kreis IST die Blattmitte - damit liegt die
 // Griffstellung fest und laesst sich nicht mehr frei setzen:
 //
 //   Lg = 0,34*72 / (1 - 0,34*0,49) = 29,37
 //   Griff  -14,4 ... +15,0   (49 % davon liegen vor der Mitte)
 //   Klinge +15,0 ... +72,0   (72, weil die Stundenstriche bei 76,8 beginnen)
 // Die Klinge reicht bis 90 - genauso weit wie die Nullpunktlinie und der
 // Bogen des Sensorfensters. Vorher endete sie auf 72 und wirkte gestutzt.
 // BEGINNT IM GRIFF (23.10.2026): die Muendung ist schraeg und endet auf der
// kurzen Seite schon bei 11,9 - ab 15 klaffte dort eine Luecke. Der Griff
// ist gefuellt und liegt darueber, der Anfang bleibt verdeckt.
const kf=pkt(d.blatt,rev,11.0), z=pkt(d.blatt,rev,90);
 for(const id of ['u_zeiger','u_glow','u_glow2']){
  $(id).setAttribute('x1',kf[0].toFixed(1)); $(id).setAttribute('y1',kf[1].toFixed(1));
  $(id).setAttribute('x2',z[0].toFixed(1));  $(id).setAttribute('y2',z[1].toFixed(1));
 }
 // Der Griff ist ein Rechteck und wird GEDREHT, nicht neu gezeichnet. Seine
 // Masse stehen fest im SVG; hier bewegt sich nur der Winkel.
 $('u_griff').setAttribute('transform',
  'rotate('+(d.blatt/rev*360).toFixed(2)+' 100 100)');
 // FARBE = ZUSTAND DER KLINGE. Aus ist hellgrau, an ist rot, und wie rot
 // sagt die tatsaechliche AUSGABE in Prozent - nicht der Reglerwert. Der
 // Unterschied ist der ganze Witz: waehrend eines Effekts oder einer
 // Leerfahrt steht der Regler auf 50, die Klinge ist aber dunkel. Ablesbar
 // wird das nur, wenn die Anzeige der Ausgabe folgt.
 // Gemeldet wird, was die Klinge WIRKLICH zeigt (zeige_proz) - mit einer
 // Meldung leuchtet sie auch bei ausgeschaltetem Licht.
 const p=Math.max(0,Math.min(100,d.zeige_proz||0));
 const brennt=p>0;
 // DER KERN IST NICHT ROT. Am Vorbild ist die Klinge innen nahezu weiss und
 // ROT ist der Hof - eine durchgehend rote Klinge sieht aus wie ein
 // Filzstiftstrich. Deshalb wandert mit der Helligkeit BEIDES: der Kern wird
 // heller UND blasser (Saettigung faellt), von Dunkelrot bis Weissglut.
 // DIE FARBEN STEHEN FEST, nur die Intensitaet wandert.
 //
 // Vorher wanderte die Kernfarbe mit der Helligkeit: bei 100 % weissglueh-
 // end, bei 10 % fast dasselbe Rot wie der Hof. Der Kern war zwar immer
 // heller - aber der Abstand wuchs von 14 auf 38 Helligkeitspunkte, und
 // unter rund 30 % war er zu klein, um als "innen hell, aussen rot" gelesen
 // zu werden. Der Stab sah nur oben richtig aus.
 //
 // Jetzt: Kern immer nahezu weiss, Hof immer sattes Rot - und BEIDE werden
 // gemeinsam durchsichtiger. Auf schwarzem Grund heisst weniger Deckkraft
 // weniger Licht, und das Verhaeltnis bleibt dabei erhalten.
 // Der Hof traegt die eingestellte Farbe, der Kern bleibt nahezu weiss mit
 // einem Hauch davon - wie oben begruendet.
 const fz=d.zeige_farbe||d.farbe;
 const hofrgb=/^#[0-9a-f]{6}$/i.test(fz||'') ? ledSicht(fz) : null;
 const kern=hofrgb ? (()=>{const n=parseInt(hofrgb.slice(1),16);
   const m=x=>Math.round(255-(255-x)*0.14);
   return 'rgb('+m(n>>16&255)+','+m(n>>8&255)+','+m(n&255)+')'})() : 'hsl(0,60%,93%)';
 $('u_zeiger').setAttribute('stroke', brennt ? kern : 'rgba(255,255,255,.55)');
 $('u_zeiger').style.opacity = brennt ? (0.42+p*0.0058).toFixed(3) : 1;
 // DER SCHEIN. Drei Groessen wachsen mit der Helligkeit, und alle drei
 // muessen es: nur heller waere ein Aufkleber, nur breiter waere Nebel.
 // Die Zahlen sind nicht geraten - drei Abstufungen wurden als Bild
 // gegeneinander gestellt; die kraeftigere brannte an der Nabe aus, die
 // schwaechere war unsichtbar.
 $('u_glow').setAttribute('x2',z[0].toFixed(1));
 $('u_glow').setAttribute('y2',z[1].toFixed(1));
 if(brennt){
  // Auch der Hof behaelt seine Farbe. Was sich aendert, ist wie stark und
  // wie weit er traegt.
  const hof=hofrgb||'hsl(0,100%,50%)';
  $('u_glow').setAttribute('stroke',hof);
  $('u_glow2').setAttribute('stroke',hof);
  $('u_glb').setAttribute('stdDeviation',(4.0+p*0.055).toFixed(2));
  $('u_glb2').setAttribute('stdDeviation',(1.4+p*0.020).toFixed(2));
  $('u_glow').style.opacity=(0.26+p*0.0055).toFixed(3);
  $('u_glow2').style.opacity=(0.34+p*0.0050).toFixed(3);
 } else { $('u_glow').style.opacity=0; $('u_glow2').style.opacity=0; }
 // Echter Nullpunkt: der Magnet liegt beim Zaehlerstand des Offsets.
 // Die Linie laeuft bis an den Bogen des Sensorfensters heran.
 const n=pkt(d.offset,rev,90);
 $('u_null').setAttribute('x2',n[0].toFixed(1));
 $('u_null').setAttribute('y2',n[1].toFixed(1));
 // Sensorfenster als Bogen um den Magneten, wenn gemessen.
 const f=parseInt(d.fenster,10);
 if(f>0){$('u_fenster').setAttribute('d',
   bogen(((d.offset-f/2)%rev+rev)%rev, (d.offset+f/2)%rev, rev, 90));
  $('u_fenster').style.opacity=1}
 else $('u_fenster').style.opacity=0;
 // Ziel nur waehrend der Fahrt.
 if(d.faehrt){const t=pkt(((d.ziel%rev)+rev)%rev,rev,84);
  $('u_ziel').setAttribute('x2',t[0].toFixed(1));
  $('u_ziel').setAttribute('y2',t[1].toFixed(1));
  $('u_ziel').style.opacity=1}
 else $('u_ziel').style.opacity=0;
 $('u_text').textContent=d.zeigt;
 // Was gerade passiert - in dieser Reihenfolge, weil das Naehere gewinnt:
 // ein laufender Timer sagt mehr als "der Motor faehrt", und ein Homing mehr
 // als beides.
 $('u_lage').textContent =
   d.fuehrung==='Homing'          ? 'Homing laeuft' :
   !d.timer.startsWith('aus')     ? 'Timer \u00b7 '+d.timer :
   d.faehrt                       ? 'faehrt auf '+d.ziel :
   d.fuehrung==='Uhrbetrieb'      ? 'Uhrbetrieb' :
   d.fuehrung==='Hand'            ? 'von Hand \u00b7 '+d.rueckfall :
                                    d.fuehrung;
 // Kurz in der Bedienung, ausfuehrlich steht es in der Zustandstabelle.
 $('u_hint').innerHTML='<span style="color:var(--warn)">&#9679;</span> Nullpunkt'+
  ' &nbsp; <span style="color:var(--ok)">&#9679;</span> Ziel';
}
strichen();

// --- Geraet -------------------------------------------------------------
// Das Blatt traegt Geheimnisse und kommt deshalb aus einem eigenen,
// geschuetzten Endpunkt - nicht aus dem offen lesbaren Zustand.
// Export ueber fetch statt ueber einen Verweis: nur so laesst sich das Token
// als Kopfzeile mitgeben, statt es in die Adresszeile zu schreiben.
function exportieren(){
 hol('/test/export').then(r=>{if(!r.ok)throw 0;return r.text()}).then(x=>{
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([x],{type:'application/json'}));
  a.download='star-watch-einstellungen.json';a.click();
  URL.revokeObjectURL(a.href);
  $('impm').textContent='Export geladen.';
 }).catch(()=>{$('impm').textContent='Export abgewiesen - Token noetig.'});
}

function blatt_holen(){
 hol_still('/test/geraeteblatt').then(r=>r.ok?r.text():null).then(x=>{
  $('gb_blatt').textContent = x || 'Token noetig - unten eintragen.';
  // KOPPLUNG: kam das Blatt, OHNE dass wir ein Token hatten, dann hing der
  // Browser am Zugangspunkt der Uhr - dort gilt das Token als erbracht. Wir
  // uebernehmen es jetzt, damit die Bedienung auch nach dem Umzug ins
  // Heimnetz weiterlaeuft. Niemand tippt etwas ab.
  if(x && !tok){
   const m = x.match(/TOKEN=(\S+)/);
   if(m){tok=m[1];localStorage.setItem('sw_tok',tok);
    tokDa('Beim Koppeln uebernommen - kein Abtippen noetig.');}
  }
 }).catch(()=>{$('gb_blatt').textContent='nicht abrufbar'});
}

// FIRMWARE (27.10.2026). Suchen und Einspielen sind die Konsolenkommandos
// 'yp' und 'yi' - die Uhr antwortet sofort und arbeitet danach; das Ergebnis
// steht im Zustand (upd). Waehrend des Ladens schweigt der Webserver
// (ruhe_hook), danach startet die Uhr neu - den Rest zeigt der Wartekasten.
// Die Suche blockiert den Webserver, bis sie fertig ist - der naechste
// Zustand traegt also schon ihr Ergebnis.
function fwSuchen(){$('fw_m').textContent='Sehe nach \u2026';c('yp')}
function fwEinspielen(){const n=$('fw_ein').dataset.v||'';
 if(!confirm('Auf '+n+' aktualisieren? Die Uhr steht dabei rund eine Minute, startet neu und macht ein Homing.'))return;
 $('fw_ein').style.display='none';
 $('fw_m').textContent='L\u00e4dt '+n+' \u2026 Die Seite ist so lange still, danach startet die Uhr neu.';c('yi')}
function fwZeigen(d){const u=d.upd;if(!u)return;
 $('fw_jetzt').textContent='Firmware '+d.fw+(u.bewaehrung?' \u00b7 auf Bew\u00e4hrung':'');
 stelle('fw_kanal',e=>e.value=u.kanal);stelle('fw_modus',e=>e.value=String(u.modus));
 const bereit=u.stand===2;$('fw_ein').style.display=bereit?'':'none';
 $('fw_ein').dataset.v=u.neu;$('fw_ein').textContent=bereit?'Jetzt auf '+u.neu+' aktualisieren':'Jetzt aktualisieren';
 $('fw_m').textContent=u.meldung||(u.stand===0?'Noch nicht gesucht.':'\u2014')}
function neustart(){
 if(!confirm('Neustart? Die Uhr braucht danach ein Homing.')) return;
 post('/test/neustart').then(r=>r.text()).then(t=>{$('impm').textContent=t});
}
function werksreset(){
 if(!confirm('Werkseinstellungen?\n\nBenutzerwerte UND WLAN werden geloescht.\n'+
   'Der Hall-Offset bleibt - die Kalibrierung geht nicht verloren.\n\n'+
   'Ohne WLAN ist die Uhr danach nur noch am eigenen Zugangspunkt erreichbar.')) return;
 post('/test/werksreset').then(r=>r.text()).then(t=>{$('impm').textContent=t});
}
function importieren(){
 const f=$('imp').files[0];
 if(!f){$('impm').textContent='Erst eine Datei waehlen.';return}
 const l=new FileReader();
 l.onload=()=>{hol('/test/import',{method:'POST',body:l.result})
  .then(r=>r.text().then(t=>{$('impm').textContent=t;t&&z()}))
  .catch(()=>{$('impm').textContent='Import fehlgeschlagen.'})};
 l.readAsText(f);
}
// Die neue Adresse anzeigen, sonst nichts. Frueher standen hier drei Knoepfe
// und ein Countdown, der nach 10 s von selbst hinuebersprang.
//
// WARUM DAS WEG IST (25.08.2026): der Kasten erscheint, solange der Browser
// noch am Zugangspunkt der Uhr haengt. Ein Sprung auf die Heimnetz-Adresse
// fuehrt von dort ins Leere - das Geraet ist in diesem WLAN gar nicht
// erreichbar. Ein Countdown, der einen genau dorthin schiebt, ist kein Dienst.
// "hier bleiben" war die Notbremse dafuer, also mit dem Countdown hinfaellig.
// "jetzt wechseln" war doppelt gemoppelt - der Verweis steht direkt darueber.
// Und "Zugangspunkt abschalten" braucht es nicht: er faellt beim naechsten
// Neustart weg, und auf der Konsole gibt es 'na'.
function zeigNeu(ip,name){
 $('neu').style.display='block';
 const u='http://'+ip+'/', nm='http://'+name+'/';
 $('neu_ip').textContent=u; $('neu_ip').href=u;
 $('neu_nm').textContent=nm; $('neu_nm').href=nm;
 warten_auf(u,'neu_txt');
}

// Antwortet diese Adresse? Von einer fremden Adresse aus darf der Browser die
// Antwort NICHT lesen (andere Herkunft) - aber ob ueberhaupt eine kam, sieht er
// sehr wohl: 'no-cors' liefert eine undurchsichtige Antwort, und die kommt nur
// zustande, wenn jemand geantwortet hat. Genau das reicht als Lebenszeichen.
function erreichbar(u,ms){
 const ac=new AbortController(); const t=setTimeout(()=>ac.abort(),ms||2500);
 return fetch(u+'test/zustand',{mode:'no-cors',cache:'no-store',signal:ac.signal})
  .then(()=>true).catch(()=>false).then(x=>{clearTimeout(t);return x});
}

// Auf eine Adresse warten und dann von selbst hinwechseln.
//
// WARUM NICHT EINFACH EIN COUNTDOWN (25.08.2026): der alte sprang nach 10 s
// blind hinueber. Solange der Browser noch am Zugangspunkt der Uhr haengt, ist
// die Heimnetz-Adresse von dort gar nicht erreichbar - der Sprung landete im
// Nichts, und die Seite war weg. Erst fragen, dann springen: dann geschieht es
// von selbst UND es geht auf.
let wache=null;
function warten_auf(u,feld){
 if(wache) return;
 wache=setInterval(()=>{erreichbar(u).then(ok=>{
  if(!ok) return;
  clearInterval(wache); wache=null;
  if(feld) $(feld).innerHTML='Erreichbar &mdash; wechsle dorthin \u2026';
  location.href=u;
 })},2000);
}
// NETZNAMEN SIND FREMDE DATEN. Jeder in Funkreichweite kann ein WLAN namens
// <img src=x onerror=...> aufspannen - als HTML eingesetzt liefe das in dieser
// Seite, und hier liegt das Token. Deshalb Optionen als Elemente mit
// textContent, nie als zusammengesetztes HTML (26.09.2026; vorher innerHTML).
let wl_beruehrt=false, wl_aktuell='';
function netzOption(ssid,text){const o=document.createElement('option');
 o.value=ssid;o.textContent=text;return o}
function scan(){$('ns').textContent='suche …';
 post('/test/netze').then(r=>r.json()).then(l=>{
  l.sort((a,b)=>b.rssi-a.rssi);
  const s=$('wn'),vorher=s.value||wl_aktuell;s.replaceChildren();
  // Das aktuelle Netz steht immer drin, auch wenn der Suchlauf es gerade
  // nicht sieht - sonst liesse sich nur die Adresse nicht aendern.
  if(wl_aktuell&&!l.some(n=>n.ssid===wl_aktuell))
   s.append(netzOption(wl_aktuell,wl_aktuell+'  (aktuell)'));
  l.forEach(n=>s.append(netzOption(n.ssid,n.ssid+'  ('+n.rssi+' dBm'+
   (n.offen?', offen':'')+(n.ssid===wl_aktuell?', aktuell':'')+')')));
  if(vorher)s.value=vorher;
  $('ns').textContent=l.length?l.length+' gefunden':'keins gefunden - nochmal';
 }).catch(()=>{$('ns').textContent='Suche fehlgeschlagen'})}
function wlArt(){const f=$('wa').value==='fest';wl_beruehrt=true;
 $('wl_fest').style.display=f?'flex':'none'}
// Wer in der Netz-Karte etwas anfasst, dem gehoeren die Felder, bis
// uebernommen ist - derselbe Grundsatz wie beim MQTT-Feld.
['input','change'].forEach(ev=>$('k_netz').addEventListener(ev,()=>wl_beruehrt=true));
function wlan(){const n=$('wn').value;
 if(!n){$('ns').textContent='erst ein Netz waehlen';return}
 const fest=$('wa').value==='fest';
 const d={ssid:n,pass:$('wp').value,art:fest?'fest':'dhcp'};
 if(fest) ['ip','maske','gw','dns'].forEach(k=>d[k]=v('wl_'+k));
 $('ns').textContent='verbinde mit '+n+' …';
 // post() statt fetch(): im Heimnetz verlangt die Uhr das Token. Mit fetch()
 // ging es nur am Zugangspunkt - aus dem Heimnetz liess sich das WLAN nie
 // umstellen (bis 26.09.2026). Und das Passwort steht im Rumpf, nicht in der
 // Adresse.
 post('/test/wlan',d).then(r=>r.text()).then(x=>{$('ns').textContent=x.split('\n')[0];$('wp').value='';
   wl_beruehrt=false;
   // Kommt am Ende der Ersteinrichtung das Token mit, gehoert es ab jetzt
   // diesem Browser. Niemand tippt etwas ab, niemand holt etwas aus einem Log.
   const m=x.match(/TOKEN=(\S+)/);
   if(m){tok=m[1];localStorage.setItem('sw_tok',tok);
    tokDa('Beim Einrichten uebernommen - kein Abtippen noetig.')}})
  .catch(()=>{$('ns').textContent='Verbindung hierher beendet - siehe serielle Ausgabe'})}
// ---- Rampe in Stufen -----------------------------------------------------
// Schritte/s^2 ist keine Groesse, zu der jemand ein Gefuehl hat. Die Stufen
// sind deshalb ueber die ANLAUFZEIT gedacht: wie lange braucht der Zeiger aus
// dem Stand auf Vollgas (8 U/s)?
//
// WARUM ES NACH OBEN EINE GRENZE GIBT: eine zu harte Rampe reisst dem Riemen
// einen Zahn, und das sind 18 Grad = 36 Minuten Anzeigefehler (siehe
// antrieb.h). Erprobt ist einzig 51200 - damit liefen am 23.08.2026 alle acht
// Stufen von 1 bis 8 U/s mit vollstaendigen Hall-Flanken. Die hoechste Stufe
// hier liegt 1,5-mal darueber, nicht doppelt, und sagt das auch.
//
// NACH UNTEN ist die Grenze kein Schutz des Motors, sondern der Anzeige:
// unter 12800 braucht eine Umdrehung so lange, dass kurze Timer nicht mehr
// gehen. Sanfter schadet dem Antrieb nie.
const RAMPEN=[[12800,'sehr sanft'],[25600,'sanft'],[51200,'normal'],
              [64000,'flott'],[76800,'hart']];

// ---- Timer in Stufen ------------------------------------------------------
// Ein linearer Regler von 1 s bis 1 h waere unbrauchbar - 90 % des Weges lagen
// zwischen 50 und 60 Minuten. Deshalb eine Leiter mit brauchbaren Schritten.
// Lichtautomatik: Stufen statt freier Zahlen - die Skala ist logarithmisch,
// und ein linearer Regler von 0,1 bis 2000 lx waere unten nicht bedienbar.
const LA_HELL=[10,20,30,50,100,200,500,1000,2000];  // ab 10 lx (27.10.2026)
const LA_AUS=[0.1,0.2,0.3,0.5,1,2,5,10,20];
function luxText(x){return String(x).replace('.',',')+' lx';}
// Messwerte so genau, wie es im Dunkeln darauf ankommt: 0,52 lx statt 0,5 -
// sonst sah der Messwert genauso aus wie die Schwelle "aus unter 0,5 lx".
function luxZahl(x){return x<1 ? x.toFixed(2) : x<10 ? x.toFixed(1) : String(Math.round(x));}
function naechste(liste,x){let b=0;liste.forEach((w,i)=>{if(Math.abs(Math.log(w/x))<Math.abs(Math.log(liste[b]/x)))b=i});return b;}
const TIMER=[10,30,60,120,180,300,600,900,1200,1800,2700,3600];
function timerText(i){const s=TIMER[i];
 return s<60 ? s+' s' : (s/60)+' min';}

// EIN Knopf, zwei Bedeutungen. Vorher standen "starten" und "abbrechen"
// nebeneinander, und einer der beiden griff immer ins Leere - abbrechen,
// wenn nichts laeuft, ist kein Angebot, sondern eine Falle. Was der Knopf
// gerade tut, sagt seine Aufschrift; DASS etwas laeuft, sagt sein Leuchten.
// Das "// aus" hinter der Aufschrift ist damit weg: es wiederholte nur, was
// die Zustandstabelle ohnehin fuehrt.
let timer_laeuft=false;
function timerKnopf(){
 if(timer_laeuft) c('t-');
 else c('t '+TIMER[$('sl_timer').value]);}

// Zeitzone. Die POSIX-Zeichenkette ist nichts, was jemand tippen will -
// darum eine Auswahl, und das Feld nur noch fuer den Sonderfall.
// WER "eigene ..." WAEHLT, DEM GEHOERT DAS FELD. Vorher schrieb der
// 1-Sekunden-Takt die Auswahl gleich wieder auf die gelebte Zone zurueck und
// klappte das Feld zu - zum Tippen blieb die eine Sekunde zwischen zwei
// Abrufen. Diese Sperre haelt es offen, bis die Zone gesetzt oder eine
// benannte gewaehlt wird.
let tz_eigen_offen=false;
function tzWahl(){const w=$('tzs').value;
 if(w===''){tz_eigen_offen=true;
  $('tz_eigen').style.display='flex'; $('tz').focus(); return}
 tz_eigen_offen=false;
 $('tz_eigen').style.display='none'; $('tz').value=w; c('z '+w);}
// Gesetzt ist gesetzt: ab hier fuehrt der Takt die Anzeige wieder. Steht die
// eigene Zone nicht in der Liste, laesst er das Feld von selbst offen.
function tzEigen(){const w=v('tz'); if(!w) return;
 tz_eigen_offen=false; c('z '+w);}

// Ein Bedienelement nur dann nachfuehren, wenn es NICHT gerade bedient wird.
function stelle(id, f){const e=$(id); if(document.activeElement!==e) f(e);}

// Reiter. Die Wahl ueberdauert das Neuladen - wer in der Werkstatt arbeitet,
// will nach jedem Neustart des Geraets nicht wieder hinklicken.
function reiter(w){
 localStorage.setItem('sw_reiter',w);
 $('bl_bed').style.display = w==='bed' ? '' : 'none';
 $('bl_wrk').style.display = w==='wrk' ? '' : 'none';
 // Der Knopf zeigt, WOHIN er fuehrt, nicht wo man ist: im Uhrbild das
 // Zahnrad, in den Einstellungen die Uhr.
 $('ic_zahn').style.display = w==='bed' ? '' : 'none';
 $('ic_uhr').style.display  = w==='bed' ? 'none' : '';
 $('rt_um').title = w==='bed' ? 'Einstellungen' : 'Uhr';
}
function umschalten(){reiter(localStorage.getItem('sw_reiter')==='wrk'?'bed':'wrk')}

// ===========================================================================
// EINRICHTUNGSMODUS
// ===========================================================================
// Eine Uhr im Auslieferungszustand zeigt NUR das, was jetzt zu tun ist: das
// WLAN eintragen. Alles andere waere Ballast - der Zeiger kann ohne Netz
// keine Zeit kennen, der Timer ist sinnlos, und das Geraeteblatt gibt es am
// offenen Zugangspunkt ohnehin nicht.
//
// WARUM DAS NOETIG WURDE: die Netz-Karte liegt im Block 'wrk', also hinter
// dem Zahnrad. Wer eine frische Uhr zum ersten Mal aufmacht, landet im Block
// 'bed' und sieht ein Zifferblatt, das nichts tut - und muss von selbst
// darauf kommen, in den Einstellungen nach dem WLAN zu suchen. Am 24.09.2026
// genau so passiert.
let setup_an = false;
function setupModus(an){
 if(an === setup_an) return;
 setup_an = an;
 document.body.classList.toggle('setup', an);
 $('einricht').style.display = an ? '' : 'none';
 $('rt_um').style.display    = an ? 'none' : '';
 if(an){
  $('sperre').style.display='none';
  $('bl_bed').style.display='none';
  $('bl_wrk').style.display='';
  // Alle Karten aus, nur die eine an. Ueber die Klasse und nicht ueber CSS,
  // damit hier nichts von der Anordnung im Stylesheet abhaengt.
  document.querySelectorAll('#bl_wrk .k').forEach(k=>{k.style.display='none'});
  $('k_netz').style.display='';
  // Die Liste gleich fuellen. Wer erst "Netze suchen" druecken muss, glaubt,
  // es sei nichts da.
  scan();
 } else {
  document.querySelectorAll('#bl_wrk .k').forEach(k=>{k.style.display=''});
  reiter(localStorage.getItem('sw_reiter')||'bed');
 }
}

let erster_zustand = true;
// --- Farbe -----------------------------------------------------------------
// Drei Regler, 0..255 je Kanal. Linear wie in der Firmware: 128 ist halber
// Strom. Die Helligkeit ist der Regler in der Leiste, nicht diese drei.
let fa_bis=0, fa_letzt=0, fa_wunsch='';
// WIE DIE KLINGE AUSSIEHT, nicht was im Register steht (27.09.2026). Der Wert
// ist ein Tastverhaeltnis, also LINEARES Licht; der Bildschirm erwartet sRGB
// mit Gamma. Und die drei LEDs sind fuers Auge nicht gleich stark: am Geraet
// ist 255/215/100 weiss und 255/128/0 gelb. Daher je Kanal ein Faktor, der
// dieses Weiss auf 1/1/1 bringt, dann Gamma. Ohne das zeigte die Oberflaeche
// das Weiss als Orange und das Gelb als Braunorange.
// SEIT 27.10.2026 JE UHR: das Weiss steht in der Uhr ('lw'), und die
// Faktoren kommen aus ihm. An C4E8 ist es 255/215/100, an BE40 100/255/150.
let WEISS='ffd764', LED_SICHT=[1,255/215,255/100];
function weissSetzen(hex){if(hex===WEISS)return;WEISS=hex;const n=parseInt(hex,16);
 LED_SICHT=[n>>16&255,n>>8&255,n&255].map(x=>255/Math.max(x,1));farbFelder()}
function weissMerken(){c('lw '+FA_REG.map(id=>(+$(id).value).toString(16).padStart(2,'0')).join(''))}
function ledSicht(hex){const n=parseInt(String(hex).replace('#',''),16);
 return '#'+[n>>16&255,n>>8&255,n&255].map((x,i)=>Math.round(255*Math.pow(Math.min(1,x/255*LED_SICHT[i]),1/2.2))
  .toString(16).padStart(2,'0')).join('')}
const FA_REG=['fr','fg','fb'];
function farbAnzeigen(hex,regler){const n=parseInt(hex,16),k=[n>>16&255,n>>8&255,n&255];
 FA_REG.forEach((id,i)=>{if(regler)stelle(id,e=>e.value=k[i]);$(id+'_w').textContent=k[i]});
 ['ff0000','00ff00','0014ff','ff6e00','9b00ff'].forEach(x=>$('fa_'+x).classList.toggle('an',x===hex.toLowerCase()));
 $('fa_weiss').classList.toggle('an',WEISS===hex.toLowerCase())}
// Senden ohne Log-Nachladen: waehrend des Ziehens kaemen sonst Dutzende
// Anfragen. Hoechstens alle 250 ms, und der LETZTE Wert geht immer raus.
function farbFelder(){document.querySelectorAll('#k_farbe button[id^="fa_"] .fp').forEach(e=>{
 const id=e.parentNode.id.slice(3);e.style.background=ledSicht(id==='weiss'?WEISS:id)})}
farbFelder();
// Knopf: mit Zuendung ('lf'). Regler: ohne ('lfr') - er schickt beim Ziehen
// alle 250 ms einen Wert, und jeder wuerde die Zuendung neu anfangen lassen.
let fa_mit_zuendung=true;
function farbeSetzen(hex){fa_mit_zuendung=true;fa_wunsch=hex;farbAnzeigen(hex,true);fa_bis=Date.now()+1500;
 const jetzt=Date.now();clearTimeout(farbeSetzen.t);
 if(jetzt-fa_letzt<250){farbeSetzen.t=setTimeout(()=>farbeSenden(),250-(jetzt-fa_letzt));return}
 farbeSenden()}
function farbeSenden(){fa_letzt=Date.now();post('/test/cmd',{k:(fa_mit_zuendung?'lf ':'lfr ')+fa_wunsch})}
function farbRegler(){const hex=FA_REG.map(id=>(+$(id).value).toString(16).padStart(2,'0')).join('');
 fa_mit_zuendung=false;fa_wunsch=hex;farbAnzeigen(hex,false);fa_bis=Date.now()+1500;
 const jetzt=Date.now();clearTimeout(farbeSetzen.t);
 if(jetzt-fa_letzt<250){farbeSetzen.t=setTimeout(()=>farbeSenden(),250-(jetzt-fa_letzt));return}
 farbeSenden()}

function z(){fetch('/test/zustand').then(r=>r.json()).then(d=>{
 setupModus(!!d.einrichtung);
 if(erster_zustand){
  erster_zustand=false;
  // Das Geraeteblatt erst holen, wenn es ueberhaupt eines geben kann. Beim
  // Laden zu fragen, bevor der Zustand da ist, erzeugte den 401 - und damit
  // genau den roten Balken, der bei der Ersteinrichtung nichts zu suchen hat.
  // Ohne Token holt die Seite es sich zuerst selbst - dann kommt das Blatt mit.
  if(!d.einrichtung) (tok?Promise.resolve():tokHolen()).then(()=>blatt_holen());
 }
 $('net').textContent=d.netz;
 // Wieder da: Zaehler zuruecksetzen, Wartekasten weg.
 weg=0; if($('warte').style.display==='block') $('warte').style.display='none';
 // Die Heimnetz-Adresse ueberdauert einen Neustart nur, wenn sie gemerkt wird.
 if(d.heimnetz) localStorage.setItem('sw_heim','http://'+d.heimnetz+'/');
 // Den Selbsthilfe-Knopf gibt es nur, wo er auch etwas findet.
 if(d.entw) $('sperrb').style.display='inline-block';
 const r=[['Uhrzeit',d.uhr+'  \u00b7 '+d.modus],['Zeitquelle',d.zeitquelle],['Zeiger','Schritt '+d.pos+'  &rarr; '+d.blatt+' auf dem Blatt  &rarr; zeigt '+d.zeigt],
  ['Ziel',d.ziel+(d.faehrt?'  F&Auml;HRT':'  steht')],['F&uuml;hrung',d.fuehrung],
  ['Abweichung',d.drift],['Endstufe',d.endstufe],['Homing','<span class="'+kl(d.homing)+'">'+d.homing+'</span>'],
  ['R&uuml;ckfall',d.rueckfall],['Referenz','<span class="'+(d.referenz==='gueltig'?'ok':'warn')+'">'+d.referenz+'</span>'],['Hall-Offset',d.offset+' Schritte'],['Hall-Sensor',d.hallsensor],['Hall',d.d0],['Hall-Feld',d.feld+' mT  \u00b7 Ruhe '+d.ruhe+' mT'],
  ['Hall-Spanne','<span class="'+d.spanne_kl+'">'+d.spanne+' mT</span>'],
  ['Hall-Fenster',d.fenster],['Umgebungslicht',d.lux],['Licht',d.licht],['Effekt',d.effekt],['Timer',d.timer],
  ['Treiber',d.treiber]];
 $('z').innerHTML=r.map(x=>'<tr><td>'+x[0]+'</td><td class="v">'+x[1]+'</td></tr>').join('');
 $('tmin').textContent=(d.tmin/1000).toFixed(2).replace('.',',')+' s';
 if(document.activeElement!==$('tz')) $('tz').value=d.tz;
 // Steht die gelebte Zeitzone in der Auswahl? Wenn nicht, ist es eine eigene -
 // dann zeigt die Auswahl das auch an, statt stumm auf Wien zu stehen.
 stelle('mq_an',  e=>e.checked=!!d.mq_an);
 if(!mq_beruehrt){
  stelle('mq_host',e=>e.value=d.mq_host);
  stelle('mq_port',e=>e.value=d.mq_port);
  stelle('mq_user',e=>e.value=d.mq_user);
 }
 // Das Passwort kommt nie zurueck. Der Platzhalter sagt trotzdem, woran man
 // ist - sonst sieht ein gesetztes wie ein fehlendes aus.
 stelle('mq_pw',  e=>e.placeholder = d.mq_pw ? 'gesetzt' : 'keins');
 stelle('tzs', e=>{
  if(tz_eigen_offen){e.value=''; return}
  const da=[...e.options].some(o=>o.value===d.tz);
  e.value = da ? d.tz : '';
  $('tz_eigen').style.display = da ? 'none' : 'flex';});

 // ---- Die angepinnte Leiste und die Regler ----------------------------
 // NIE UEBERSCHREIBEN, WAEHREND JEMAND SIE ANFASST. Der Takt laeuft jede
 // Sekunde; zoege er den Schieber unter dem Finger zurueck, waere kein Wert
 // einstellbar. Dasselbe Muster wie bei den Eingabefeldern darueber.
 // EIN Schalter fuer alles. Er steht nur dann auf ein, wenn BEIDES laeuft -
 // Licht und Endstufe. Ein gemischter Zustand entsteht nur noch ueber die
 // Konsole; die Leiste zeigt ihn dann ehrlich als "aus" statt halb an.
 // AUSNAHME NACHT (28.09.2026): hat die Nachtabschaltung die Klinge
 // ausgemacht, laeuft die Uhr weiter - der Schalter bleibt an und sagt
 // "Nacht". Vorher sprang er auf aus, und nach jedem Flashen im dunklen
 // Zimmer sah es aus, als sei die Uhr abgeschaltet.
 const nacht = !!(d.la && d.la.nacht && d.la.sensor && !d.la.nacht_an);
 stelle('tg_alles', e=>e.checked=(!!d.endstufe_an && (!!d.licht_an || nacht)));
 $('tg_alles_lb').textContent = (nacht && !d.licht_an && d.endstufe_an)
   ? 'Betrieb \u00b7 Nacht' : 'Betrieb';
 stelle('tg_ez', e=>e.checked=!!d.eff_start);
 // AUTOMATIK. Die Regler sind Stufen; steht im Geraet ein Wert dazwischen
 // (ueber 'lu' gesetzt), zeigt die Anzeige ihn trotzdem genau.
 if(d.la){
  stelle('tg_la', e=>e.checked=!!d.la.an);
  stelle('tg_ln', e=>e.checked=!!d.la.nacht);
  stelle('sl_lamin', e=>{e.value=Math.round(d.la.min*100);
   $('sl_lamin_w').textContent=Math.round(d.la.min*100)+' % des Reglers'});
  stelle('sl_lahell', e=>{e.value=naechste(LA_HELL,d.la.hell);
   $('sl_lahell_w').textContent=luxText(d.la.hell)});
  stelle('sl_laaus', e=>{e.value=naechste(LA_AUS,d.la.aus);
   $('sl_laaus_w').textContent=luxText(d.la.aus)+' \u00b7 an \u00fcber '+luxText(d.la.ein)});
  ['sl_lamin','sl_lahell'].forEach(k=>$(k).disabled=!d.la.an);
  $('sl_laaus').disabled=!d.la.nacht;
  let t;
  if(!d.la.sensor) t='Kein Lichtsensor \u2014 die Klinge folgt nur dem Regler';
  else if(d.la.lux<0) t='Lichtsensor misst gleich';
  else {
   t='Umgebung '+luxText(luxZahl(d.la.lux))+' \u2192 '+(d.la.an?Math.round(d.la.faktor*100)+' % deines Werts':'Automatik aus');
   // Der Countdown der Nachtabschaltung - beschriftet, damit er nicht als
   // Messwert gelesen wird. Eingeschaltet wird sofort, dort gibt es keinen.
   if(d.la.nacht&&d.la.frist>0) t+=' \u00b7 '+(d.la.nacht_an?'Nachtabschaltung':'Einschalten')+' in '+d.la.frist+' s';
   else if(d.la.nacht&&!d.la.nacht_an) t+=' \u00b7 Nacht, Klinge aus';
  }
  $('la_jetzt').textContent=t;
  $('la_lux').textContent = !d.la.sensor ? 'kein Sensor'
    : d.la.lux<0 ? 'misst gleich' : luxText(luxZahl(d.la.lux));
 }
 stelle('tg_es', e=>e.checked=!!d.eff_stunde);
 // FARBE. Waehrend gezogen wird und kurz danach gewinnt die Hand - sonst
 // sprang der Regler auf den Wert der vorigen Antwort zurueck, bevor der
 // neue angekommen ist.
 if(/^#[0-9a-f]{6}$/i.test(d.weiss||'')) weissSetzen(d.weiss.slice(1).toLowerCase());
 fwZeigen(d);
 if(Date.now()>fa_bis && /^#[0-9a-f]{6}$/i.test(d.farbe||''))
  farbAnzeigen(d.farbe.slice(1),true);
 // MELDUNG: sichtbar, solange eine steht - mit dem Knopf, der sie beendet.
 $('fa_meld').style.display = d.meld_aktiv ? '' : 'none';
 if(d.meld_aktiv){ $('fa_mpt').style.background=ledSicht(d.meld_farbe);
  $('fa_mtx').textContent='Meldung '+(d.meld_farbe||'').toUpperCase()+' \u00b7 '+(d.meld_text||''); }
 // NETZ: was gerade gilt. Die Felder werden nur vorbelegt, solange niemand
 // daran arbeitet - sonst setzte der Sekundentakt jede Eingabe zurueck.
 wl_aktuell=d.wl_ssid||'';
 $('wl_jetzt').textContent = d.heimnetz
  ? 'Verbunden mit '+wl_aktuell+' \u00b7 '+d.wl_ip+' \u00b7 '+d.wl_rssi+' dBm \u00b7 '+(d.wl_fest?'feste IP':'DHCP')
  : (wl_aktuell ? 'Gespeichert: '+wl_aktuell+' \u00b7 nicht verbunden' : 'Kein Heimnetz eingerichtet');
 const imNetz=document.activeElement&&document.activeElement.closest&&document.activeElement.closest('#k_netz');
 if(!wl_beruehrt && !imNetz){
  const s=$('wn');
  if(wl_aktuell && ![...s.options].some(o=>o.value===wl_aktuell))
   s.prepend(netzOption(wl_aktuell,wl_aktuell+'  (aktuell)'));
  if(wl_aktuell) s.value=wl_aktuell;
  $('wa').value=d.wl_fest?'fest':'dhcp';
  $('wl_fest').style.display=d.wl_fest?'flex':'none';
  // Bei DHCP stehen hier die VOM ROUTER vergebenen Werte - wer auf "manuell"
  // umstellt, hat damit eine passende Vorlage statt leerer Felder.
  ['ip','maske','gw','dns'].forEach(k=>$('wl_'+k).value=d['wl_'+k]||'');
 }
 stelle('sl_hell',  e=>{e.value=Math.round(d.licht_aus_proz);
                        $('sl_hell_w').textContent=Math.round(d.licht_aus_proz)+' %'});
 stelle('sl_tempo', e=>{const u=Math.round(d.tempo/d.rev);e.value=u;
                        $('sl_tempo_w').textContent=u+' U/s'});
 stelle('sl_kreise',e=>{e.value=d.kreise;
                        $('sl_kreise_w').innerHTML=d.kreise>0?d.kreise+'&times;':'aus'});
 // Rampe: die naechstliegende Stufe zeigen. Steht im Geraet ein Wert, den
 // keine Stufe trifft (etwa von Hand ueber 'a' gesetzt), sagt die Anzeige das,
 // statt auf die naechste Stufe zu springen und einen falschen Wert zu
 // behaupten.
 stelle('sl_rampe', e=>{
  const i=RAMPEN.findIndex(x=>x[0]===d.rampe);
  if(i>=0){e.value=i; $('sl_rampe_w').textContent=RAMPEN[i][1];}
  else $('sl_rampe_w').textContent='eigen';
 });
 $('ramp_h').innerHTML='Auf '+(d.tempo/d.rev).toFixed(0)+'&nbsp;U/s in <b>'+
  (d.tempo/d.rampe).toFixed(2).replace('.',',')+'&nbsp;s</b>. Erprobt: <b>normal</b>.';
 // Der Knopf folgt dem Geraet, nicht dem letzten Klick: bricht der Timer von
 // selbst ab oder laeuft er ab, steht binnen einer Sekunde wieder "starten"
 // da. Der Zustand kommt aus derselben Zeichenkette wie die Tabelle.
 timer_laeuft = !d.timer.startsWith('aus');
 $('t_btn').textContent = timer_laeuft ? 'abbrechen' : 'starten';
 $('t_btn').className   = timer_laeuft ? 'p an' : 'p';
 ['0','1'].forEach(k=>$('tl_'+k).classList.toggle('an', String(d.t_lauf)===k));
 $('bar_uhr').textContent=d.uhr.slice(0,5);
 // Die Gangart ist nicht mehr einstellbar: Minutensprung ist die Vorgabe und
 // bleibt es. 'm 1' geht weiter ueber die Konsole - fuer die Werkstatt, nicht
 // fuer den Beschenkten.
 $('gb_name').textContent=d.geraet;
 $('gb_serie').textContent=d.serie;
 $('kopf_id').textContent=d.name+'  v'+d.fw;
 // Im Browsertab steht der Name - bei mehreren Uhren der einzige Weg, die
 // Tabs auseinanderzuhalten.
 document.title = d.alias ? d.alias+' \u00b7 Star Watch' : 'Star Watch';
 if(!al_beruehrt) stelle('al', e=>e.value=d.alias);
 $('keff').innerHTML = d.kreise>0
  ? (d.keff_min/1000).toFixed(2).replace('.',',')+' bis '+
    (d.keff_max/1000).toFixed(2).replace('.',',')+' s'
  : 'so lange wie der Weg &mdash; ohne Umweg';
 blatt(d);
 $('ez').innerHTML='<span class="'+kl(d.homing)+'">'+d.homing+'</span>';
 $('eo').textContent=d.offset;
 // Das Feld zeigt den gelebten Wert - aber nie waehrend getippt wird, sonst
 // ueberschreibt der 1-Sekunden-Takt die Eingabe.
 if(document.activeElement!==$('oo')) $('oo').value=d.offset;
 // NUR beim Uebergang zeigen (25.08.2026). Vorher genuegte, dass die Uhr
 // ueberhaupt im Heimnetz haengt - und das tut sie immer. Der Kasten stand
 // damit dauerhaft da und bot an, auf die Adresse zu wechseln, auf der man
 // schon war. Gemeint ist er fuer genau einen Moment: der Browser haengt noch
 // am Zugangspunkt des Uhrwerks, die Uhr hat das WLAN gefunden.
 const hier=location.hostname;
 if(d.heimnetz&&hier!==d.heimnetz&&hier!==d.name){zeigNeu(d.heimnetz,d.name);}
 else {$('neu').style.display='none';}})
 .catch(()=>{$('net').textContent='keine Verbindung';weg++;
  // Drei Fehlschlaege in Folge, nicht einer: ein einzelner verlorener Abruf
  // waehrend einer Fahrt ist normal und darf keinen Kasten aufblenden.
  if(weg>=3) wartemodus()})}
// NEUSTART UND UPDATE: die Uhr ist ein bis zwei Minuten weg. Die Seite soll
// sich danach von SELBST wieder melden - auch wenn das Geraet per DHCP eine
// andere Adresse bekommen hat. Deshalb wird die zuletzt gesehene
// Heimnetz-Adresse gemerkt und im Wartefall MITgeprueft.
let weg=0;
function wartemodus(){
 if($('warte').style.display==='block') return;
 $('warte').style.display='block';
 const heim=localStorage.getItem('sw_heim');
 const hier=location.protocol+'//'+location.host+'/';
 const ziele=[hier]; if(heim&&heim!==hier) ziele.push(heim);
 const puls=setInterval(()=>{
  Promise.all(ziele.map(u=>erreichbar(u).then(ok=>ok?u:null)))
   .then(a=>{const u=a.find(Boolean); if(!u) return;
    clearInterval(puls);
    $('warte_txt').innerHTML='Wieder da &mdash; lade neu \u2026';
    if(u===hier) location.reload(); else location.href=u;
   })},2500);
}

// MARKIEREN, KOPIEREN, LADEN (27.10.2026). Bis dahin schrieb lg() das Log
// alle 1,5 s neu - jede Markierung war weg, bevor man kopieren konnte.
// Jetzt: nur bei neuem Inhalt, und nie, solange darin markiert ist.
// Kopieren ohne navigator.clipboard: das gibt es nur in sicheren Kontexten,
// und die Uhr spricht http.
let log_text='';
function logMarkiert(){const a=getSelection();return a&&!a.isCollapsed&&$('log').contains(a.anchorNode)}
function lg(){fetch('/test/log').then(r=>r.text()).then(x=>{log_text=x;const p=$('log');
 if(logMarkiert()||p.textContent===(x||' '))return;
 const u=p.scrollTop+p.clientHeight>=p.scrollHeight-24;p.textContent=x||' ';if(u)p.scrollTop=p.scrollHeight})
 // Waehrend eines Neustarts scheitert das im 1,5-s-Takt. Ohne dieses catch
 // waere jeder Fehlschlag eine unbehandelte Ablehnung - das Log meldet den
 // Ausfall ohnehin nicht, dafuer ist z() da.
 .catch(()=>{})}
function logKopieren(){const t=document.createElement('textarea');t.value=log_text||$('log').textContent;
 t.style.position='fixed';t.style.opacity='0';document.body.appendChild(t);t.select();
 let ok=false;try{ok=document.execCommand('copy')}catch(e){}t.remove();
 $('logm').textContent=ok?'Kopiert.':'Kopieren ging nicht \u2013 Herunterladen nehmen.'}
function logLaden(){const a=document.createElement('a');
 a.href=URL.createObjectURL(new Blob([log_text||$('log').textContent],{type:'text/plain'}));
 a.download='starwatch-log-'+new Date().toISOString().slice(0,19).replace(/[:T]/g,'-')+'.txt';
 document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),1000)}
reiter(localStorage.getItem('sw_reiter')||'bed');
z();lg();setInterval(z,1000);setInterval(lg,1500);
</script></body></html>)HTML";

/// Zeichen, die JSON nicht roh vertraegt. Nur das Notwendige - die Texte
/// dieser Firmware sind bewusst ohne Umlaute und ohne Sonderzeichen.
String json_text(const String &s) {
  String r;
  r.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '"' || c == '\\')
      r += '\\';
    if (c == '\n') {
      r += "\\n";
      continue;
    }
    r += c;
  }
  return r;
}
}  // namespace

// ===========================================================================
// Doppelausgabe
// ===========================================================================

size_t Doppelausgabe::write(uint8_t c) {
  Serial.write(c);
  ring_[schreib_] = static_cast<char>(c);
  schreib_ = (schreib_ + 1) % GROESSE;
  if (schreib_ == 0)
    umgelaufen_ = true;
  return 1;
}

size_t Doppelausgabe::write(const uint8_t *puffer, size_t laenge) {
  for (size_t i = 0; i < laenge; i++)
    write(puffer[i]);
  return laenge;
}

String Doppelausgabe::log() const {
  String s;
  if (umgelaufen_) {
    s.reserve(GROESSE + 1);
    // Ab der Schreibmarke steht das Aelteste.
    for (size_t i = 0; i < GROESSE; i++) {
      const char c = ring_[(schreib_ + i) % GROESSE];
      if (c != '\0')
        s += c;
    }
  } else {
    s.reserve(schreib_ + 1);
    for (size_t i = 0; i < schreib_; i++)
      s += ring_[i];
  }
  return s;
}

void Doppelausgabe::log_leeren() {
  schreib_ = 0;
  umgelaufen_ = false;
  memset(ring_, 0, GROESSE);
}

// ===========================================================================
// WLAN
// ===========================================================================

void Testweb::funk_drosseln() { SENDELEISTUNG = SENDELEISTUNG_GEDROSSELT; }
bool Testweb::funk_gedrosselt() { return SENDELEISTUNG == SENDELEISTUNG_GEDROSSELT; }

bool Testweb::beginn(Antrieb *antrieb, Hall *hall, Licht *licht, Lux *lux,
                     Betrieb *betrieb, Kommandofn kommando) {
  antrieb_ = antrieb;
  hall_ = hall;
  licht_ = licht;
  lux_ = lux;
  betrieb_ = betrieb;
  betrieb_zeiger = betrieb;
  kommando_ = kommando;
  aktiv = this;

  nvs_oeffnen();
  const String gespeichert = nvs.getString(NVS_SSID, "");
  if (gespeichert.length() > 0) {
    // In NVS eingerichtete Zugangsdaten haben VORRANG vor zugangsdaten.h:
    // sie sind die spaetere, ausdrueckliche Angabe des Benutzers, und sie zu
    // uebergehen waere die unangenehmere Ueberraschung.
    const String pass = nvs.getString(NVS_PASS, "");
    konsole.printf("WLAN aus NVS: verbinde mit '%s' ", gespeichert.c_str());
    WiFi.mode(WIFI_STA);
    funk_einstellen();
    adresse_anwenden();
    WiFi.begin(gespeichert.c_str(), pass.c_str());
    const uint32_t ab = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - ab < WLAN_TIMEOUT_MS) {
      delay(250);
      konsole.print(".");
    }
    konsole.println();
    if (WiFi.status() == WL_CONNECTED) {
      verbunden_ = true;
      eigener_ap_ = false;
      // sta_aktiv_ NICHT vergessen: davon haengt ab, ob die Oberflaeche die
      // Adresse im Heimnetz kennt. Ohne diese Zeile meldete der Zustand nach
      // einem Neustart "eigener Zugangspunkt", obwohl das Geraet im Heimnetz
      // hing - und der gruene Kasten mit der Adresse erschien nie.
      sta_aktiv_ = true;
      konsole.printf("Verbunden, %d dBm. Oberflaeche: http://%s/\n", WiFi.RSSI(),
                     WiFi.localIP().toString().c_str());
      routen_setzen();
      // Der ESP32-Webserver speichert NUR Kopfzeilen, die vorher angemeldet sind.
  // Ohne diese Zeile bliebe `server.header(...)` immer leer, und das Token
  // waere nur ueber den Abfrageteil zu uebergeben.
  {
    // X-Starwatch und Origin fuer den Schutz vor fremden Seiten (CSRF),
    // siehe waechter(). Den Host liefert der Server ohnehin.
    static const char *koepfe[] = {"X-Starwatch-Token", "X-Starwatch", "Origin"};
    server.collectHeaders(koepfe, 3);
  }
  server.begin();
      ota_einrichten();
      return true;
    }
    konsole.println("Netz aus NVS nicht erreichbar - spanne eigenen Zugangspunkt auf.");
    konsole.println("(Mit 'nx' die gespeicherten Zugangsdaten loeschen.)");
  }

#if STARWATCH_HAT_WLAN
  konsole.printf("WLAN: verbinde mit '%s' ", STARWATCH_WLAN_SSID);
  WiFi.mode(WIFI_STA);
  funk_einstellen();  // VOR dem Verbinden, siehe dort
  WiFi.setSleep(false);            // sonst antwortet der Webserver traege
#if defined(STARWATCH_IP) && defined(STARWATCH_GATEWAY) && defined(STARWATCH_MASKE)
  {
    // Bei fester IP MUSS ein DNS-Server gesetzt werden - am Pruefstand
    // gelernt: ohne ihn bleibt die NTP-Zeit ungueltig (ab AP 3.2 relevant).
    const IPAddress ip(STARWATCH_IP), gw(STARWATCH_GATEWAY), maske(STARWATCH_MASKE);
#ifdef STARWATCH_DNS
    const IPAddress dns(STARWATCH_DNS);
#else
    const IPAddress dns(STARWATCH_GATEWAY);
#endif
    WiFi.config(ip, gw, maske, dns);
  }
#endif
  WiFi.begin(STARWATCH_WLAN_SSID, STARWATCH_WLAN_PASSWORT);

  const uint32_t ab = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - ab < WLAN_TIMEOUT_MS) {
    delay(250);
    konsole.print(".");
  }
  konsole.println();

  if (WiFi.status() == WL_CONNECTED) {
    verbunden_ = true;
    eigener_ap_ = false;
    sta_aktiv_ = true;
    konsole.printf("WLAN verbunden, %s dBm. Oberflaeche: http://%s/\n",
                   String(WiFi.RSSI()).c_str(), WiFi.localIP().toString().c_str());
    routen_setzen();
    // Der ESP32-Webserver speichert NUR Kopfzeilen, die vorher angemeldet sind.
  // Ohne diese Zeile bliebe `server.header(...)` immer leer, und das Token
  // waere nur ueber den Abfrageteil zu uebergeben.
  {
    // X-Starwatch und Origin fuer den Schutz vor fremden Seiten (CSRF),
    // siehe waechter(). Den Host liefert der Server ohnehin.
    static const char *koepfe[] = {"X-Starwatch-Token", "X-Starwatch", "Origin"};
    server.collectHeaders(koepfe, 3);
  }
  server.begin();
    ota_einrichten();
    return true;
  }
  konsole.println("WLAN nicht erreichbar - spanne eigenen Zugangspunkt auf.");
#else
  konsole.println("Keine zugangsdaten.h - spanne eigenen Zugangspunkt auf.");
  konsole.println("(Vorlage: lib/testweb/zugangsdaten.example.h)");
#endif

  // =====================================================================
  // Rueckfall: eigener Zugangspunkt - und zwar in ZWEI Ausfuehrungen
  // =====================================================================
  //
  // GEAENDERT AM 23.09.2026. Bis dahin war dieses Netz IMMER mit einem
  // gewuerfelten Schluessel gesichert (AP 6.2). Das ist bei der Einrichtung
  // ein Widerspruch in sich: der Schluessel entsteht beim ersten Start im
  // Geraet, steht nur auf der seriellen Konsole - und wer die hat, braucht
  // den Zugangspunkt gar nicht mehr. Wer sie nicht hat, kommt nicht hinein.
  // Eine frisch geflashte Uhr war damit ohne Kabel nicht einzurichten.
  //
  // Das ist derselbe Fehler wie am 24.08.2026 beim Token, und die Lehre von
  // damals steht im Arbeitsstand: "eine Absicherung, die den letzten Weg zum
  // Geraet zumauert, ist kein Schutz, sondern ein Ausfall."
  //
  // Deshalb jetzt nach Zustand unterschieden:
  //
  //   KEINE Zugangsdaten bekannt  ->  OFFENES Netz. Auslieferungszustand,
  //      es gibt nichts zu schuetzen. Frei ist dort AUSSCHLIESSLICH die
  //      WLAN-Einrichtung - siehe befugt(). Motor, Licht, Geraeteblatt,
  //      Update und Werksreset verlangen auch dort das Token.
  //
  //   Zugangsdaten bekannt, Netz nicht erreichbar  ->  GESICHERTES Netz mit
  //      dem gewuerfelten Schluessel, wie bisher. Das ist der Rettungsweg;
  //      dort darf alles, wer den Schluessel aus dem Geraeteblatt hat. Zu
  //      diesem Zeitpunkt HAT der Besitzer es - die Uhr lief ja schon.
  //
  // Der Schluessel selbst bleibt gewuerfelt und wandert nicht in den
  // Quelltext zurueck: er schuetzt weiterhin den Rettungsweg und das OTA.
  WiFi.mode(WIFI_AP);
  funk_einstellen();
  // Die Seriennummer im Namen ist nicht Zierde: stehen beim Aufbau zwei Uhren
  // nebeneinander, sind sie sonst nicht auseinanderzuhalten.
  const String ap_name = String("StarWatch-") + betrieb_->seriennummer().c_str();
  const String ap_pw = betrieb_->ap_passwort().c_str();

  // Sind ueberhaupt schon Zugangsdaten bekannt - aus NVS oder einkompiliert?
  const bool zugangsdaten_bekannt =
      nvs.getString(NVS_SSID, "").length() > 0 || STARWATCH_HAT_WLAN;
  offener_ap_ = !zugangsdaten_bekannt;

  const bool steht = offener_ap_ ? WiFi.softAP(ap_name.c_str())
                                 : WiFi.softAP(ap_name.c_str(), ap_pw.c_str());
  if (!steht) {
    konsole.println("Zugangspunkt liess sich nicht aufspannen. Es bleibt die\n"
                    "serielle Konsole - die kann alles, was die Oberflaeche kann.");
    return false;
  }
  verbunden_ = true;
  eigener_ap_ = true;
  sta_aktiv_ = false;

  if (offener_ap_) {
    konsole.printf("Zugangspunkt '%s' steht, OHNE Schluessel (Ersteinrichtung).\n"
                   "Dort ist nur die WLAN-Einrichtung frei; alles andere\n"
                   "verlangt das Token aus dem Geraeteblatt.\n",
                   ap_name.c_str());
  } else {
    // Schluessel wie beim OTA-Passwort - siehe dort.
    Serial.printf("Zugangspunkt '%s', Schluessel '%s'.\n", ap_name.c_str(),
                  ap_pw.c_str());
    if (g::ENTWICKLUNGSGERAET)
      konsole.printf("Zugangspunkt '%s' steht, Schluessel '%s' (Rettungsweg -\n"
                     "Zugangsdaten sind bekannt, das Netz ist nicht erreichbar).\n",
                     ap_name.c_str(), ap_pw.c_str());
    else
      konsole.printf("Zugangspunkt '%s' steht (Schluessel im Geraeteblatt).\n",
                     ap_name.c_str());
  }
  konsole.printf("Oberflaeche: http://%s/\n", WiFi.softAPIP().toString().c_str());
  routen_setzen();
  // Der ESP32-Webserver speichert NUR Kopfzeilen, die vorher angemeldet sind.
  // Ohne diese Zeile bliebe `server.header(...)` immer leer, und das Token
  // waere nur ueber den Abfrageteil zu uebergeben.
  {
    // X-Starwatch und Origin fuer den Schutz vor fremden Seiten (CSRF),
    // siehe waechter(). Den Host liefert der Server ohnehin.
    static const char *koepfe[] = {"X-Starwatch-Token", "X-Starwatch", "Origin"};
    server.collectHeaders(koepfe, 3);
  }
  server.begin();
  dns_starten();
  ota_einrichten();
  return true;
}

String Testweb::adresse() const {
  if (!verbunden_)
    return "-";
  return eigener_ap_ ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

// ===========================================================================
// Routen
// ===========================================================================

String Testweb::zustand_json() {
  const int32_t pos = antrieb_->position();
  const int32_t soll = betrieb_->soll_jetzt();
  // Die Aussteuerung aus der letzten Referenzfahrt, sobald es eine gibt.
  // hall_->spanne_mt() zaehlt nur, was seit dem letzten Ruecksetzen
  // vorbeigezogen ist - steht der Zeiger, sind das Zehntel Millitesla, und
  // die Anzeige meldete "zu wenig", obwohl der Magnet 9,85 mT bringt.
  const float spanne_mt = betrieb_->letztes_homing().ok()
                              ? betrieb_->letztes_homing().spanne_mt
                              : hall_->spanne_mt();

  const char *wer = "niemand";
  switch (betrieb_->fuehrung()) {
    case Fuehrung::homing:  wer = "Homing"; break;
    case Fuehrung::hand:    wer = "Hand";          break;
    case Fuehrung::timer:   wer = "Timer";         break;
    case Fuehrung::uhr:     wer = "Uhrbetrieb";    break;
    case Fuehrung::niemand: break;
  }

  // Die Bewertung der Aussteuerung gehoert in die Anzeige, nicht in den Kopf
  // des Ablesenden: ausserhalb des Fensters aus geraet.h ist der Index
  // unsicher.
  const char *spanne_kl = "";
  if (spanne_mt > 0.05f)
    spanne_kl = (spanne_mt < g::HALL_SPANNE_MIN_MT)
                    ? "warn"
                    : ((spanne_mt > g::HALL_SPANNE_MAX_MT) ? "warn" : "ok");

  // Beide Adressen nennen, solange beide laufen - das ist der Zweck des
  // Doppelbetriebs beim Einrichten.
  String netz;
  if (sta_aktiv_ && WiFi.status() != WL_CONNECTED && wlan_weg_seit != 0)
    netz = "HEIMNETZ WEG seit " + String((millis() - wlan_weg_seit) / 1000u) +
           " s - verbindet selbst wieder";
  else if (sta_aktiv_ && WiFi.status() == WL_CONNECTED) {
    netz = "Heimnetz " + WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + " dBm";
    if (WiFi.softAPgetStationNum() >= 0 && eigener_ap_ == false &&
        (WiFi.getMode() & WIFI_AP))
      netz += "   (Zugangspunkt noch aktiv)";
  } else {
    netz = "eigener Zugangspunkt " + adresse();
  }

  String fenster = hall_->fenster_schritte() == 0
                       ? String("noch nicht gemessen")
                       : (String(hall_->fenster_schritte()) + " Schritte");

  String s = "{";
  s += "\"netz\":\"" + json_text(netz) + "\",";
  // Die neue Adresse ausdruecklich als eigenes Feld: die Oberflaeche zeigt sie
  // gross an, sobald sie da ist. Ohne das muesste man sie am Kabel ablesen.
  s += "\"heimnetz\":\"" +
       String((sta_aktiv_ && WiFi.status() == WL_CONNECTED)
                  ? WiFi.localIP().toString()
                  : String()) + "\",";
  // DAS AKTUELLE NETZ fuer die Netz-Karte (26.09.2026): Name, und die
  // Adresse so, wie sie gerade WIRKLICH gilt - bei DHCP also die vom Router
  // vergebene, damit man sie als feste Adresse uebernehmen kann. Das
  // Passwort steht nirgends.
  {
    const bool steht = sta_aktiv_ && WiFi.status() == WL_CONNECTED;
    const Adresswahl a = gespeicherte_adresse();
    s += "\"wl_ssid\":\"" + json_text(steht ? WiFi.SSID() : gespeicherte_ssid()) + "\",";
    s += "\"wl_fest\":" + String(a.fest ? "true" : "false") + ",";
    s += "\"wl_ip\":\"" + (steht ? WiFi.localIP().toString() : a.ip) + "\",";
    s += "\"wl_maske\":\"" + (steht ? WiFi.subnetMask().toString() : a.maske) + "\",";
    s += "\"wl_gw\":\"" + (steht ? WiFi.gatewayIP().toString() : a.gw) + "\",";
    s += "\"wl_dns\":\"" + (steht ? WiFi.dnsIP().toString() : a.dns) + "\",";
    s += "\"wl_rssi\":" + String(steht ? WiFi.RSSI() : 0) + ",";
  }
  s += "\"name\":\"starwatch.local\",";
  s += "\"geraet\":\"" + json_text(String(betrieb_->geraetename().c_str())) + "\",";
  s += "\"alias\":\"" + json_text(String(betrieb_->alias().c_str())) + "\",";
  s += "\"name\":\"" + json_text(String(betrieb_->anzeigename().c_str())) + "\",";
  s += "\"fw\":\"" + String(g::FIRMWARE_VERSION) + "\",";
  s += "\"serie\":\"" + json_text(String(betrieb_->seriennummer().c_str())) + "\",";
  // OTA- UND AP-PASSWORT STEHEN HIER NICHT MEHR (AP 1.3, 24.08.2026).
  // `/test/zustand` ist offen lesbar - haetten die Passwoerter darin gestanden,
  // waere der Schutz der schreibenden Endpunkte wertlos gewesen: wer das
  // OTA-Passwort liest, spielt eigene Firmware ein. Sie kommen jetzt aus
  // `/test/geraeteblatt`, und das verlangt das Token.
  s += "\"uhr\":\"" + String(betrieb_->uhrzeit_text().c_str()) + "\",";
  s += "\"modus\":\"" + String(zeitmodus_text(betrieb_->zeitmodus())) + "\",";
  s += "\"zeitquelle\":\"" +
       String(betrieb_->zeit_aus_netz() ? "Netz (NTP)" : "von Hand / unbekannt") + "\",";
  s += "\"tz\":\"" + json_text(String(betrieb_->zeitzone().c_str())) + "\",";
  // DAS PASSWORT GEHT NICHT MIT. Der Zustand ist offen abrufbar, sobald ein
  // Token vorliegt, und er landet im Browser-Speicher. Ob eines gesetzt IST,
  // genuegt der Oberflaeche vollauf.
  s += "\"mq_an\":" + String(betrieb_->mqtt_an() ? "true" : "false") + ",";
  s += "\"mq_host\":\"" + json_text(String(betrieb_->mqtt_host().c_str())) + "\",";
  s += "\"mq_port\":" + String(betrieb_->mqtt_port()) + ",";
  s += "\"mq_user\":\"" + json_text(String(betrieb_->mqtt_benutzer().c_str())) + "\",";
  s += "\"mq_pw\":" + String(betrieb_->mqtt_passwort().empty() ? "false" : "true") + ",";
  s += "\"pos\":" + String(pos) + ",";
  // Der Zaehler laeuft im Betrieb ueber eine Umdrehung hinaus und wird nicht
  // zurueckgesetzt. "3877 / 3200" liest sich deshalb falsch - die Anzeige
  // nennt beides: den Zaehlerstand und die Lage auf dem Zwoelferblatt.
  s += "\"blatt\":" + String(normiere(pos, g::REV)) + ",";
  s += "\"rev\":" + String(g::REV) + ",";
  // Das FAHRtempo, nicht das, was der Antrieb gerade zufaellig gesetzt hat -
  // die Referenzfahrt und der Uhrbetrieb setzen eigene Werte, und die gehoeren
  // nicht in die Anzeige "so schnell faehrt der Zeiger".
  s += "\"tempo\":" + String(betrieb_->fahrtempo()) + ",";
  s += "\"rampe\":" + String(antrieb_->beschleunigung()) + ",";
  s += "\"kreise\":" + String(betrieb_->effekt_kreise()) + ",";
  s += "\"rueckfall\":\"" +
       String(betrieb_->rueckfall_gesperrt()
                  ? "gesperrt (Einmessen)"
                  : (betrieb_->rueckfall_modus() == Zeitmodus::aus
                         ? "aus"
                         : (betrieb_->rueckfall_rest_ms() > 0
                                ? (String(betrieb_->rueckfall_rest_ms() / 1000 + 1) +
                                   " s -> " +
                                   zeitmodus_text(betrieb_->rueckfall_modus()))
                                : String("bereit")))) + "\",";
  // Dauer eines Sprungs mit Effekt: kuerzester und laengster Fall.
  s += "\"keff_min\":" +
       String(fahrzeit_ms(betrieb_->effekt_kreise() * g::REV + 1,
                          antrieb_->beschleunigung(), betrieb_->fahrtempo())) + ",";
  s += "\"keff_max\":" +
       String(fahrzeit_ms((betrieb_->effekt_kreise() + 1) * g::REV,
                          antrieb_->beschleunigung(), betrieb_->fahrtempo())) + ",";
  // Kuerzeste moegliche Umdrehung, in ms - die Oberflaeche schreibt daraus
  // hin, wie kurz ein Timer sein darf.
  s += "\"tmin\":" +
       String(timer_mindestdauer_ms(g::REV, antrieb_->beschleunigung(),
                                    g::TEMPO_FAHRT)) + ",";
  s += "\"zeigt\":\"" + String(zeigerzeit(pos, g::REV, g::SEK_JE_SCHRITT).c_str()) + "\",";
  s += "\"ziel\":" + String(antrieb_->ziel()) + ",";
  s += "\"faehrt\":" + String(antrieb_->laeuft() ? "true" : "false") + ",";
  s += "\"fuehrung\":\"" + String(wer) + "\",";
  s += "\"drift\":\"" +
       (soll < 0 ? String("-")
                 : String(drift(pos, soll, g::REV)) + " Schritte") + "\",";
  s += "\"endstufe\":\"" +
       String(antrieb_->endstufe_an() ? "bestromt" : "aus \u00b7 Freilauf") + "\",";
  // Als WAHRHEITSWERT fuer die Schalter der Oberflaeche. Aus dem Fliesstext
  // darueber liesse er sich zwar herausklauben - aber dann haenge die
  // Bedienung an einer Formulierung, und die aendert sich irgendwann.
  s += "\"endstufe_an\":" + String(antrieb_->endstufe_an() ? "true" : "false") + ",";
  s += "\"modus_nr\":" + String((int)betrieb_->zeitmodus()) + ",";
  s += "\"rueckfall_nr\":" + String((int)betrieb_->rueckfall_modus()) + ",";
  s += "\"eff_start\":" + String(betrieb_->effekt_start() ? "true" : "false") + ",";
  s += "\"eff_stunde\":" + String(betrieb_->effekt_stunde() ? "true" : "false") + ",";
  s += "\"t_lauf\":" + String((int)betrieb_->timerlauf()) + ",";
  s += "\"homing\":\"" +
       json_text(hominggrund_text(betrieb_->letztes_homing().grund)) + "\",";
  s += "\"offset\":" + String(betrieb_->hall_offset()) + ",";
  // Ob der Uhrbetrieb ueberhaupt laufen DARF. Ohne Referenz steht der Zeiger,
  // und das muss dastehen - sonst sieht es aus, als haenge die Uhr.
  s += "\"referenz\":\"" +
       String(betrieb_->referenz_gueltig()
                  ? "gueltig"
                  : "FEHLT - Uhrbetrieb gesperrt, Zeiger bleibt stehen") +
       "\",";
  s += "\"entw\":" + String(g::ENTWICKLUNGSGERAET ? "true" : "false") + ",";
  // Steht die Uhr noch im Auslieferungszustand? Daran haengt der
  // Einrichtungsmodus der Oberflaeche - siehe setupModus() im Skript.
  s += "\"einrichtung\":" + String(aktiv != nullptr && aktiv->offener_ap()
                                        ? "true" : "false") + ",";
  s += "\"d0\":\"" + String(hall_->magnet_erkannt() ? "MAGNET" : "frei") + "\",";
  // Der Sensor ist ein Steckmodul an J5. Steckt es nicht,
  // kann die Uhr nicht referenzieren - das gehoert in die Zustandstabelle
  // und nicht nur ins Protokoll.
  s += "\"hallsensor\":\"" +
       String(hall_->bereit()
                  ? ("TMAG5273 auf 0x" + String(hall_->sensoradresse(), HEX))
                  : String("FEHLT: ") + tmaggrund_text(hall_->sensorgrund())) +
       "\",";
  s += "\"feld\":\"" + String(hall_->feld_gefiltert(), 2) + "\",";
  s += "\"ruhe\":\"" + String(hall_->ruhe_mt(), 3) + "\",";
  s += "\"spanne\":\"" + String(spanne_mt, 2) + "\",";
  s += "\"spanne_kl\":\"" + String(spanne_kl) + "\",";
  // Umgebungslicht. Auch die Einstellung des Sensors gehoert hinein: ohne
  // sie ist ein Lux-Wert nicht nachzupruefen - dieselbe Zahl kommt bei
  // x1/8 und 25 ms aus einem ganz anderen Zaehlwert als bei x2 und 800 ms.
  {
    // Lichtautomatik fuer die Karte (27.09.2026): Einstellung und Wirkung in
    // EINEM Objekt, damit die Oberflaeche nicht aus Textfeldern zurueckrechnet.
    const Lichtautomatik &la = betrieb_->lichtautomatik();
    Nachtschalter &ns = betrieb_->nachtschalter();
    const bool da = lux_ != nullptr && lux_->bereit();
    const bool gemessen = da && lux_->gemessen();
    const uint32_t lauf = ns.frist_laeuft_ms(millis());
    const uint32_t frist =
        lauf == 0 ? 0 : (lauf >= g::LUX_NACHT_VERZUG_MS ? 0 : (g::LUX_NACHT_VERZUG_MS - lauf + 999) / 1000);
    s += "\"la\":{\"an\":" + String(la.an ? "true" : "false") +
         ",\"nacht\":" + String(la.nacht ? "true" : "false") +
         ",\"min\":" + String(la.anteil_min, 2) + ",\"hell\":" + String(la.lux_hell, 0) +
         ",\"aus\":" + String(la.nacht_aus, 1) + ",\"ein\":" + String(nacht_an_lux(la), 1) +
         ",\"sensor\":" + String(da ? "true" : "false") +
         ",\"lux\":" + String(gemessen ? lux_->lux() : -1.0f, 2) +
         ",\"faktor\":" + String(licht_->dimmfaktor(), 2) +
         ",\"nacht_an\":" + String(ns.an() ? "true" : "false") +
         ",\"frist\":" + String(frist) + "},";
  }
  if (lux_ != nullptr && lux_->bereit() && lux_->gemessen())
    s += "\"lux\":\"" + String(lux_->lux(), 1) + " lx  (Stufe " +
         String(lux_->stufe()) + ", x" + String(lux_->verstaerkung(), 3) + ", " +
         String(lux_->integrationszeit_ms()) + " ms, roh " + String(lux_->rohwert()) +
         ")\",";
  else if (lux_ != nullptr)
    s += "\"lux\":\"" + json_text(luxgrund_text(lux_->grund())) + "\",";
  else
    s += "\"lux\":\"-\",";
  s += "\"fenster\":\"" + json_text(fenster) + "\",";
  // Fuer das Zifferblatt: als ZAHL, nicht im Fliesstext. Die Anzeige soll die
  // Helligkeit faerben koennen, ohne einen Satz zu zerlegen.
  s += "\"licht_an\":" + String(licht_->ist_an() ? "true" : "false") + ",";
  s += "\"licht_aus_proz\":" + String(licht_->ausgabe_prozent(), 1) + ",";
  {
    char farbe[8];
    snprintf(farbe, sizeof(farbe), "#%06lx", (unsigned long)licht_->farbe());
    s += "\"farbe\":\"" + String(farbe) + "\",";
    // Was die Klinge JETZT zeigt - mit Meldung, Zustandssignal und Effekt.
    // Das Zifferblatt malt danach; `farbe` bleibt der eingestellte Wert.
    snprintf(farbe, sizeof(farbe), "#%06lx", (unsigned long)licht_->zeige_farbe());
    s += "\"zeige_farbe\":\"" + String(farbe) + "\",";
    // FIRMWARE-KARTE (27.10.2026): Modus, Kanal und Ergebnis der letzten
    // Suche - als Zahlen und Woerter, damit die Seite nichts zerlegen muss.
    if (aktiv_update_ != nullptr) {
      const Updatekanal k = aktiv_update_->kanal();
      s += "\"upd\":{\"modus\":" + String(static_cast<int>(aktiv_update_->modus())) +
           ",\"kanal\":\"" + String(k == Updatekanal::beta ? "b" : (k == Updatekanal::stabil ? "s" : "e")) +
           "\",\"stand\":" + String(static_cast<int>(aktiv_update_->stand())) +
           ",\"neu\":\"" + json_text(aktiv_update_->gefundene_version().c_str()) +
           "\",\"meldung\":\"" + json_text(aktiv_update_->meldung().c_str()) +
           "\",\"bewaehrung\":" + String(aktiv_update_->auf_bewaehrung() ? "true" : "false") + "},";
    }
    // Weiss DIESER Uhr ('lw') - fuer den Weiss-Knopf und die App.
    if (betrieb_zeiger != nullptr) {
      snprintf(farbe, sizeof(farbe), "#%06lx", (unsigned long)betrieb_zeiger->weiss());
      s += "\"weiss\":\"" + String(farbe) + "\",";
    }
    s += "\"zeige_proz\":" + String(licht_->zeige_prozent(), 1) + ",";
    const auto m = licht_->signal_zustand(Licht::Ebene::meldung);
    s += "\"meld_aktiv\":" + String(m.aktiv ? "true" : "false") + ",";
    if (m.aktiv) {
      snprintf(farbe, sizeof(farbe), "#%06lx", (unsigned long)m.farbe);
      s += "\"meld_farbe\":\"" + String(farbe) + "\",";
      s += "\"meld_text\":\"" + String(muster_text(m.muster)) + ", " +
           (m.rest_ms == 0 ? String("bis quittiert")
                           : String("noch ") + String((m.rest_ms + 59999u) / 60000u) + " min") +
           "\",";
    }
  }
  s += "\"licht\":\"" + String(licht_->ist_an() ? "an" : "aus") + ", Regler " +
       String(licht_->soll_prozent(), 0) + " %, Ausgabe " +
       String(licht_->ausgabe_prozent(), 1) + " %\",";
  s += "\"effekt\":\"" + String(effekt_text(licht_->effekt())) + "\",";
  s += "\"timer\":\"" + String(betrieb_->timer_zustand_text()) +
       ", Rest " + String(betrieb_->timer_restzeit().c_str()) + ", " +
       String(timerlauf_text(betrieb_->timerlauf())) + "\",";
  // NICHT den Treiber abfragen: das blockiert 100 ms am UART, und diese
  // Antwort geht jede Sekunde raus. Der Knopf "Treiber auslesen" tut es.
  s += "\"treiber\":\"" +
       String(antrieb_->endstufe_an() ? "siehe 'Treiber auslesen'" : "Endstufe aus") +
       "\"}";
  return s;
}

/// Bonjour fuer die App (23.10.2026). starwatch.local teilen sich ALLE Uhren
/// im Netz - wer zwei hat, bekam eine beliebige. Deshalb zusaetzlich ein
/// eigener Dienst "_starwatch._tcp" unter "StarWatch-<Serie>", mit Seriennummer
/// und Adresse im TXT: die App findet jede Uhr einzeln und muss den geteilten
/// Namen nie aufloesen. Mehrfach aufrufbar - ein zweites addService scheitert
/// still, der TXT-Eintrag wird ersetzt.
static void mdns_dienste() {
  const String serie =
      betrieb_zeiger != nullptr ? String(betrieb_zeiger->seriennummer().c_str()) : String("");
  MDNS.setInstanceName(String("StarWatch-") + serie);
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("starwatch", "tcp", 80);
  MDNS.addServiceTxt("starwatch", "tcp", "serie", serie);
  const IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  MDNS.addServiceTxt("starwatch", "tcp", "ip", ip.toString());
}

void Testweb::ota_einrichten() {
  ArduinoOTA.setHostname("starwatch");
  // Gemeinsames Entwicklungspasswort, wenn beim Bauen eines gesetzt war -
  // siehe OTA_PASSWORT_ENTWICKLUNG in geraet.h. Sonst das der einzelnen Uhr.
  ArduinoOTA.setPassword(g::OTA_PASSWORT_GEMEINSAM ? g::OTA_PASSWORT_ENTWICKLUNG
                                                   : betrieb_->ota_passwort().c_str());

  ArduinoOTA.onStart([]() {
    update_laeuft = true;
    funk_wach(true);  // im Modem-Sleep brachen Uploads bei 40 % ab
    // ZUERST den Zeiger anhalten. Ein Update, das mitten in einer Fahrt
    // beginnt, laesst den Motor stehen und die Endstufe bestromt - und nach
    // dem Neustart weiss niemand, wo der Zeiger steht. Ausserdem reisst ein
    // Stillstand ohne Rampe dem Riemen einen Zahn ab, und das sind 18 Grad
    // = 36 min Anzeigefehler.
    if (aktiv != nullptr && aktiv->betrieb_ != nullptr) {
      ota_modus_vorher = aktiv->betrieb_->zeitmodus();
      aktiv->betrieb_->stopp();
    }
    if (aktiv != nullptr && aktiv->licht_ != nullptr) {
      ota_licht_war_an = aktiv->licht_->ist_an();
      aktiv->licht_->effekt_abbrechen();
      aktiv->licht_->an(false);
    }
    konsole.println("\nOTA: Update beginnt. Zeiger angehalten, Licht aus.");
  });

  ArduinoOTA.onEnd([]() {
    update_laeuft = false;
    konsole.println("\nOTA: fertig, starte neu.");
  });

  ArduinoOTA.onProgress([](unsigned int fertig, unsigned int gesamt) {
    static unsigned int letztes = 0;
    const unsigned int p = gesamt ? (fertig * 100u / gesamt) : 0u;
    if (p >= letztes + 20) {  // nicht jeden Block melden, das flutet das Log
      letztes = p;
      konsole.printf("OTA: %u %%\n", p);
    }
  });

  ArduinoOTA.onError([](ota_error_t fehler) {
    // AUCH IM FEHLERFALL freigeben - sonst bliebe die Oberflaeche nach einem
    // misslungenen Update fuer immer stumm, und man haette kein Werkzeug mehr,
    // um den Fehler zu suchen.
    update_laeuft = false;
    const char *t = "unbekannt";
    switch (fehler) {
      case OTA_AUTH_ERROR:    t = "Passwort falsch"; break;
      case OTA_BEGIN_ERROR:   t = "Start misslungen - passt das Image in die Partition?"; break;
      case OTA_CONNECT_ERROR: t = "Verbindung abgebrochen"; break;
      case OTA_RECEIVE_ERROR: t = "Empfang abgebrochen"; break;
      case OTA_END_ERROR:     t = "Abschluss misslungen"; break;
    }
    konsole.printf("\nOTA FEHLER: %s\n", t);
    // UND ZURUECK IN DEN BETRIEB: Uhrbetrieb wie vorher, Klinge wieder an
    // (mit Zuendung), wenn sie vorher brannte. Ein laufender Timer ist beim
    // Start abgebrochen worden und bleibt es.
    if (aktiv != nullptr && aktiv->betrieb_ != nullptr &&
        ota_modus_vorher != Zeitmodus::aus)
      aktiv->betrieb_->zeitmodus(ota_modus_vorher);
    if (aktiv != nullptr && aktiv->licht_ != nullptr && ota_licht_war_an)
      aktiv->licht_->zuenden();
    konsole.println("OTA: abgebrochen - Uhr und Klinge laufen weiter wie vorher.");
  });

  // DAS ENTWICKLER-OTA NUR IN DER ENTWICKLUNGSFASSUNG (26.09.2026). Es nimmt
  // UNSIGNIERTE Images an, sobald das Passwort stimmt - in einer ausgelieferten
  // Uhr waere es die Hintertuer an der Signaturpruefung vorbei (siehe Kopf von
  // testweb.h: "faellt weg, wenn AP 3.6 das richtige OTA baut" - das steht
  // laengst). In der Auslieferung kommen Updates nur ueber den signierten Weg.
  //
  // ArduinoOTA.begin() startete nebenbei mDNS; ohne ihn tut es das hier selbst,
  // sonst waere starwatch.local weg. handle() ohne begin() tut nichts
  // (ArduinoOTA.cpp prueft _initialized).
  if (g::ENTWICKLUNGSGERAET) {
    ArduinoOTA.begin();
  } else {
    MDNS.begin(MDNS_NAME);
    konsole.println("Entwickler-OTA aus (Auslieferungsfassung) - Updates nur signiert.");
  }
  log_server.begin();
  log_server.setNoDelay(true);

  // mDNS fuer das Heimnetz. ArduinoOTA startet mDNS schon; der Dienst wird
  // hier ausdruecklich angemeldet, damit die Weboberflaeche auffindbar ist
  // und nicht nur der Update-Port.
  mdns_dienste();

  // WOHIN das Passwort gemeldet wird, haengt an der Fassung. `konsole` schreibt
  // auch in den Ringpuffer, und den liefert `/test/log` OFFEN aus - in der
  // Auslieferung waere das ein Selbsttor: wer es liest, spielt eigene Firmware
  // ein. In der ENTWICKLUNGSfassung muss es dort trotzdem stehen, sonst kommt
  // niemand mehr per OTA an ein Geraet, das nur ueber VPN erreichbar ist.
  // Das GEMEINSAME Passwort wird nie ausgegeben: es steht beim Entwickler im
  // Schluesselbund und gilt fuer jede Test-Uhr - im Log waere es fuer alle
  // zugleich offen.
  if (g::ENTWICKLUNGSGERAET && g::OTA_PASSWORT_GEMEINSAM) {
    konsole.printf("OTA steht (gemeinsames Entwicklungspasswort). Log ueber Netz "
                   "auf Port %u.\n", LOG_PORT);
  } else if (g::ENTWICKLUNGSGERAET) {
    Serial.printf("OTA steht (Passwort '%s').\n", betrieb_->ota_passwort().c_str());
  }
  if (g::ENTWICKLUNGSGERAET) {
    if (!g::OTA_PASSWORT_GEMEINSAM)
      konsole.printf("OTA steht (Passwort '%s'). Log ueber Netz auf Port %u.\n",
                     betrieb_->ota_passwort().c_str(), LOG_PORT);
    // WARUM HIER UND NICHT NUR BEIM START (25.08.2026): der Ringpuffer fasst
    // 4096 Zeichen. Das Blatt stand einmal beim Start darin und war nach
    // Stunden Betrieb herausgerollt - genau in dem Moment, als es gebraucht
    // wurde, weil der Browser das Token verloren hatte. `ota_einrichten()`
    // laeuft bei JEDER Verbindung, also steht es nach jedem Netzwechsel und
    // jedem Wiederverbinden wieder da. Kostet ein paar Zeilen Puffer und ist
    // der einzige Weg zurueck an ein Geraet, an dem kein Kabel haengt.
    konsole.printf("\n--- GERAETEBLATT - AUFSCHREIBEN ---\n%s"
                   "-----------------------------------\n",
                   betrieb_->geraeteblatt().c_str());
  }
  else
    konsole.printf("OTA steht (Passwort im Geraeteblatt). Log ueber Netz auf Port %u.\n",
                   LOG_PORT);
  konsole.printf("Name im Netz: http://%s.local/\n", MDNS_NAME);
}

// ===========================================================================
// ZUGRIFFSSCHUTZ - AP 1.3, entschieden und gebaut am 24.08.2026
// ===========================================================================
// Der Weg stammt aus App-und-HA-Architektur.md Abschnitt 5: die Uhr uebergibt
// bei der KOPPLUNG ein Geraetetoken - kein Aufkleber, keine Passworteingabe.
//
// WAS GESCHUETZT IST, UND WAS NICHT
//   offen      Oberflaeche, Zustand, Log, Diagnose, Netzsuche
//   geschuetzt alles Schreibende (Kommandos, Import, Neustart, Werksreset,
//              WLAN einrichten) UND alles, was Geheimnisse traegt
//              (Geraeteblatt, Export)
//
// Die Trennung nach lesend/schreibend allein reicht NICHT: der Zustand trug
// bis heute das OTA-Passwort mit sich. Wer es liest, spielt eigene Firmware
// ein - der Schutz waere zum Fenster hinaus gewesen. Deshalb ist das
// Kriterium nicht "schreibt es?", sondern "schreibt es ODER verraet es
// etwas?".
//
// AM EIGENEN ZUGANGSPUNKT GILT DAS TOKEN ALS ERBRACHT. Wer dort ist, hat
// dessen Schluessel schon eingegeben - er ist physisch gekoppelt. Genau das
// ist der Moment, den das Architekturdokument als Uebergabepunkt nennt: die
// Ersteinrichtung soll ohne vorher bekanntes Token gehen, sonst ist eine
// frisch ausgepackte Uhr nicht einzurichten.
//
// Das Token kommt als Kopfzeile `X-Starwatch-Token` oder als Abfrageteil
// `?t=`. Die Kopfzeile ist der Regelweg; der Abfrageteil existiert fuer
// einfache Verweise (Export als Download), wo sich keine Kopfzeile setzen
// laesst.
bool befugt() {
  // NUR am GESICHERTEN Zugangspunkt gilt die Kopplung. Am offenen
  // Einrichtungsnetz haette sonst jeder in Funkreichweite vollen Zugriff -
  // samt Geraeteblatt, in dem Token und OTA-Passwort stehen. Genau das waere
  // der Preis dafuer, den Schluessel wegzulassen, und er ist unnoetig: zum
  // Einrichten braucht es nur eine einzige Route, siehe einrichtung_frei().
  if (aktiv != nullptr && aktiv->eigener_ap() && !aktiv->offener_ap())
    return true;  // Kopplung am gesicherten Zugangspunkt
  if (betrieb_zeiger == nullptr)
    return true;  // vor der Einrichtung nichts zu schuetzen
  const String soll = betrieb_zeiger->token().c_str();
  if (soll.length() == 0)
    return true;
  // NUR NOCH DIE KOPFZEILE (26.09.2026). Der Abfrageteil `?t=` ist entfallen:
  // ein Token in der Adresse landet im Browserverlauf, in Proxy-Logs und in
  // jedem kopierten Link. Benutzt hat ihn die Oberflaeche ohnehin nicht mehr.
  if (server.hasHeader("X-Starwatch-Token") &&
      server.header("X-Starwatch-Token") == soll)
    return true;
  return false;
}

// ===========================================================================
// SCHUTZ VOR FREMDEN SEITEN (26.09.2026)
// ===========================================================================
// Bedienen geht im Heimnetz ohne Token - Licht, Timer, Zeiger, Meldungen.
// Das hiess bis heute: JEDE Webseite, die jemand im Heimnetz oeffnet, konnte
// das im Hintergrund ausloesen, mit einem schlichten
//     <img src="http://starwatch.local/test/cmd?k=u+6">
// (CSRF). WLED und Tasmota haben dieselbe Luecke. Drei Sperren schliessen sie:
//
//   1 AENDERUNGEN NUR PER POST MIT DER KOPFZEILE `X-Starwatch`. Eine fremde
//     Seite darf eine eigene Kopfzeile nur senden, wenn das Ziel vorher per
//     CORS zustimmt - die Uhr stimmt nie zu, also blockt der Browser.
//   2 ORIGIN: schickt der Browser eine Herkunft mit, muss es die Uhr selbst
//     sein.
//   3 HOST: gegen DNS-Rebinding. Dabei zeigt eine fremde Domain ploetzlich
//     auf die Uhr, und fuer den Browser ist das "dieselbe Seite" - 1 und 2
//     greifen dann nicht. Abgewiesen werden deshalb OEFFENTLICHE Namen:
//     erlaubt sind IP-Adressen, Namen ohne Punkt und lokale Endungen. So
//     bleibt die Uhr unter jedem Router-Namen erreichbar (fritz.box, .lan).
//
// AM EIGENEN ZUGANGSPUNKT entfallen 2 und 3: dort gibt es kein Internet und
// keine fremde Seite, und die Einrichtung laeuft unter beliebigen Namen
// (captive portal). Die Kopfzeile verlangt 1 trotzdem - die Oberflaeche
// schickt sie immer.

/// Ist das ein Name, unter dem die Uhr erreichbar sein darf?
bool name_erlaubt(String h) {
  h.toLowerCase();
  const int d = h.lastIndexOf(':');
  if (d >= 0 && h.indexOf(']') < 0)  // Port abtrennen (IPv6 hat keine hier)
    h = h.substring(0, d);
  if (h.endsWith("."))
    h = h.substring(0, h.length() - 1);
  if (h.length() == 0)
    return false;
  IPAddress ip;
  if (ip.fromString(h))
    return true;  // eine Adresse ist nie der Name einer fremden Seite
  if (h.indexOf('.') < 0)
    return true;  // Rechnername ohne Domain, ueber den Suchbereich des Routers
  static const char *LOKAL[] = {".local", ".lan", ".home", ".home.arpa",
                                ".internal", ".localdomain", ".fritz.box", ".intranet"};
  for (const char *e : LOKAL)
    if (h.endsWith(e))
      return true;
  return false;
}

/// Der Waechter vor jeder /test-Route. `aendert`: die Route veraendert etwas
/// oder loest etwas aus. Antwortet selbst, wenn er ablehnt.
bool waechter(bool aendert) {
  const bool am_ap = aktiv != nullptr && aktiv->eigener_ap();
  if (!am_ap && !name_erlaubt(server.hostHeader())) {
    server.send(403, "text/plain; charset=utf-8",
                "Abgewiesen: unbekannter Name '" + server.hostHeader() +
                    "'. Die Uhr ueber ihre IP oder starwatch.local aufrufen.");
    return false;
  }
  if (!aendert)
    return true;
  if (server.method() != HTTP_POST || !server.hasHeader("X-Starwatch")) {
    server.send(403, "text/plain; charset=utf-8",
                "Abgewiesen: Aenderungen nur per POST mit Kopfzeile X-Starwatch.");
    return false;
  }
  if (!am_ap && server.hasHeader("Origin")) {
    String o = server.header("Origin");
    const int s = o.indexOf("://");
    if (s >= 0)
      o = o.substring(s + 3);
    // "null" schicken Sandbox-Rahmen und lokale Dateien - ohne Punkt waere es
    // sonst als Rechnername durchgegangen.
    if (o == "null" || !name_erlaubt(o)) {
      server.send(403, "text/plain; charset=utf-8", "Abgewiesen: fremde Herkunft.");
      return false;
    }
  }
  return true;
}

/// Ist dieser Zugriff Teil der ERSTEINRICHTUNG am offenen Zugangspunkt?
///
/// Gilt fuer genau eine Route: das Setzen der WLAN-Zugangsdaten. Das Suchen
/// der Netze braucht es nicht, das war noch nie geschuetzt.
///
/// WARUM NICHT MEHR: alles andere - Motor, Licht, Geraeteblatt, Export,
/// Import, Neustart, Werksreset, MQTT - kann warten, bis die Uhr im Heimnetz
/// haengt und das Token vorliegt. Eine Uhr im Auslieferungszustand hat nichts
/// zu verlieren AUSSER dem, was man ihr gleich anvertraut; und das WLAN-
/// Passwort gibt sie ueber keine Route wieder heraus.
bool einrichtung_frei() {
  return aktiv != nullptr && aktiv->offener_ap();
}

/// Verlangt dieses Konsolenkommando das Token?
///
/// ===========================================================================
/// WOVOR DAS TOKEN SCHUETZT - und wovor nicht mehr (24.09.2026)
/// ===========================================================================
/// Bis heute sass es vor `/test/cmd` und damit vor JEDER Bedienung: Licht an,
/// Timer stellen, Helligkeit aendern. Genau deshalb begegnete es dem
/// Benutzer ueberhaupt - und eine Aufforderung, ein Token aus einem Log zu
/// holen, ist fuer jemanden, der nur das Licht anmachen will, sinnlos.
///
/// Der Angreifer waere jemand, der SCHON im Heimnetz ist. Dass der die
/// Klinge umschalten oder den Zeiger drehen kann, ist eine Laestigkeit, kein
/// Schaden - und dagegen ein Tokenfeld aufzustellen, kostet jeden ehrlichen
/// Benutzer mehr, als es den unehrlichen behindert.
///
/// GESCHUETZT BLEIBT, was nicht rueckgaengig zu machen ist oder was das
/// Geraet aus der Hand gibt:
///
///   n...  Netzkommandos. `nx` vergisst die Zugangsdaten, `ns` setzt neue -
///         damit nimmt man die Uhr aus dem Netz oder in ein fremdes hinein.
///   y...  Updates. `yi` spielt Firmware ein.
///   b     Neustart und `b werksreset`.
///   o     Offset. Ein falscher Offset heisst: die Uhr zeigt dauerhaft die
///         falsche Zeit und behauptet, kalibriert zu sein. Das ist die
///         teuerste stille Fehlfunktion, die dieses Geraet kennt.
///   e     Endstufe aus. Dann ist der Motor stromlos und DIE KLINGE KIPPT
///         aus ihrer Lage - das einzige Kommando mit koerperlicher Folge.
///
/// FREI IST DER REST: Zeit, Zeigerfahrten, Prueflaeufe, Homing, Timer,
/// Licht, Messen und Diagnose.
///
/// SCHWARZE LISTE, NICHT WEISSE, und das ist eine bewusste Abwaegung. Eine
/// weisse Liste waere sicherer, aber ein neu hinzugekommenes harmloses
/// Kommando waere dann still gesperrt, und niemand wuesste warum. Hier ist
/// die Abwaegung vertretbar, weil das Token kein Sicherheitswall ist,
/// sondern ein Schutz gegen Versehen und Laestigkeit.
///
/// WER EIN KOMMANDO HINZUFUEGT, das Einstellungen dauerhaft aendert, das
/// Geraet aus dem Netz nimmt oder etwas Koerperliches ausloest, traegt den
/// Buchstaben HIER ein. Sonst ist es offen.
bool kommando_braucht_token(const String &k) {
  if (k.length() == 0)
    return true;
  switch (k[0]) {
    case 'n':  // Netz: n, nl, ns, na, nr, nx
    case 'y':  // Updates: y, yp, yi
    case 'b':  // Neustart, Werksreset
    case 'o':  // Offset - stille Fehlfunktion, wenn falsch
    case 'e':  // Endstufe aus - die Klinge kippt
      return true;
    case 'i':  // 'ih <mA>' - zu wenig Haltestrom, und die Klinge sackt ab
      return k.length() > 2 && k[1] == 'h';
    default:
      return false;
  }
}

/// Antwort auf einen Zugriff ohne Token.
void abgewiesen() {
  const bool einrichtung = aktiv != nullptr && aktiv->offener_ap();
  server.send(401, "text/plain; charset=utf-8",
              einrichtung
                  ? "Kein gueltiges Token.\n\n"
                    "Diese Uhr steht im Auslieferungszustand und spannt ein "
                    "OFFENES Netz auf.\nDort ist absichtlich nur eines frei: "
                    "das Eintragen der WLAN-Zugangsdaten\n(Karte 'Netz'). "
                    "Alles andere waere sonst fuer jeden in Funkreichweite "
                    "offen.\n\n"
                    "Trag das WLAN ein. Danach steht das Token im "
                    "Geraeteblatt.\n"
                  : "Kein gueltiges Token.\n\n"
                    "Diese Uhr verlangt fuer Bedienung und Geheimnisse ein "
                    "Token (AP 1.3).\nEs steht im Geraeteblatt: beim Start auf "
                    "der Konsole, oder in der\nOberflaeche, solange du am "
                    "gesicherten Zugangspunkt der Uhr haengst.\n");
}

/// Wohin das Captive Portal umleitet.
///
/// GERECHNET, NICHT ANGENOMMEN: hier stand unbedingt `WiFi.softAPIP()`. Laeuft
/// der eigene Zugangspunkt nicht - der Normalfall, sobald die Uhr im Heimnetz
/// haengt -, liefert das `0.0.0.0`, und die Umleitung zeigte ins Leere. Im
/// eigentlichen Anwendungsfall (der Client haengt AM Zugangspunkt) fiel das
/// nie auf, weil er dann laeuft. Am 24.08.2026 beim Nachpruefen gefunden.
String portal_ziel() {
  const IPAddress ap = WiFi.softAPIP();
  if (ap != IPAddress(0, 0, 0, 0))
    return String("http://") + ap.toString() + "/";
  if (WiFi.status() == WL_CONNECTED)
    return String("http://") + WiFi.localIP().toString() + "/";
  return String("/");  // letzter Ausweg: gleiche Adresse, Wurzelpfad
}

void Testweb::dns_starten() {
  // Alles auf die eigene Adresse. "*" ist die Catch-all-Form der Bibliothek -
  // damit beantwortet das Geraet nicht nur star.watch, sondern jeden Namen,
  // und genau das loest bei iOS und Android das "Anmelden"-Fenster aus.
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns_laeuft = dns.start(53, "*", WiFi.softAPIP());
  if (dns_laeuft)
    konsole.printf("Name am Zugangspunkt: http://%s/  (jeder Name fuehrt hierher)\n",
                   EIGENER_NAME);
  else
    konsole.println("DNS liess sich nicht starten - es bleibt die IP 192.168.4.1.");
}

void Testweb::dns_beenden() {
  if (!dns_laeuft)
    return;
  dns.stop();
  dns_laeuft = false;
}

void Testweb::log_tick() {
  // Neuen Zuhoerer annehmen. Nur einer: der bestehende wird abgeloest, sonst
  // haengt eine tote Verbindung den Port dauerhaft zu.
  if (log_server.hasClient()) {
    if (log_client)
      log_client.stop();
    log_client = log_server.available();
    log_gesendet = konsole.log().length();  // nicht die ganze Vorgeschichte
    log_befugt = false;                     // jede Verbindung faengt von vorn an
    log_client.println("\n--- Star Watch, Log ueber Netz. Eingaben gehen an die "
                       "Konsole. ---");
  }
  if (!log_client || !log_client.connected())
    return;

  // Was seit dem letzten Mal dazugekommen ist, nachschicken.
  const String jetzt = konsole.log();
  if (jetzt.length() > log_gesendet) {
    log_client.print(jetzt.substring(log_gesendet));
    log_gesendet = jetzt.length();
  } else if (jetzt.length() < log_gesendet) {
    log_gesendet = jetzt.length();  // Ringpuffer ist umgelaufen oder geleert
  }

  // Eingaben von dort wie Konsoleneingaben behandeln - dieselbe Regel wie
  // ueberall: alle Wege fuehren dieselben Kommandos aus.
  while (log_client.available()) {
    static char puffer[g::KOMMANDO_MAX];
    static size_t len = 0;
    static bool zu_lang = false;
    const char c = log_client.read();
    if (c == '\r' || c == '\n') {
      if (len > 0) {
        puffer[len] = '\0';
        if (zu_lang) {
          konsole.printf("\n[netz] ABGEWIESEN: Kommando laenger als %u "
                         "Zeichen.\n", (unsigned)g::KOMMANDO_MAX - 1);
        } else if (strncmp(puffer, "token ", 6) == 0) {
          // Anmelden am Log-Port. Beim Absichern der Weboberflaeche waere
          // dieser Weg fast uebersehen worden: Port 23 fuehrt Kommandos aus
          // und war voellig offen - er ist damit genauso maechtig wie
          // /test/cmd, nur ohne Tuer.
          log_befugt = (betrieb_zeiger != nullptr &&
                        String(puffer + 6) == betrieb_zeiger->token().c_str());
          log_client.println(log_befugt ? "Token gilt. Kommandos frei."
                                        : "Token stimmt nicht.");
        } else if (log_befugt || (aktiv != nullptr && aktiv->eigener_ap() &&
                                  !aktiv->offener_ap())) {
          // 'pz' IST KEIN KOMMANDO, SONDERN EINE FRAGE, und sie wird hier
          // beantwortet statt in main.cpp - weil die Antwort NUR an den
          // Fragenden gehen darf. Ueber `konsole` liefe sie in den Ringpuffer,
          // und der fasst 4096 Zeichen: bei fuenf Fragen je Sekunde waere das
          // Log nach einer halben Minute mit Positionsmeldungen vollgelaufen
          // und alles Brauchbare herausgerollt.
          //
          // Die App braucht das, um den Zeiger auf dem Schirm aus der
          // ISTPOSITION zu zeichnen statt aus dem, was sie selbst befohlen
          // hat. Der Motor hinkt einem schnellen Ziel hinterher; wer die
          // Absicht zeichnet, zeigt etwas, das es an der Wand nicht gibt.
          if (strcmp(puffer, "pz") == 0) {
            if (antrieb_ != nullptr)
              log_client.printf("#p %ld %ld\n", (long)antrieb_->position(),
                                (long)antrieb_->ziel());
            len = 0;
            zu_lang = false;
            continue;
          }
          // 'ps' kommt beim Steuern mehrmals je Sekunde (siehe main.cpp):
          // mitgeschrieben wuerde es die Verbindung mit der Antwort auf die
          // eigenen Befehle fluten.
          if (!(puffer[0] == 'p' && puffer[1] == 's'))
            konsole.printf("\n[netz] %s\n", puffer);
          ausfuehren(puffer);
        } else {
          log_client.println(
              "Kein Token. Zum Mitlesen genuegt die Verbindung; fuer Kommandos\n"
              "zuerst eine Zeile 'token <wert>' senden. Der Wert steht im\n"
              "Geraeteblatt (serielle Konsole oder Oberflaeche).");
        }
        len = 0;
        zu_lang = false;
      }
    } else if (len < sizeof(puffer) - 1) {
      puffer[len++] = c;
    } else {
      zu_lang = true;  // bis zum Zeilenende weiterlesen, dann melden
    }
  }
}

void Testweb::routen_setzen() {
  // JEDE Anfrage zaehlt als Bedienung und weckt den Funk (siehe funk_wach).
  // Die erste nach dem Schlaf ist dadurch nur einmal etwas langsamer.
  server.addMiddleware([](WebServer &s, Middleware::Callback weiter) {
    (void)s;
    letzte_bedienung_ms = millis();
    funk_wach(true);
    return weiter();
  });

  // SCHUTZ VOR FREMDEN SEITEN: jede Route geht durch waechter() - lesende mit
  // false (nur der Host wird geprueft), aendernde mit true (POST, Kopfzeile
  // X-Starwatch, Origin). Begruendung dort.
  server.on("/", HTTP_GET, []() {
    if (!waechter(false))
      return;
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", SEITE);
  });

  server.on("/test/zustand", HTTP_GET, []() {
    if (aktiv == nullptr || !waechter(false))
      return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", aktiv->zustand_json());
  });

  server.on("/test/log", HTTP_GET, []() {
    if (!waechter(false))
      return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", konsole.log());
  });

  // Leeren veraendert etwas - also POST, wie jede Aenderung.
  server.on("/test/log", HTTP_POST, []() {
    if (!waechter(true))
      return;
    konsole.log_leeren();
    server.send(200, "text/plain; charset=utf-8", "Log geleert.");
  });

  server.on("/test/cmd", HTTP_POST, []() {
    if (!waechter(true))
      return;
    if (aktiv == nullptr) {
      server.send(500, "text/plain", "keine Kommandofunktion");
      return;
    }
    String k = server.arg("k");
    if (k.length() == 0) {
      server.send(400, "text/plain", "kein Kommando");
      return;
    }
    // NICHT die Route, sondern das KOMMANDO entscheidet - siehe
    // kommando_braucht_token(). Bedienung ist im Heimnetz frei; Eingriffe
    // verlangen weiterhin das Token.
    if (kommando_braucht_token(k) && !befugt()) {
      abgewiesen();
      return;
    }
    // Erst antworten, dann ausfuehren: `h` und `g` dauern Sekunden bis
    // Minuten, und der Browser soll nicht darauf warten. Das Ergebnis holt
    // er sich aus der Mitschrift.
    server.send(200, "text/plain", "ok");

    // ABWEISEN, NICHT KUERZEN. Hier stand `strncpy` in einen 80-Byte-Puffer,
    // und ein laengeres Kommando wurde stillschweigend abgeschnitten und
    // trotzdem ausgefuehrt - bei `yu <url>` also mit halber Adresse. Die
    // Antwort ist zu diesem Zeitpunkt schon raus, deshalb geht die Meldung
    // ins Log.
    if (k.length() >= g::KOMMANDO_MAX) {
      konsole.printf("\n[web] ABGEWIESEN: Kommando ist %u Zeichen lang, "
                     "hoechstens %u sind erlaubt.\n",
                     (unsigned)k.length(), (unsigned)g::KOMMANDO_MAX - 1);
      return;
    }
    char puffer[g::KOMMANDO_MAX];
    strncpy(puffer, k.c_str(), sizeof(puffer) - 1);
    puffer[sizeof(puffer) - 1] = '\0';
    konsole.printf("\n[web] %s\n", puffer);
    aktiv->ausfuehren(puffer);
  });

  // =========================================================================
  // AP 3.7.2 - Export, Import, Neustart, Werksreset
  // =========================================================================

  server.on("/test/geraeteblatt", HTTP_GET, []() {
    if (aktiv == nullptr || betrieb_zeiger == nullptr || !waechter(false))
      return;
    // GESCHUETZT: hier stehen OTA-Passwort, AP-Schluessel und das Token
    // selbst. Am eigenen Zugangspunkt offen - das ist der Kopplungsmoment.
    if (!befugt()) {
      abgewiesen();
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    // Am Ende maschinenlesbar, damit die Seite das Token uebernehmen kann,
    // ohne dass jemand es abtippt. Genau der Kopplungsmoment, den das
    // Architekturdokument meint: "kein Aufkleber, keine Passworteingabe".
    server.send(200, "text/plain; charset=utf-8",
                String("GERAETEBLATT - aufschreiben\n"
                       "==========================\n") +
                    betrieb_zeiger->geraeteblatt().c_str() +
                    "\nWLAN-QR: WIFI:T:WPA;S:StarWatch-" +
                    betrieb_zeiger->seriennummer().c_str() + ";P:" +
                    betrieb_zeiger->ap_passwort().c_str() + ";;\n" +
                    "TOKEN=" + betrieb_zeiger->token().c_str() + "\n");
  });

  // DAS TOKEN HOLT SICH DIE SEITE SELBST (27.10.2026, Fabian: "kein Grund,
  // dass der Benutzer etwas von Hand machen muss, egal ob App oder Web").
  // Im Heimnetz gibt die Uhr es heraus - an jeden, der POST mit X-Starwatch
  // schickt und keine fremde Herkunft hat (waechter). Damit schuetzt das
  // Token noch vor fremden Webseiten, die diese Kopfzeile nicht setzen
  // koennen, nicht mehr vor Geraeten im Heimnetz. So entschieden.
  // NICHT am offenen Einrichtungsnetz: dort waere es fuer jeden in
  // Funkreichweite. Das Token kommt dann am Ende der Einrichtung mit.
  server.on("/test/token", HTTP_POST, []() {
    if (aktiv == nullptr || betrieb_zeiger == nullptr || !waechter(true))
      return;
    if (einrichtung_frei()) {
      abgewiesen();
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", betrieb_zeiger->token().c_str());
  });

  server.on("/test/diagnose", HTTP_GET, []() {
    if (aktiv == nullptr || !waechter(false))
      return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", aktiv->diagnose_text());
  });

  server.on("/test/export", HTTP_GET, []() {
    if (aktiv == nullptr || !waechter(false))
      return;
    // Traegt den Netznamen und ist eine Einstellungsdatei - geschuetzt.
    if (!befugt()) {
      abgewiesen();
      return;
    }
    // OHNE HALL-OFFSET. Der Fertigungsbereich wird nicht exportiert - eine
    // Datei von einer anderen Uhr wuerde sonst deren Kalibrierung mitbringen
    // und diese hier verstellen (AP 3.1).
    //
    // UND OHNE WLAN-PASSWORT. Der Umsetzungsplan sieht es "auf ausdrueckliche
    // Anforderung" vor - das setzt aber AP 1.3 (Zugriffsschutz) voraus, und
    // den gibt es noch nicht. Dieser Endpunkt ist UNGESICHERT: wer im Netz
    // ist, kann ihn abrufen. Ein Passwort hier hineinzuschreiben heisst, den
    // WLAN-Schluessel an jeden im Netz zu geben. Aus demselben Grund gibt es
    // in dieser Klasse absichtlich keinen Passwort-Getter.
    server.sendHeader("Content-Disposition",
                      "attachment; filename=\"star-watch-einstellungen.json\"");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", aktiv->einstellungen_json());
  });

  server.on("/test/import", HTTP_POST, []() {
    if (aktiv == nullptr || !waechter(true))
      return;
    if (!befugt()) {
      abgewiesen();
      return;
    }
    const String koerper = server.hasArg("plain") ? server.arg("plain") : String();
    if (koerper.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "Leere Datei.");
      return;
    }
    String meldung;
    const bool ok = aktiv->einstellungen_uebernehmen(koerper, &meldung);
    server.send(ok ? 200 : 400, "text/plain; charset=utf-8", meldung);
  });

  server.on("/test/neustart", HTTP_POST, []() {
    if (!waechter(true))
      return;
    if (!befugt()) {
      abgewiesen();
      return;
    }
    server.send(200, "text/plain; charset=utf-8", "Neustart in 2 s.");
    konsole.println("\n[web] Neustart angefordert.");
    neustart_ab_ms = millis();
    neustart_mit_reset = false;
  });

  server.on("/test/werksreset", HTTP_POST, []() {
    if (!waechter(true))
      return;
    if (!befugt()) {
      abgewiesen();
      return;
    }
    server.send(200, "text/plain; charset=utf-8",
                "Werksreset: Benutzerwerte und WLAN geloescht, "
                "Hall-Offset bleibt. Neustart in 2 s.");
    konsole.println("\n[web] WERKSRESET angefordert. Der Hall-Offset bleibt (AP 3.1).");
    neustart_ab_ms = millis();
    neustart_mit_reset = true;
  });

  // POST, obwohl nur gelesen wird: der Suchlauf haelt die Uhr rund zwei
  // Sekunden an. Das soll keine fremde Seite ausloesen koennen.
  server.on("/test/netze", HTTP_POST, []() {
    if (aktiv == nullptr || !waechter(true))
      return;
    // Jedes Netz EINMAL, mit dem staerksten Zugangspunkt (23.10.2026): bei
    // mehreren APs mit demselben Namen stand "Voldemodem" sonst dreimal da.
    // Welcher AP es wird, entscheidet ohnehin die Uhr beim Verbinden.
    const int n = WiFi.scanNetworks();
    std::vector<Funknetz> v;
    for (int i = 0; i < n; i++) {
      Funknetz f;
      f.ssid = WiFi.SSID(i).c_str();
      f.rssi = WiFi.RSSI(i);
      f.offen = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      v.push_back(f);
    }
    WiFi.scanDelete();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", netze_json(v, 8192).c_str());
  });

  server.on("/test/wlan", HTTP_POST, []() {
    if (!waechter(true))
      return;
    // Die EINZIGE Route, die am offenen Einrichtungsnetz ohne Token geht.
    if (!befugt() && !einrichtung_frei()) {
      abgewiesen();
      return;
    }
    if (aktiv == nullptr)
      return;
    // Erst antworten, dann umschalten: die Verbindung zum Browser bricht beim
    // Wechsel vom Zugangspunkt in das Heimnetz ohnehin ab.
    const String ssid = server.arg("ssid");
    String pass = server.arg("pass");

    // GLEICHES NETZ, LEERES PASSWORT: das gespeicherte gilt weiter. Wer nur
    // die Adresse von DHCP auf fest umstellt, soll dafuer nicht das
    // WLAN-Passwort des Hauses heraussuchen muessen.
    if (pass.length() == 0 && ssid.length() > 0 && ssid == aktiv->gespeicherte_ssid())
      pass = aktiv->gespeichertes_passwort();

    // ADRESSE: nur wenn mitgeschickt ('art' = dhcp | fest). Ohne 'art' bleibt
    // die gespeicherte Einstellung - so bleibt die Route fuer die
    // Ersteinrichtung genau so einfach wie vorher.
    Testweb::Adresswahl adr;
    const bool mit_adresse = server.hasArg("art");
    if (mit_adresse) {
      adr.fest = server.arg("art") == "fest";
      adr.ip = server.arg("ip");
      adr.maske = server.arg("maske");
      adr.gw = server.arg("gw");
      adr.dns = server.arg("dns");
      String fehler;
      if (ssid.length() > 0 && !Testweb::adresse_pruefen(adr, &fehler)) {
        // NICHTS SPEICHERN. Eine falsche feste Adresse macht die Uhr im
        // Heimnetz unerreichbar, obwohl das WLAN verbunden ist - und dann
        // oeffnet sie auch keinen Zugangspunkt mehr.
        server.send(400, "text/plain", "Nicht gespeichert: " + fehler);
        return;
      }
    }

    // ===================================================================
    // DAS TOKEN WIRD HIER UEBERGEBEN - das ist der Kopplungsmoment.
    // ===================================================================
    // Wer am offenen Zugangspunkt die Einrichtung abschliesst, stand
    // physisch in Funkreichweite und hat der Uhr gerade das WLAN-Passwort
    // des Hauses anvertraut. Genau das ist die Kopplung, und genau dann
    // gehoert ihm das Token - nicht erst, nachdem er es aus einem Log
    // abgeschrieben hat.
    //
    // WARUM ES DAS BRAUCHT: bis 24.09.2026 galt am Zugangspunkt
    // `befugt() == true`, die Oberflaeche holte sich das Token beim Koppeln
    // also von selbst. Mit dem offenen Netz faellt das weg, und uebrig
    // bliebe der Knopf "aus dem Log holen" - eine Behelfsloesung aus der
    // Entwicklung, die in einer Ersteinrichtung nichts zu suchen hat.
    //
    // Der Empfaenger ist nicht beliebig: er muss die Einrichtung ABSCHLIESSEN,
    // um an die Zeile zu kommen. Wer das kann, hat die Uhr ohnehin in der
    // Hand.
    String antwort =
        ssid.length()
            ? "Verbinde mit '" + ssid +
                  "'. Diese Seite bleibt erreichbar - die neue Adresse "
                  "erscheint oben, sobald die Verbindung steht."
            : "Zugangsdaten geloescht.";
    // Feste Adresse: die neue ist schon jetzt bekannt - sagen, wohin es geht.
    if (ssid.length() > 0 && mit_adresse && adr.fest)
      antwort = "Verbinde mit '" + ssid + "' unter der festen Adresse " + adr.ip +
                ". Danach: http://" + adr.ip + "/";
    if (ssid.length() > 0 && einrichtung_frei() && betrieb_zeiger != nullptr)
      antwort += "\nTOKEN=" + String(betrieb_zeiger->token().c_str());
    server.send(200, "text/plain", antwort);
    delay(200);
    aktiv->wlan_setzen(ssid, pass, mit_adresse ? &adr : nullptr);
  });

  server.on("/test/mqtt", HTTP_POST, []() {
    if (!waechter(true))
      return;
    if (!befugt()) {
      abgewiesen();
      return;
    }
    if (aktiv == nullptr || betrieb_zeiger == nullptr)
      return;
    betrieb_zeiger->mqtt_an(server.arg("an") == "1");
    betrieb_zeiger->mqtt_host(server.arg("host").c_str());
    betrieb_zeiger->mqtt_port((uint16_t)server.arg("port").toInt());
    betrieb_zeiger->mqtt_benutzer(server.arg("user").c_str());
    // LEERES FELD HEISST UNVERAENDERT, nicht "loeschen". Der Zustand gibt das
    // Passwort nie heraus, also steht im Feld nach jedem Neuladen nichts -
    // wuerde leer als "loeschen" gelten, raeumte jedes Speichern der uebrigen
    // Werte das Passwort mit weg.
    const String pw = server.arg("pw");
    if (pw.length() > 0)
      betrieb_zeiger->mqtt_passwort(pw.c_str());
    server.send(200, "text/plain; charset=utf-8",
                betrieb_zeiger->mqtt_bereit()
                    ? "Gemerkt."
                    : "Gemerkt - ohne Haken und Adresse wird nichts versucht.");
  });

  // Captive-Portal-Erkennung. iOS, Android und Windows rufen nach dem
  // Verbinden je eine feste Adresse auf, um zu pruefen, ob sie ins Internet
  // kommen. Antwortet man mit einer Umleitung, oeffnet das Betriebssystem die
  // Seite von selbst - dasselbe "Anmelden"-Fenster wie in einem Hotel-WLAN.
  auto umleiten = []() {
    server.sendHeader("Location", portal_ziel(), true);
    server.send(302, "text/plain", "");
  };
  server.on("/hotspot-detect.html", HTTP_GET, umleiten);   // iOS, macOS
  server.on("/generate_204", HTTP_GET, umleiten);          // Android
  server.on("/gen_204", HTTP_GET, umleiten);               // Android, aelter
  server.on("/connecttest.txt", HTTP_GET, umleiten);       // Windows
  server.on("/ncsi.txt", HTTP_GET, umleiten);              // Windows
  server.on("/canonical.html", HTTP_GET, umleiten);        // Firefox

  server.onNotFound([]() {
    // Am Zugangspunkt fuehrt JEDER Name hierher (Catch-all-DNS). Ein 404 auf
    // eine unbekannte Adresse waere dort das Falsche - der Benutzer hat
    // vermutlich star.watch getippt und will die Oberflaeche sehen.
    if (aktiv != nullptr && aktiv->eigener_ap()) {
      server.sendHeader("Location", portal_ziel(), true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain",
                "Nicht da. Diese Oberflaeche kennt nur / und /test/...\n"
                "Sie ist ein Messgeraet - die API der Uhr entsteht in AP 3.4.");
  });
}

int Testweb::netze_suchen() {
  // Ein Scan im AP-Modus unterbricht die eigenen Beacons kurz - die Seite
  // haengt dabei zwei Sekunden. Das ist hinnehmbar, weil man danach etwas
  // auswaehlen will; wichtig ist nur, dass der AP hinterher weiterlaeuft.
  const int n = WiFi.scanNetworks();
  konsole.printf("\n%d Netze gefunden:\n", n);
  for (int i = 0; i < n; i++)
    konsole.printf("  %-32s %4d dBm  %s\n", WiFi.SSID(i).c_str(), static_cast<int>(WiFi.RSSI(i)),
                   WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "offen" : "verschluesselt");
  if (n == 0)
    konsole.println("  (keins - im AP-Modus kann der Scan leer bleiben, nochmal versuchen)");
  return n;
}

String Testweb::gespeicherte_ssid() const {
  nvs_oeffnen();
  return nvs.getString(NVS_SSID, "");
}

String Testweb::gespeichertes_passwort() const {
  nvs_oeffnen();
  return nvs.getString(NVS_PASS, "");
}

Testweb::Adresswahl Testweb::gespeicherte_adresse() const {
  nvs_oeffnen();
  Adresswahl a;
  a.fest = nvs.getBool(NVS_FEST, false);
  a.ip = nvs.getString(NVS_IP, "");
  a.maske = nvs.getString(NVS_MASKE, "");
  a.gw = nvs.getString(NVS_GW, "");
  a.dns = nvs.getString(NVS_DNS, "");
  return a;
}

bool Testweb::adresse_pruefen(const Adresswahl &a, String *fehler) {
  if (!a.fest)
    return true;
  IPAddress ip, maske, gw, dns;
  if (!ip.fromString(a.ip)) { *fehler = "IP-Adresse ungueltig"; return false; }
  if (!maske.fromString(a.maske) || !maske_gueltig(maske)) {
    *fehler = "Netzmaske ungueltig (z. B. 255.255.255.0)";
    return false;
  }
  if (!gw.fromString(a.gw)) { *fehler = "Gateway ungueltig"; return false; }
  if (a.dns.length() > 0 && !dns.fromString(a.dns)) { *fehler = "DNS ungueltig"; return false; }
  const uint32_t m = als_zahl(maske), i = als_zahl(ip), g = als_zahl(gw);
  if ((i & m) != (g & m)) {
    *fehler = "IP und Gateway liegen nicht im selben Netz - die Uhr waere unerreichbar";
    return false;
  }
  if (i == g) { *fehler = "IP und Gateway sind gleich"; return false; }
  if ((i & ~m) == 0 || (i & ~m) == ~m) {
    *fehler = "Das ist die Netz- oder Rundrufadresse, keine Geraeteadresse";
    return false;
  }
  return true;
}

bool Testweb::wlan_setzen(const String &ssid, const String &passwort,
                          const Adresswahl *adresse) {
  nvs_oeffnen();  // auch vor beginn() - siehe nvs_oeffnen()
  if (ssid.length() == 0) {
    nvs.remove(NVS_SSID);
    nvs.remove(NVS_PASS);
    // Die feste Adresse gehoert zum Netz - sie geht mit. Sonst kaeme die Uhr
    // im naechsten WLAN mit einer Adresse hoch, die dort nicht passt.
    nvs.remove(NVS_FEST);
    nvs.remove(NVS_IP);
    nvs.remove(NVS_MASKE);
    nvs.remove(NVS_GW);
    nvs.remove(NVS_DNS);
    konsole.println("Zugangsdaten und Adresseinstellung geloescht. Nach dem naechsten\n"
                    "Start laeuft wieder der eigene Zugangspunkt.");
    return true;
  }

  // DEN ALTEN STAND MERKEN. Scheitert die Verbindung, kommt er zurueck - bis
  // 26.09.2026 stand das nur im Kommentar weiter unten, der Code hatte die
  // neuen Daten da schon gespeichert. Ein Tippfehler beim UMSTELLEN
  // ueberschrieb so ein funktionierendes WLAN.
  const String alt_ssid = nvs.getString(NVS_SSID, "");
  const String alt_pass = nvs.getString(NVS_PASS, "");
  const bool alt_fest = nvs.getBool(NVS_FEST, false);
  const String alt_ip = nvs.getString(NVS_IP, ""), alt_maske = nvs.getString(NVS_MASKE, ""),
               alt_gw = nvs.getString(NVS_GW, ""), alt_dns = nvs.getString(NVS_DNS, "");

  nvs.putString(NVS_SSID, ssid);
  nvs.putString(NVS_PASS, passwort);
  if (adresse != nullptr) {
    nvs.putBool(NVS_FEST, adresse->fest);
    if (adresse->fest) {
      nvs.putString(NVS_IP, adresse->ip);
      nvs.putString(NVS_MASKE, adresse->maske);
      nvs.putString(NVS_GW, adresse->gw);
      nvs.putString(NVS_DNS, adresse->dns);
    }
  }
  // NUR die SSID ins Log. Der Ringpuffer ist ueber die Weboberflaeche
  // abrufbar, und ein WLAN-Passwort hat dort nichts zu suchen.
  konsole.printf("\nGespeichert: '%s' (Passwort %d Zeichen, nicht im Log), Adresse %s.\n",
                 ssid.c_str(), passwort.length(),
                 nvs.getBool(NVS_FEST, false) ? "fest" : "per DHCP");
  konsole.print("Verbinde ");

  // ===================================================================
  // DER ZUGANGSPUNKT BLEIBT AN. WIFI_AP_STA, nicht WIFI_STA.
  // ===================================================================
  // Sonst gibt es ein Henne-Ei-Problem: schaltet das Geraet beim Verbinden
  // seinen eigenen Zugangspunkt ab, bricht die Seite weg - und die neue
  // Adresse im Heimnetz steht dann nur in der seriellen Ausgabe. Genau das
  // Kabel, das man mit der Einrichtung loswerden wollte.
  //
  // Mit AP+STA laeuft beides gleichzeitig: man bleibt am Zugangspunkt und
  // SIEHT dort die neue Adresse in der Zustandsanzeige. Der Zugangspunkt
  // faellt beim naechsten Neustart weg (dann verbindet sich das Geraet nur
  // noch als Station) oder sofort auf Knopfdruck.
  //
  // ABER NUR, WENN DER ZUGANGSPUNKT SCHON LAEUFT (26.09.2026). Wird aus dem
  // Heimnetz heraus umgestellt, war nie einer eingerichtet - WIFI_AP_STA
  // spannte dann den Vorgabe-Zugangspunkt des ESP auf: OFFEN, Name "ESP_...".
  // Aus dem Heimnetz heraus bleibt es deshalb bei der Station; wer die
  // Adresse wechselt, bekommt die neue in der Antwort genannt.
  WiFi.mode(eigener_ap_ ? WIFI_AP_STA : WIFI_STA);
  funk_einstellen();
  adresse_anwenden();
  WiFi.begin(ssid.c_str(), passwort.c_str());
  const uint32_t ab = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - ab < WLAN_TIMEOUT_MS) {
    delay(250);
    konsole.print(".");
  }
  konsole.println();

  if (WiFi.status() == WL_CONNECTED) {
    verbunden_ = true;
    eigener_ap_ = false;  // Heimnetz ist ab jetzt die Hauptadresse
    sta_aktiv_ = true;
    // Die Ersteinrichtung ist damit vorbei. Ohne diese Zeile bliebe die
    // Oberflaeche im Einrichtungsmodus haengen, bis jemand neu startet - und
    // zeigte weiter nur die Netz-Karte, obwohl die Uhr laengst fertig ist.
    offener_ap_ = false;
    mdns_dienste();  // die Adresse im TXT ist jetzt die aus dem Heimnetz
    konsole.printf("Verbunden, %d dBm.\n", WiFi.RSSI());
    konsole.printf("NEUE ADRESSE: http://%s/   oder   http://starwatch.local/\n",
                   WiFi.localIP().toString().c_str());
    konsole.println("Der eigene Zugangspunkt laeuft noch weiter, damit die neue\n"
                    "Adresse dort ablesbar ist. Er faellt beim naechsten Neustart\n"
                    "weg - oder sofort mit 'na'.");
    return true;
  }

  // Nicht erreichbar: der Zugangspunkt lief ohnehin weiter, es ist also
  // nichts verloren. Nach einem Tippfehler im Passwort bleibt das Geraet
  // erreichbar, statt nur noch am Kabel bedienbar zu sein.
  konsole.println("NICHT verbunden - falsches Passwort, oder das Netz ist zu weit weg.");
  konsole.println("Der eigene Zugangspunkt laeuft weiter, nochmal versuchen.");

  // WAR ES DIE ERSTEINRICHTUNG, kommen die Zugangsdaten wieder heraus.
  //
  // Sonst haette ein Tippfehler im WLAN-Passwort eine dauerhafte Folge: beim
  // naechsten Start gaelten die Daten als "bekannt", der Zugangspunkt kaeme
  // gesichert hoch - und der Besitzer stuende vor genau dem Schluessel, den
  // er nicht hat. Ein Tippfehler darf keine Uhr aussperren.
  //
  // Bei einer spaeteren AENDERUNG kommt die alte Angabe zurueck: dort hat der
  // Besitzer das Geraeteblatt laengst, und die vorherige, gute Einstellung
  // wegzuwerfen waere schlimmer.
  if (offener_ap_) {
    nvs.remove(NVS_SSID);
    nvs.remove(NVS_PASS);
    nvs.remove(NVS_FEST);
    konsole.println("Ersteinrichtung: Zugangsdaten wieder verworfen - der\n"
                    "Zugangspunkt bleibt offen, damit du es erneut versuchen kannst.");
  } else {
    nvs.putString(NVS_SSID, alt_ssid);
    nvs.putString(NVS_PASS, alt_pass);
    nvs.putBool(NVS_FEST, alt_fest);
    nvs.putString(NVS_IP, alt_ip);
    nvs.putString(NVS_MASKE, alt_maske);
    nvs.putString(NVS_GW, alt_gw);
    nvs.putString(NVS_DNS, alt_dns);
    konsole.println("Alte Einstellung wiederhergestellt.");
  }
  WiFi.disconnect();

  // AUS DEM HEIMNETZ GEKOMMEN: gleich zurueck in das alte Netz. Sonst waere
  // die Uhr bis zum naechsten Neustart gar nicht mehr erreichbar - es laeuft
  // ja kein Zugangspunkt.
  if (!eigener_ap_ && alt_ssid.length() > 0) {
    konsole.printf("Zurueck zu '%s' ", alt_ssid.c_str());
    adresse_anwenden();
    funk_einstellen();
    WiFi.begin(alt_ssid.c_str(), alt_pass.c_str());
    const uint32_t ab2 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - ab2 < WLAN_TIMEOUT_MS) {
      delay(250);
      konsole.print(".");
    }
    konsole.println();
    if (WiFi.status() == WL_CONNECTED) {
      sta_aktiv_ = true;
      verbunden_ = true;
      konsole.printf("Wieder verbunden: http://%s/\n", WiFi.localIP().toString().c_str());
      return false;
    }
    konsole.println("Auch das alte Netz antwortet nicht - Neustart oeffnet den\n"
                    "Zugangspunkt (Schluessel im Geraeteblatt).");
  }
  sta_aktiv_ = false;
  // Nur behaupten, was stimmt: ein Zugangspunkt laeuft nur, wenn er vorher
  // schon lief (siehe WiFi.mode oben).
  eigener_ap_ = (WiFi.getMode() & WIFI_AP) != 0;
  verbunden_ = eigener_ap_;
  return false;
}

String Testweb::heimnetz_adresse() const {
  if (!sta_aktiv_ || WiFi.status() != WL_CONNECTED)
    return String();
  return WiFi.localIP().toString();
}

void Testweb::ap_abschalten() {
  if (!sta_aktiv_) {
    konsole.println("Kein Heimnetz verbunden - der Zugangspunkt bleibt, sonst\n"
                    "waere das Geraet nur noch am Kabel erreichbar.");
    return;
  }
  dns_beenden();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  eigener_ap_ = false;
  konsole.printf("Zugangspunkt abgeschaltet. Nur noch: http://%s/  oder  http://%s.local/\n",
                 WiFi.localIP().toString().c_str(), MDNS_NAME);
}

void Testweb::ausfuehren(char *zeile) {
  if (kommando_ != nullptr)
    kommando_(zeile);
}

// ===========================================================================
// Einstellungen als Datei (AP 3.7.2)
// ===========================================================================

/// Eine Zahl aus flachem JSON holen: "schluessel": <zahl>.
///
/// WARUM KEIN JSON-PARSER: dieses Schema ist flach und hat sechs bekannte
/// Schluessel. Eine Bibliothek dafuer aufzunehmen waere eine
/// Abhaengigkeitsentscheidung, und die gehoert zu AP 3.4, wo die echte API
/// entsteht - dort wird ohnehin geparst werden muessen. Bis dahin genuegt
/// Suchen und Lesen.
///
/// @return false, wenn der Schluessel fehlt oder keine Zahl folgt.
/// Wie json_zahl, aber fuer einen TEXT in Anfuehrungszeichen. Ohne
/// Escapes: der einzige Text, der hier gelesen wird, ist der Uhrname, und der
/// darf weder " noch \ enthalten (alias_pruefen). Steht doch ein Escape
/// darin, weist die Pruefung ihn danach ab.
static bool json_wort(const String &text, const char *schluessel, String *wert) {
  const int k = text.indexOf(String("\"") + schluessel + "\"");
  if (k < 0)
    return false;
  const int doppelpunkt = text.indexOf(':', k);
  if (doppelpunkt < 0)
    return false;
  const int a = text.indexOf('"', doppelpunkt);
  if (a < 0)
    return false;
  const int e = text.indexOf('"', a + 1);
  if (e < 0)
    return false;
  *wert = text.substring(a + 1, e);
  return true;
}

static bool json_zahl(const String &text, const char *schluessel, double *wert) {
  const int k = text.indexOf(String("\"") + schluessel + "\"");
  if (k < 0)
    return false;
  int i = text.indexOf(':', k);
  if (i < 0)
    return false;
  i++;
  while (i < (int)text.length() && (text[i] == ' ' || text[i] == '\t'))
    i++;
  const int anfang = i;
  if (i < (int)text.length() && (text[i] == '-' || text[i] == '+'))
    i++;
  bool ziffer = false;
  while (i < (int)text.length() &&
         ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) {
    if (text[i] != '.')
      ziffer = true;
    i++;
  }
  if (!ziffer)
    return false;
  *wert = text.substring(anfang, i).toDouble();
  return true;
}

namespace {
/// Laufzeit als "3 d 04:17:22" - ein Sekundenwert ist beim Lesen wertlos.
String dauer_text(uint32_t ms) {
  const uint32_t s = ms / 1000u;
  char t[40];
  snprintf(t, sizeof(t), "%lu d %02lu:%02lu:%02lu", (unsigned long)(s / 86400u),
           (unsigned long)((s / 3600u) % 24u), (unsigned long)((s / 60u) % 60u),
           (unsigned long)(s % 60u));
  return t;
}

const char *neustartgrund() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "Einschalten";
    case ESP_RST_SW:       return "Software (Neustart oder nach Brownout)";
    case ESP_RST_PANIC:    return "ABSTURZ (Panic)";
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "WATCHDOG - etwas hat die Schleife blockiert";
    case ESP_RST_BROWNOUT: return "BROWNOUT - Versorgung eingebrochen";
    case ESP_RST_DEEPSLEEP: return "Tiefschlaf";
    default:               return "unbekannt";
  }
}
}  // namespace

String Testweb::diagnose_text() {
  String t;
  t.reserve(1800);
  t += "STAR WATCH - DIAGNOSE\n";
  t += "=====================\n\n";

  t += "GERAET\n";
  t += "  Name           " + String(betrieb_->geraetename().c_str()) + "\n";
  t += "  Alias          " +
       String(betrieb_->alias().empty() ? "(keiner)" : betrieb_->alias().c_str()) + "\n";
  t += "  Firmware       " + String(g::FIRMWARE_VERSION) +
       (g::ENTWICKLUNGSGERAET ? "  (ENTWICKLUNGSFASSUNG - Geheimnisse stehen im "
                                "Netz-Log, nicht ausliefern)"
                              : "") + "\n";
  t += "  Partition      " + String(aktiv_update_ != nullptr
                                        ? aktiv_update_->partition_text().c_str()
                                        : "-") + "\n";
  t += "  Laufzeit       " + dauer_text(millis()) + "\n";
  t += "  Letzter Start  " + String(neustartgrund()) + "\n";
  // Der KLEINSTE je freie Heap ist die aussagekraeftige Zahl: ein Leck faellt
  // hier auf, lange bevor das Geraet abstuerzt.
  t += "  Speicher frei  " + String(ESP.getFreeHeap() / 1024) + " KB (kleinster je " +
       String(ESP.getMinFreeHeap() / 1024) + " KB)\n\n";

  t += "NETZ\n";
  t += "  Verbindung     " + String(WiFi.status() == WL_CONNECTED
                                        ? (String("Heimnetz ") + WiFi.localIP().toString())
                                        : String("KEIN HEIMNETZ")) + "\n";
  if (WiFi.status() == WL_CONNECTED)
    t += "  Pegel          " + String(WiFi.RSSI()) + " dBm\n";
  if (WiFi.status() == WL_CONNECTED)
    t += "  Zugangspunkt   " + WiFi.BSSIDstr() + ", Kanal " + String(WiFi.channel()) + "\n";
  t += "  Sendeleistung  " +
       String(funk_gedrosselt() ? "11 dBm - GEDROSSELT, letzter Start war ein Brownout"
                                : "19,5 dBm") + "\n";
  {
    const uint64_t summe = funk_schlaf_summe_ms +
                           (funk_schlaeft ? millis() - funk_schlaf_ab_ms : 0u);
    t += "  Funksparen     " +
         String(funk_schlaeft ? "schlaeft gerade" : "wach") + ", zuletzt " +
         String(funk_letzter_schlaf_ms / 60000u) + " min geschlafen, seit Start " +
         String(millis() > 0 ? static_cast<int>(100.0 * summe / millis()) : 0) +
         " % der Zeit\n";
  }
  t += "  Ausfaelle      " + String(wlan_ausfaelle) +
       (wlan_weg_seit != 0
            ? String(" (JETZT WEG seit ") + String((millis() - wlan_weg_seit) / 1000u) + " s)"
            : String()) + "\n\n";

  t += "ZEIT\n";
  t += "  Uhrzeit        " + String(betrieb_->uhrzeit_text().c_str()) + "\n";
  t += "  Quelle         " + String(betrieb_->zeit_aus_netz() ? "Netz (NTP)"
                                                             : "von Hand / unbekannt") + "\n";
  t += "  Zeitzone       " + String(betrieb_->zeitzone().c_str()) + "\n";
  t += "  Modus          " + String(zeitmodus_text(betrieb_->zeitmodus())) + "\n";
  t += "  Timer-Lauf     " + String(timerlauf_text(betrieb_->timerlauf())) + "\n";
  t += "  Abweichung     " + String(drift(antrieb_->position(), betrieb_->soll_jetzt(),
                                          g::REV)) + " Schritte\n\n";

  t += "ANTRIEB\n";
  const uint8_t ic = antrieb_->ic_version();
  t += "  Treiber        0x" + String(ic, HEX) +
       (ic == 0x21 ? " (TMC2209, antwortet)" : " KEINE GUELTIGE ANTWORT - VM pruefen!") + "\n";
  if (ic == 0x21) {
    t += "  Motorstrom     " + String(antrieb_->strom_ma()) + " mA RMS\n";
    t += "  DRV_STATUS     0x" + String(antrieb_->drv_status(), HEX) + "\n";
  }
  t += "  Fahrtempo      " + String(betrieb_->fahrtempo()) + " Schritte/s\n";
  t += "  Rampe          " + String(antrieb_->beschleunigung()) + " Schritte/s^2\n";
  t += "  Position       " + String(antrieb_->position()) + " (Blatt " +
       String(normiere(antrieb_->position(), g::REV)) + ")\n\n";

  t += "HOMING UND HALL\n";
  t += "  Sensor         " +
       String(hall_->bereit() ? ("TMAG5273 auf 0x" + String(hall_->sensoradresse(), HEX))
                              : String("FEHLT - ") + tmaggrund_text(hall_->sensorgrund())) +
       "\n";
  t += "  Letzte Fahrt   " + String(hominggrund_text(betrieb_->letztes_homing().grund)) + "\n";
  if (betrieb_->letztes_homing().ok()) {
    t += "  Schwerpunkt    " + String(betrieb_->letztes_homing().schwerpunkt, 2) + "\n";
    t += "  Nachlauf       " + String(betrieb_->letztes_homing().nachlauf) + "\n";
    t += "  Aussteuerung   " + String(betrieb_->letztes_homing().spanne_mt, 2) +
         " mT (Soll " + String(g::HALL_SPANNE_MIN_MT, 1) + " bis " +
         String(g::HALL_SPANNE_MAX_MT, 1) + ", beide noch GESETZT statt gemessen)\n";
  }
  t += "  Feldbreite     " + String(hall_->fenster_schritte()) + " Schritte\n";

  // Homing-Verlauf (05.10.2026): gespeichert, ueberlebt den Neustart. Ganze
  // Perioden Abweichung heissen Schrittverlust - siehe schrittverlust().
  {
    const Homingverlauf &v = betrieb_->homingverlauf();
    t += "\nHOMING-VERLAUF (neueste zuerst, gespeichert; Abw = Abweichung zur\n"
         "vorigen Referenz, ganze Perioden zu " + String(g::SCHRITTE_JE_PERIODE) +
         " Schritten = Schrittverlust)\n";
    if (v.anzahl() == 0)
      t += "  (noch keine Eintraege)\n";
    for (size_t i = 0; i < v.anzahl(); i++)
      t += "  " + String(betrieb_->homingeintrag_text(v.neuer(i)).c_str()) + "\n";
  }

  t += "\nUMGEBUNGSLICHT (J8)\n";
  if (lux_ == nullptr || !lux_->bereit()) {
    t += "  Sensor         " +
         String(lux_ == nullptr ? "nicht angemeldet" : luxgrund_text(lux_->grund())) +
         "\n  Folge          Klinge folgt dem Regler, keine Nachtabschaltung\n";
  } else {
    t += "  Beleuchtung    " + String(lux_->lux(), 1) + " lx\n";
    t += "  Einstellung    Stufe " + String(lux_->stufe()) + " von 8, x" +
         String(lux_->verstaerkung(), 3) + ", " + String(lux_->integrationszeit_ms()) +
         " ms, " + String(lux_->aufloesung(), 4) + " lx je Schritt\n";
    t += "  Rohwert        " + String(lux_->rohwert()) + " von 65535\n";
    t += "  Dimmfaktor     " + String(licht_->dimmfaktor() * 100.0f, 0) + " %\n";
    t += "  Schwellen      dimmen " + String(g::LUX_DUNKEL, 1) + " bis " +
         String(g::LUX_HELL, 0) + " lx, Nacht aus unter " + String(g::LUX_NACHT_AUS, 1) +
         " lx / an ueber " + String(g::LUX_NACHT_AN, 1) + " lx\n";
    t += "                 (gesetzt, nicht im Zimmer gemessen)\n";
  }
  t += "  Hall-Offset    " + String(betrieb_->hall_offset()) + "\n\n";

  t += "UPDATES\n";
  if (aktiv_update_ != nullptr) {
    t += "  Modus          " + String(updatemodus_text(aktiv_update_->modus())) + "\n";
    t += "  Kanal          " + String(updatekanal_text(aktiv_update_->kanal())) + "\n";
    t += "  Manifest       " + String(aktiv_update_->url().c_str()) + "\n";
    t += "  Letzte Meldung " + String(aktiv_update_->meldung().empty()
                                          ? "noch nicht geprueft"
                                          : aktiv_update_->meldung().c_str()) + "\n";
  } else {
    t += "  (nicht angebunden)\n";
  }
  return t;
}

String Testweb::einstellungen_json() const {
  String j = "{\n";
  j += "  \"schema\": " + String(g::KONFIG_SCHEMA) + ",\n";
  j += "  \"geraet\": \"" + String(g::GERAETETYP) + "\",\n";
  j += "  \"firmware\": \"" + String(g::FIRMWARE_VERSION) + "\",\n";
  j += "  \"hinweis\": \"Ohne Hall-Offset (Fertigungswert), ohne WLAN-Passwort "
       "und ohne Broker-Passwort.\",\n";
  j += "  \"einstellungen\": {\n";
  j += "    \"tempo\": " + String(betrieb_->fahrtempo()) + ",\n";
  j += "    \"rampe\": " + String(antrieb_->beschleunigung()) + ",\n";
  j += "    \"kreise\": " + String(betrieb_->effekt_kreise()) + ",\n";
  j += "    \"modus\": " + String((int)betrieb_->zeitmodus()) + ",\n";
  j += "    \"timer_lauf\": " + String((int)betrieb_->timerlauf()) + ",\n";
  j += "    \"alias\": \"" + json_text(String(betrieb_->alias().c_str())) + "\",\n";
  j += "    \"licht\": " +
       String(licht_ != nullptr ? licht_->soll_prozent() : 0.0f, 1) + ",\n";
  // Als ZAHL (0xRRGGBB dezimal), weil der Import nur Zahlen liest.
  j += "    \"farbe\": " +
       String((unsigned long)(licht_ != nullptr ? licht_->farbe() : g::LED_FARBE_VORGABE)) +
       ",\n";
  j += "    \"wlan_ssid\": \"" + json_text(gespeicherte_ssid()) + "\",\n";
  // Broker-ADRESSE ja, Broker-PASSWORT nein - dieselbe Linie wie beim WLAN.
  // Zusaetzliche Felder brauchen KEINE neue Schemaversion: eine aeltere
  // Firmware ueberliest sie, eine neuere vermisst sie nicht.
  j += "    \"mqtt_host\": \"" + json_text(String(betrieb_->mqtt_host().c_str())) + "\",\n";
  j += "    \"mqtt_port\": " + String(betrieb_->mqtt_port()) + ",\n";
  j += "    \"mqtt_user\": \"" + json_text(String(betrieb_->mqtt_benutzer().c_str())) + "\"\n";
  j += "  }\n}\n";
  return j;
}

bool Testweb::einstellungen_uebernehmen(const String &datei, String *meldung) {
  double v = 0.0;

  // ZUERST das Schema, dann der Geraetetyp. Eine Datei, die nicht hierher
  // gehoert, wird abgelehnt statt halb verstanden - genau dafuer gibt es die
  // Schemaversion.
  if (!json_zahl(datei, "schema", &v)) {
    *meldung = "Kein Feld \"schema\" - das ist keine Einstellungsdatei.";
    return false;
  }
  if ((int)v != g::KONFIG_SCHEMA) {
    *meldung = "Schema " + String((int)v) + ", diese Firmware kennt " +
               String(g::KONFIG_SCHEMA) + ". Abgelehnt.";
    return false;
  }
  if (datei.indexOf(String("\"") + g::GERAETETYP + "\"") < 0) {
    *meldung = "Geraetetyp passt nicht (erwartet \"" + String(g::GERAETETYP) +
               "\"). Abgelehnt.";
    return false;
  }

  int uebernommen = 0;
  String bericht;

  if (json_zahl(datei, "tempo", &v) && v >= 1 && v <= g::TEMPO_MAX) {
    betrieb_->fahrtempo((uint32_t)v);
    antrieb_->tempo((uint32_t)v);
    bericht += "Tempo " + String((uint32_t)v) + ", ";
    uebernommen++;
  }
  if (json_zahl(datei, "rampe", &v) && v >= 1) {
    antrieb_->rampe((uint32_t)v, (uint32_t)v);
    bericht += "Rampe " + String((uint32_t)v) + ", ";
    uebernommen++;
  }
  if (json_zahl(datei, "kreise", &v) && v >= 0) {
    betrieb_->effekt_kreise((int32_t)v);
    bericht += "Kreise " + String(betrieb_->effekt_kreise()) + ", ";
    uebernommen++;
  }
  if (json_zahl(datei, "modus", &v) && v >= 0 && v <= 2) {
    betrieb_->zeitmodus((Zeitmodus)(int)v);
    bericht += "Modus " + String((int)v) + ", ";
    uebernommen++;
  }
  {
    String name;
    if (json_wort(datei, "alias", &name)) {
      if (betrieb_->alias(std::string(name.c_str()))) {
        bericht += "Name " + String(betrieb_->anzeigename().c_str()) + ", ";
        uebernommen++;
      } else {
        bericht += "Name abgewiesen, ";
      }
    }
  }
  if (json_zahl(datei, "timer_lauf", &v) && (v == 0 || v == 1)) {
    betrieb_->timerlauf(v == 1 ? Timerlauf::sekundentakt : Timerlauf::gleitend);
    bericht += "Timer " + String(timerlauf_text(betrieb_->timerlauf())) + ", ";
    uebernommen++;
  }
  if (json_zahl(datei, "licht", &v) && v >= 0 && v <= 100 && licht_ != nullptr) {
    licht_->soll_prozent((float)v);
    bericht += "Licht " + String((float)v, 1) + " %, ";
    uebernommen++;
  }

  if (json_zahl(datei, "farbe", &v) && v >= 0 && v <= 0xFFFFFF && licht_ != nullptr) {
    licht_->farbe((uint32_t)v);
    char f[8];
    snprintf(f, sizeof(f), "#%06lx", (unsigned long)(uint32_t)v);
    bericht += "Farbe " + String(f) + ", ";
    uebernommen++;
  }

  if (uebernommen == 0) {
    *meldung = "Schema passt, aber kein brauchbarer Wert darin.";
    return false;
  }

  betrieb_->werte_sichern();
  *meldung = String(uebernommen) + " Werte uebernommen und gespeichert: " +
             bericht + "\nDer Hall-Offset wurde NICHT angetastet.";
  konsole.printf("\n[web] Import: %s\n", meldung->c_str());
  return true;
}

void Testweb::wlan_aufsicht() {
  if (!sta_aktiv_)
    return;
  const bool steht = (WiFi.status() == WL_CONNECTED);

  if (steht && wlan_weg_seit != 0) {
    const uint32_t weg_s = (millis() - wlan_weg_seit) / 1000u;
    wlan_weg_seit = 0;
    wlan_wieder_da = true;
    // mDNS neu anmelden: der Name haengt an der Adresse, und die kann sich
    // beim Wiederverbinden geaendert haben.
    MDNS.end();
    if (MDNS.begin(MDNS_NAME))
      mdns_dienste();
    konsole.printf("\n[netz] WLAN wieder da nach %lu s. Adresse %s.\n",
                   (unsigned long)weg_s, WiFi.localIP().toString().c_str());
    return;
  }

  if (!steht) {
    if (wlan_weg_seit == 0) {
      wlan_weg_seit = millis();
      wlan_ausfaelle++;
      wlan_letzter_nachstoss = millis();
      konsole.println("\n[netz] WLAN weg. Der Kern verbindet selbst wieder; die UHR LAEUFT\n"
                      "       weiter - nur die Zeitkorrektur pausiert.");
      return;
    }
    if (millis() - wlan_letzter_nachstoss > WLAN_NACHSTOSS_MS) {
      wlan_letzter_nachstoss = millis();
      WiFi.reconnect();
    }
  }
}

void Testweb::neu_verbinden() {
  if (!sta_aktiv_)
    return;
  WiFi.reconnect();
}

bool Testweb::wlan_rueckkehr_abholen() {
  const bool r = wlan_wieder_da;
  wlan_wieder_da = false;
  return r;
}

uint32_t Testweb::wlan_weg_ms() const {
  return wlan_weg_seit == 0 ? 0 : (millis() - wlan_weg_seit);
}

void Testweb::stumm(bool an) {
  update_laeuft = an;
  // Das Update laedt AUSSERHALB von tick() - also hier wecken, nicht dort.
  if (an)
    funk_wach(true);
}

void Testweb::tick() {
  // Vor allem anderen: steht ein Neustart an? Erst hier, damit die HTTP-
  // Antwort den Browser noch erreicht hat.
  if (neustart_ab_ms != 0 && millis() - neustart_ab_ms > NEUSTART_FRIST_MS) {
    if (neustart_mit_reset) {
      betrieb_->werksreset();
      wlan_setzen("", "");
    }
    delay(50);
    ESP.restart();
  }

  wlan_aufsicht();

  if (!verbunden_)
    return;

  // Waehrend eines Updates nur der Update-Kanal. Die Oberflaeche wartet
  // solange - eine unbeantwortete Anfrage ist besser als ein abgebrochenes
  // Update, und die Seite meldet sich von selbst zurueck (Wartekasten).
  if (update_laeuft) {
    ArduinoOTA.handle();
    return;
  }

  // Wach bleiben, solange jemand da ist; sonst nach FUNK_WACH_MS schlafen.
  // Nur im Heimnetz: als Zugangspunkt muss der Funk ohnehin wach sein.
  {
    const bool jemand_da = update_laeuft || (log_client && log_client.connected()) ||
                           millis() - letzte_bedienung_ms < FUNK_WACH_MS ||
                           WiFi.getMode() != WIFI_STA;
    funk_wach(jemand_da);
  }
  server.handleClient();
  if (dns_laeuft)
    dns.processNextRequest();
  ArduinoOTA.handle();
  log_tick();
}

}  // namespace starwatch
