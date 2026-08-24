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
Preferences nvs;

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

/// Sendeleistung. AM GERAET GEMESSEN (22.08.2026): mit der Vorgabe (19,5 dBm)
/// loeste der Brownout-Detektor genau beim Einschalten des Funkmoduls aus -
/// acht Neustarts in acht Sekunden, `E BOD: Brownout detector was triggered`,
/// und der Chip liess sich in dieser Schleife nicht mehr flashen.
///
/// Der Umsetzungsplan sagt es in AP 3.2 voraus: "output_power niedriger
/// setzen, falls die Brownout-Serie zurueckkommt". Sie ist zurueckgekommen,
/// sobald diese Firmware WLAN benutzt.
///
/// 11 dBm senkt die Stromspitze des Sendeverstaerkers deutlich und reicht
/// fuer einen Zugangspunkt auf dem Schreibtisch weit. DAS IST EINE
/// LINDERUNG, KEINE HEILUNG: die Ursache ist die Versorgung - siehe den
/// Hinweis, den setup() beim Brownout ausgibt.
constexpr wifi_power_t SENDELEISTUNG = WIFI_POWER_11dBm;

/// Die Oberflaeche. Eine Seite, aus dem Flash.
///
/// Die Gruppierung ist die des Pruefstands, weil sie sich dort bewaehrt hat:
/// Notbremsen oben, dann Licht, Uhr, Timer, Referenzfahrt, Hall, Motor,
/// Diagnose. Der Umsetzungsplan nennt sie ausdruecklich als brauchbare
/// Vorlage fuer AP 3.5.
const char SEITE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Star Watch - Pruefstand</title><style>
:root{--bg:#14161a;--k:#1c1f26;--r:#2a2f3a;--t:#e6e8ec;--d:#9aa1ad;--a:#7fb3ff;--w:#ffb347;--f:#ff6b6b;--o:#5fd18c}
*{box-sizing:border-box}body{margin:0;padding:12px;background:var(--bg);color:var(--t);
font:14px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
h1{font-size:17px;margin:0 0 4px}h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;
color:var(--d);margin:0 0 8px;font-weight:600}
.top{display:flex;justify-content:space-between;align-items:baseline;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.net{color:var(--d);font-size:12px}
.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(290px,1fr));gap:10px}
.k{background:var(--k);border:1px solid var(--r);border-radius:9px;padding:11px 12px}
.k.sos{border-color:#5a2a2a}
button{background:var(--r);color:var(--t);border:1px solid #3a4150;border-radius:6px;
padding:6px 10px;font-size:13px;cursor:pointer;margin:2px 2px 2px 0}
button:hover{background:#333a47}button:active{transform:translateY(1px)}
button.p{background:#2d4a7a;border-color:#3b5c93}button.s{background:#6a2626;border-color:#8a3333}
input{background:#101317;color:var(--t);border:1px solid #3a4150;border-radius:6px;
padding:5px 7px;font-size:13px;width:80px;margin:2px 2px 2px 0}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:2px 0;vertical-align:top}td:first-child{color:var(--d);white-space:nowrap;padding-right:10px}
.v{font-variant-numeric:tabular-nums}.ok{color:var(--o)}.warn{color:var(--w)}.err{color:var(--f)}
pre{background:#0d1014;border:1px solid var(--r);border-radius:7px;padding:9px;
font-size:12px;line-height:1.4;max-height:260px;overflow:auto;white-space:pre-wrap;margin:0}
.hint{color:var(--d);font-size:12px;margin:6px 0 0}
.row{display:flex;align-items:center;gap:4px;flex-wrap:wrap;margin:3px 0}
.row label{color:var(--d);min-width:64px;font-size:12px}
details{margin-top:9px;border-top:1px solid var(--r);padding-top:7px}
summary{color:var(--d);font-size:12px;cursor:pointer}
</style></head><body>
<div class="top"><h1>Star Watch <span style="color:var(--d);font-weight:400">&mdash; Pr&uuml;fstand</span></h1>
<div class="net" id="net">&nbsp;</div></div>
<div id="neu" style="display:none;background:#16301f;border:1px solid #2f6b45;
 border-radius:9px;padding:10px 12px;margin-bottom:10px">
<b style="color:var(--o)">Im Heimnetz erreichbar</b><br>
<span style="font-size:15px">&rarr; <a id="neu_ip" href="#" style="color:var(--a)"></a>
&nbsp;oder&nbsp; <a id="neu_nm" href="#" style="color:var(--a)"></a></span>
<p class="hint" id="neu_txt">&nbsp;</p>
<button class="p" id="neu_go">jetzt wechseln</button>
<button id="neu_stop">hier bleiben</button>
<button onclick="c('na')">Zugangspunkt abschalten</button></div>

<div class="g">
<div class="k sos"><h2>Sofort</h2>
<button class="s" onclick="c('s')">STOPP</button>
<button class="s" onclick="c('l 0')">LICHT AUS</button>
<button onclick="c('e 1')">Endstufe ein</button>
<button onclick="c('e 0')">Endstufe aus</button>
<p class="hint">Endstufe aus hei&szlig;t Freilauf &mdash; die Klinge kippt aus ihrer Lage.</p></div>

<div class="k"><h2>Zifferblatt</h2>
<svg id="uhr" viewBox="0 0 200 200" style="width:100%;max-width:260px;display:block;margin:0 auto">
 <circle cx="100" cy="100" r="94" fill="none" stroke="var(--r)" stroke-width="1.5"/>
 <g id="u_striche"></g>
 <path id="u_fenster" fill="none" stroke="var(--w)" stroke-width="5" opacity="0.30"/>
 <line id="u_null" x1="100" y1="100" x2="100" y2="14" stroke="var(--w)"
       stroke-width="1.5" stroke-dasharray="3 3"/>
 <circle id="u_nullp" r="4" fill="var(--w)"/>
 <line id="u_ziel" x1="100" y1="100" x2="100" y2="20" stroke="var(--o)"
       stroke-width="1.5" stroke-dasharray="2 4" opacity="0"/>
 <line id="u_zeiger" x1="100" y1="100" x2="100" y2="22" stroke="var(--a)"
       stroke-width="6" stroke-linecap="round"/>
 <circle cx="100" cy="100" r="4.5" fill="var(--t)"/>
 <text id="u_text" x="100" y="128" text-anchor="middle" font-size="15"
       fill="var(--t)" font-family="monospace">--:--</text>
</svg>
<p class="hint"><span style="color:var(--a)">&#9647;</span> Zeiger &nbsp;
<span style="color:var(--w)">&#9647;</span> echter Nullpunkt (Magnet am Hall-Sensor) mit
Sensorfenster &nbsp; <span style="color:var(--o)">&#9647;</span> Ziel, solange gefahren wird.</p>
<p class="hint" id="u_hint">&mdash;</p></div>

<div class="k"><h2>Zustand</h2><table id="z"></table></div>

<div class="k"><h2>Licht</h2>
<div class="row"><label>Helligkeit</label><input id="lh" type="number" min="0" max="100" value="60">
<button class="p" onclick="c('l '+v('lh'))">setzen</button></div>
<button onclick="c('la')">an</button><button onclick="c('lo')">aus</button>
<button onclick="c('lz')">z&uuml;nden</button><button onclick="c('ll')">l&ouml;schen</button>
<button onclick="c('ls')">Stundeneffekt</button><br>
<div class="row"><label>PWM direkt</label><input id="lw" type="number" min="0" max="100" step="0.5" value="10">
<button onclick="c('w '+v('lw'))">setzen</button></div>
<button onclick="c('lr')">Rampe 0-20 % (28 s)</button><button onclick="c('lt')">Treppe 0-100 % (33 s)</button>
<p class="hint">Rampe und Treppe sind die Messhilfen f&uuml;r AP 0.5: ab welchem Prozentwert glimmt die LED, l&auml;uft sie gleichm&auml;&szlig;ig?</p></div>

<div class="k"><h2>Uhr</h2>
<div class="row"><label>Zeit</label><input id="zt" placeholder="14:35:00" style="width:104px">
<button class="p" onclick="c('z '+v('zt'))">stellen</button>
<button onclick="jetzt()">Browserzeit</button></div>
<button onclick="c('m 0')">Modus aus</button><button onclick="c('m 1')">flie&szlig;end</button>
<button onclick="c('m 2')">Minutensprung</button>
<div class="row"><label>Zeitzone</label><input id="tz" style="width:198px">
<button onclick="c('z '+v('tz'))">setzen</button></div>
<p class="hint"><b>Im Netz holt sich die Uhr die Zeit selbst</b> (NTP, Sommerzeit aus der
Zeitzone gerechnet). Von Hand stellen braucht man nur am eigenen Zugangspunkt oder in einem Netz
ohne Internet. Die Zeitzone ist eine <b>POSIX-Zeichenkette</b> &mdash; der ESP32 hat keine
Zeitzonendatenbank, die Umstellungsregel muss vollst&auml;ndig darin stehen.</p>
<p class="hint"><b>Die Zeit ist der Normalzustand.</b> 10 s nach der letzten Handlung nimmt die
Uhr den Zeiger von selbst zur&uuml;ck &mdash; nach einem Anfahren, nach einem Timer, nach dem
Alarm. Ziel ist der hier gew&auml;hlte Modus; steht er auf <i>aus</i>, gibt es keinen
R&uuml;ckfall.</p>
<p class="hint"><b>Beim Einmessen ist der R&uuml;ckfall gesperrt.</b> Verstellen und
&bdquo;Das ist zw&ouml;lf&ldquo; halten die Uhr fern, sonst w&auml;re der Offset falsch. Ein Klick
auf einen Modus hier gibt wieder frei.</p></div>

<div class="k"><h2>Zeiger anfahren</h2>
<div class="row"><label>Uhrzeit</label>
<button class="p" onclick="c('u 12:00')">12:00</button><button class="p" onclick="c('u 15:00')">15:00</button>
<button class="p" onclick="c('u 18:00')">18:00</button><button class="p" onclick="c('u 21:00')">21:00</button></div>
<div class="row"><label>eigene</label><input id="ph" placeholder="21:30" style="width:92px">
<button class="p" onclick="c('u '+v('ph'))">anfahren</button></div>
<div class="row"><label>Schritt</label><input id="pp" type="number" value="0">
<button onclick="c('p '+v('pp'))">anfahren</button></div>
<p class="hint">Die Uhrzeit darf 0:00 bis 23:59 sein &mdash; 24 Stunden landen auf dem
Zw&ouml;lferblatt, 15:00 zeigt also wie 3:00. Eine blanke Zahl gilt als Stunde
(<code>4.5</code> = halb f&uuml;nf). Anfahren geht immer vorw&auml;rts, damit das Riemenspiel
auf derselben Seite bleibt; zum Nachstellen in beide Richtungen ist die Karte
&bdquo;Zw&ouml;lf einmessen&ldquo; da.</p></div>

<div class="k"><h2>Tempo und Rampe</h2>
<div class="row"><label>Tempo</label>
<button onclick="c('f 3200')">1 U/s</button><button onclick="c('f 6400')">2 U/s</button>
<button onclick="c('f 9600')">3 U/s</button><button onclick="c('f 12800')">4 U/s</button>
<button onclick="c('f 19200')">6 U/s</button></div>
<div class="row"><label>eigenes</label><input id="pf" type="number" min="1" max="25600">
<button onclick="c('f '+v('pf'))">Schritte/s</button></div>
<div class="row"><label>Rampe</label><input id="pa" type="number" min="1">
<button onclick="c('a '+v('pa'))">Schritte/s&sup2;</button>
<button onclick="c('a 0')">anzeigen</button></div>
<p class="hint">3200 Schritte sind eine Umdrehung. Die Rampe l&auml;sst den Zeiger anfahren und
wieder abbremsen &mdash; ohne sie w&auml;re jeder Start ein Ruck, und ein Ruck kostet
Riemenz&auml;hne. Sie begrenzt auch, wie kurz ein Timer sein darf: <b id="tmin">&mdash;</b></p>

<details><summary>Was gilt wof&uuml;r</summary>
<p class="hint">Das Tempo hier gilt f&uuml;r <b>Anfahrten, Verstellen, Pendell&auml;ufe und die
Anfahrt des Timers</b>. Der <b>Uhrbetrieb</b> f&auml;hrt bewusst mit eigenem, langsamem Tempo
(0,125 U/s) &mdash; darunter bleibt der Treiber in stealthChop und ist still. Die
<b>Referenzfahrt</b> hat ebenfalls eigene Tempi, weil sie messen muss. Beides ist Absicht und
nicht hier einstellbar. Nach einem Neustart gilt wieder der Wert aus <code>geraet.h</code>.</p>
</details></div>

<div class="k"><h2>Schwung&shy;effekt</h2>
<div class="row"><label>Kreise</label>
<button onclick="c('k 0')">aus</button><button class="p" onclick="c('k 1')">1</button>
<button class="p" onclick="c('k 2')">2</button><button class="p" onclick="c('k 3')">3</button>
<button onclick="c('k 5')">5</button>
<input id="kk" type="number" min="0" max="20" style="width:56px">
<button onclick="c('k '+v('kk'))">setzen</button></div>
<p class="hint" id="keff">&mdash;</p>
<p class="hint">Vor jedem <b>Anfahren</b> dreht der Zeiger so viele volle Runden, bevor er auf
dem Ziel zur Ruhe kommt &mdash; f&uuml;r die Leuchtspur der Klinge. Es ist <b>kein eigener
Fahrmodus</b>: die Runden werden dem Weg vorangestellt, und daraus wird <b>eine</b> Fahrt mit
<b>einer</b> Rampe. Der Zeiger l&auml;uft also weich an, erreicht das Tempo und bremst weich
auf dem Ziel aus &mdash; und landet schrittgenau.</p>
<p class="hint">Gilt <b>nur</b> f&uuml;r Anfahrten. Uhrbetrieb, Verstellen, Timer und
Referenzfahrt drehen keine Runden.</p></div>

<div class="k"><h2>Pr&uuml;flauf: Pendeln</h2>
<div class="row"><label>Runden</label><input id="xn" type="number" min="1" value="10">
<label style="min-width:32px">von</label><input id="xa" placeholder="12:00" style="width:80px">
<label style="min-width:24px">bis</label><input id="xb" placeholder="18:00" style="width:80px"></div>
<button class="p" onclick="c('x '+v('xn')+' '+v('xa')+' '+v('xb'))">Pendellauf starten</button>
<button onclick="c('s')">abbrechen</button>
<div class="row"><label>schnell</label>
<button onclick="c('x 10 12:00 18:00')">10&times; 12&harr;18</button>
<button onclick="c('x 20 12:00 15:00')">20&times; 12&harr;15</button>
<button onclick="c('x 5 12:00 12:30')">5&times; kurz</button></div>
<p class="hint">Der Zeiger f&auml;hrt n mal hin und her und muss am Ende wieder dort stehen,
wo er angefangen hat. Am Schluss vergleicht die Firmware die gefahrenen Umdrehungen mit den
gez&auml;hlten <b>Hall-Flanken</b> &mdash; nur das beweist, dass die Welle mitgedreht hat. Der
Impulsz&auml;hler z&auml;hlt auch, was der Motor nicht getan hat.</p></div>

<div class="k"><h2>Zw&ouml;lf einmessen</h2>
<p class="hint">Der Winkel zwischen Magnet und Klinge entsteht erst bei der Montage &mdash; jede
Uhr hat ihren eigenen Wert. Drei Schritte, in dieser Reihenfolge. Der Uhrbetrieb schaltet sich
dabei von selbst ab, es ist nichts vorzubereiten.</p>

<div class="row"><label>1. Suchen</label>
<button class="p" onclick="c('h')">Referenzfahrt</button>
<span class="v" id="ez">&mdash;</span></div>

<div class="row"><label>2. Auf zw&ouml;lf</label>
<button onclick="c('v -50')">-50</button><button onclick="c('v -10')">-10</button>
<button onclick="c('v -1')">-1</button><button onclick="c('v 1')">+1</button>
<button onclick="c('v 10')">+10</button><button onclick="c('v 50')">+50</button></div>
<p class="hint">Zeigt die Klinge nach der Referenzfahrt schon auf zw&ouml;lf, ist dieser Schritt
zu &uuml;berspringen.</p>

<div class="row"><label>3. Merken</label>
<button class="p" onclick="c('o!')">Das ist zw&ouml;lf</button>
<span class="v" id="eo">&mdash;</span></div>

<p class="hint">Danach noch einmal <b>Referenzfahrt</b>: landet die Klinge wieder auf zw&ouml;lf,
sitzt der Offset und AP 0.3 ist erledigt. Sonst Schritt 2 und 3 wiederholen.</p>

<details><summary>Fachwerte</summary>
<div class="row"><label>Offset</label><input id="oo" type="number" min="0" max="3199">
<button onclick="c('o '+v('oo'))">setzen</button></div>
<p class="hint">Nur zum <b>Zur&uuml;ckschreiben</b> eines notierten Werts, etwa nach Verlust des
NVS. Zum Einmessen ist &bdquo;Das ist zw&ouml;lf&ldquo; der Weg &mdash; der rechnet den Wert
selbst. Das Feld zeigt den gelebten Offset; eine leere oder unsinnige Eingabe wird abgewiesen,
statt eine 0 zu speichern.</p>
</details></div>

<div class="k"><h2>Timer</h2>
<div class="row"><label>Sekunden</label><input id="ts" type="number" value="300">
<button class="p" onclick="c('t '+v('ts'))">starten</button>
<button onclick="c('t-')">abbrechen</button></div>
<button onclick="c('t 60')">1 min</button><button onclick="c('t 300')">5 min</button>
<button onclick="c('t 3600')">1 h</button>
<p class="hint">Der Zeiger f&auml;hrt erst auf zw&ouml;lf, macht dann genau eine Umdrehung und
blitzt am Ende. Der Uhrbetrieb wird dazu abgeschaltet.</p>
<p class="hint"><b>Die Leerfahrt auf zw&ouml;lf ist dunkel</b> &mdash; sie geh&ouml;rt nicht zur
Anzeige, und eine leuchtende Klinge, die dabei quer &uuml;ber das Blatt wischt, sagt etwas, das
nicht stimmt. Danach ist das Licht wieder wie vorher. Nach dem Alarm f&auml;llt die Uhr in den
Zeitbetrieb zur&uuml;ck.</p></div>

<div class="k"><h2>Hall-Sensor</h2>
<button onclick="c('c')">Spitzenwerte zur&uuml;cksetzen</button>
<p class="hint">Die Spanne soll zwischen 250 und 550 mV liegen. Weniger hei&szlig;t zu gro&szlig;er Magnetabstand, mehr hei&szlig;t abgeschnittene Kurve &mdash; beides ist Mechanik, keine Software.</p></div>

<div class="k"><h2>WLAN einrichten</h2>
<div class="row"><button class="p" onclick="scan()">Netze suchen</button>
<span id="ns" style="color:var(--d);font-size:12px"></span></div>
<div class="row"><label>Netz</label><select id="wn" style="background:#101317;color:var(--t);
 border:1px solid #3a4150;border-radius:6px;padding:5px;font-size:13px;max-width:170px"></select></div>
<div class="row"><label>Passwort</label><input id="wp" type="password" style="width:150px"></div>
<div class="row"><button class="p" onclick="wlan()">verbinden und merken</button>
<button onclick="if(confirm('Gespeicherte Zugangsdaten loeschen?'))
 fetch('/test/wlan?ssid=&pass=').then(t)">vergessen</button></div>
<p class="hint">Die Zugangsdaten landen im Speicher des Ger&auml;ts &mdash; kein neues Flashen.
<b>Der Zugangspunkt bleibt an</b>, bis du die neue Adresse gesehen hast: sie erscheint
oben auf dieser Seite, sobald die Verbindung steht, und die Seite wechselt dann von
selbst dorthin.<br>
Am Zugangspunkt ist diese Seite auch als <b>http://star.watch/</b> erreichbar, im
Heimnetz als <b>http://starwatch.local/</b>.</p></div>

<div class="k"><h2>Ger&auml;t</h2>
<div class="row"><label>Einstellungen</label>
<button onclick="exportieren()">Export</button>
<input type="file" id="imp" accept=".json,application/json" style="width:auto">
<button onclick="importieren()">Import</button></div>
<p class="hint" id="impm">&mdash;</p>
<div class="row"><label>Diagnose</label>
<a href="/test/diagnose" target="_blank"><button>Bericht &ouml;ffnen</button></a></div>
<p class="hint">Alles auf einer Seite in Klartext: Laufzeit, Grund des letzten Starts, freier
Speicher, WLAN-Ausf&auml;lle, Treiberzustand, Referenzfahrt, Aussteuerung. <b>Zum Kopieren und
Verschicken</b> &mdash; damit aus &bdquo;sie spinnt&ldquo; nicht zwanzig R&uuml;ckfragen werden.</p>

<div class="row"><label>Neustart</label>
<button onclick="neustart()">Neustart</button>
<button class="s" onclick="werksreset()">Werkseinstellungen</button></div>
<div class="row"><label>Ger&auml;t</label><span class="v" id="gb_name">&mdash;</span>
<label style="min-width:36px">Serie</label><span class="v" id="gb_serie">&mdash;</span></div>

<details open><summary><b>Ger&auml;teblatt</b> &mdash; aufschreiben, gilt nur f&uuml;r diese Uhr</summary>
<pre id="gb_blatt" style="max-height:none">&mdash;</pre>
<p class="hint">Beim <b>ersten Start</b> gew&uuml;rfelt, danach unver&auml;nderlich; sie
&uuml;berleben Werksreset und Update. Ein gemeinsames Passwort f&uuml;r alle Uhren hie&szlig;e:
wer eine aufschraubt, &ouml;ffnet alle (AP 6.2).</p>
<p class="hint">Das Blatt kommt aus einem <b>gesch&uuml;tzten</b> Endpunkt und steht
absichtlich <b>nicht</b> in der offenen Zustandsabfrage &mdash; sonst h&auml;tte jeder im Netz
das OTA-Passwort lesen und eigene Firmware einspielen k&ouml;nnen.</p>
</details>

<details><summary><b>Zugriffstoken</b> &mdash; n&ouml;tig f&uuml;r Bedienung und Geheimnisse</summary>
<div class="row"><input id="tokf" placeholder="Token einf&uuml;gen" style="width:210px">
<button onclick="tokSetzen()">merken</button>
<button onclick="tokLoeschen()">vergessen</button></div>
<p class="hint" id="tokm">Der Browser merkt es sich. <b>Am eigenen Zugangspunkt der Uhr wird
kein Token verlangt</b> &mdash; wer dort ist, hat dessen Schl&uuml;ssel schon eingegeben. Das
ist der Kopplungsmoment: eine frisch ausgepackte Uhr l&auml;sst sich so einrichten, ohne das
Token vorher zu kennen.</p>
<p class="hint"><b>Offen bleiben</b> Zustand, Log, Diagnose und Netzsuche &mdash; sie tragen
keine Geheimnisse und sind f&uuml;r die Ferndiagnose n&uuml;tzlich. <b>Gesch&uuml;tzt sind</b>
alle Kommandos, Import, Neustart, Werksreset, WLAN einrichten, Export und dieses
Ger&auml;teblatt.</p>
</details>

<p class="hint"><b>Alle Einstellungen &uuml;berleben den Neustart</b> &mdash; Tempo, Rampe,
Schwungkreise, Zeitmodus, Helligkeit, Hall-Offset und WLAN.</p>
<p class="hint"><b>Zwei Speicherbereiche</b> (AP 3.1). <i>Fertigung:</i> der Hall-Offset &mdash;
entsteht bei der Montage, ist f&uuml;r jede Uhr anders, wird <b>nicht exportiert</b> und
<b>&uuml;berlebt den Werksreset</b>. <i>Benutzer:</i> alles andere &mdash; wird exportiert und
vom Werksreset gel&ouml;scht. Ein Import von einer <b>anderen</b> Uhr kann die Kalibrierung
dieser Uhr deshalb nicht zerst&ouml;ren.</p>
<p class="hint">Das <b>WLAN-Passwort</b> steht nicht im Export: dieser Endpunkt ist
ungesichert, solange AP 1.3 fehlt. <b>Autoupdate</b> gibt es noch nicht &mdash; es braucht
AP 3.6 (OTA mit Rollback) und einen Ablageort f&uuml;r das Manifest (AP 6.4).</p></div>

<div class="k"><h2>Motor und Diagnose</h2>
<button onclick="c('g')">Grenzsuche (8 Stufen bis 8 U/s)</button>
<button onclick="c('d')">Treiber auslesen</button>
<button onclick="c('r')">Register neu schreiben</button>
<p class="hint">&bdquo;Treiber auslesen&ldquo; blockiert rund 100 ms am UART &mdash; im Stillstand dr&uuml;cken. Die Grenzsuche findet hier wirklich die Grenze des Motors, weil die Impulse aus der Hardware kommen &mdash; und weil sie <b>Hall-Flanken</b> z&auml;hlt und nicht Impulse. Bleibt der Motor stehen, stimmt der Impulsz&auml;hler trotzdem.</p></div>
</div>

<h2 style="margin:14px 0 6px">Mitschrift</h2>
<pre id="log">&nbsp;</pre>
<div class="row" style="margin-top:6px"><label>Kommando</label>
<input id="cx" placeholder="z. B. f 1200" style="width:150px"
 onkeydown="if(event.key==='Enter'){c(v('cx'));$('cx').select()}">
<button class="p" onclick="c(v('cx'))">senden</button>
<button onclick="c('i')">Zustand ins Log</button>
<button onclick="fetch('/test/log?leeren=1').then(t)">Log leeren</button>
<button onclick="c('?')">Kommandos</button></div>
<p class="hint">Dieselben Kommandos gehen &uuml;ber die serielle Konsole (115200). Beide Wege f&uuml;hren
dieselbe Funktion aus &mdash; deshalb kann die Oberfl&auml;che nicht anders reagieren als das Kabel.</p>

<script>
const $=i=>document.getElementById(i), v=i=>$(i).value.trim();

// --- Zugriffstoken (AP 1.3) ---------------------------------------------
// Einmal einfuegen, danach merkt es der Browser. Am eigenen Zugangspunkt der
// Uhr wird es nicht verlangt - das ist der Kopplungsmoment.
let tok = localStorage.getItem('sw_tok') || '';
function kopf(){return tok?{'X-Starwatch-Token':tok}:{}}
function hol(pfad,opt){opt=opt||{};opt.headers=Object.assign({},opt.headers||{},kopf());
 return fetch(pfad,opt).then(r=>{if(r.status===401)tokFehlt();return r})}
function tokFehlt(){$('tokm').textContent='Abgewiesen - Token fehlt oder stimmt nicht.'}
function tokSetzen(){tok=v('tokf');localStorage.setItem('sw_tok',tok);
 $('tokm').textContent='Gemerkt. Wird ab jetzt mitgeschickt.';blatt_holen()}
function tokLoeschen(){tok='';localStorage.removeItem('sw_tok');
 $('tokm').textContent='Vergessen.'}

function c(k){hol('/test/cmd?k='+encodeURIComponent(k)).then(t)}
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
 let h='';
 for(let i=0;i<12;i++){
  const a=pkt(i*(3200/12),3200,94), b=pkt(i*(3200/12),3200,i%3?86:80);
  h+='<line x1="'+a[0].toFixed(1)+'" y1="'+a[1].toFixed(1)+'" x2="'+b[0].toFixed(1)+
     '" y2="'+b[1].toFixed(1)+'" stroke="var(--d)" stroke-width="'+(i%3?1:2)+'"/>';
 }
 for(const [i,t] of [[0,'12'],[3,'3'],[6,'6'],[9,'9']]){
  const p=pkt(i*(3200/12),3200,66);
  h+='<text x="'+p[0].toFixed(1)+'" y="'+(p[1]+5).toFixed(1)+'" text-anchor="middle"'+
     ' font-size="13" fill="var(--d)">'+t+'</text>';
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
 // Zeiger
 const z=pkt(d.blatt,rev,78);
 $('u_zeiger').setAttribute('x2',z[0].toFixed(1));
 $('u_zeiger').setAttribute('y2',z[1].toFixed(1));
 // Echter Nullpunkt: der Magnet liegt beim Zaehlerstand des Offsets.
 const n=pkt(d.offset,rev,86), np=pkt(d.offset,rev,90);
 $('u_null').setAttribute('x2',n[0].toFixed(1));
 $('u_null').setAttribute('y2',n[1].toFixed(1));
 $('u_nullp').setAttribute('cx',np[0].toFixed(1));
 $('u_nullp').setAttribute('cy',np[1].toFixed(1));
 // Sensorfenster als Bogen um den Magneten, wenn gemessen.
 const f=parseInt(d.fenster,10);
 if(f>0){$('u_fenster').setAttribute('d',
   bogen(((d.offset-f/2)%rev+rev)%rev, (d.offset+f/2)%rev, rev, 90));
  $('u_fenster').style.opacity=0.30}
 else $('u_fenster').style.opacity=0;
 // Ziel nur waehrend der Fahrt.
 if(d.faehrt){const t=pkt(((d.ziel%rev)+rev)%rev,rev,84);
  $('u_ziel').setAttribute('x2',t[0].toFixed(1));
  $('u_ziel').setAttribute('y2',t[1].toFixed(1));
  $('u_ziel').style.opacity=1}
 else $('u_ziel').style.opacity=0;
 $('u_text').textContent=d.zeigt;
 $('u_hint').innerHTML='Zeiger Blatt <b>'+d.blatt+'</b> von '+rev+
  ' &rarr; zeigt '+d.zeigt+'. Nullpunkt bei Blatt <b>'+d.offset+'</b>'+
  (f>0?', Fenster '+f+' Schritte':', Fenster noch nicht gemessen')+'.';
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
 hol('/test/geraeteblatt').then(r=>r.ok?r.text():null).then(x=>{
  $('gb_blatt').textContent = x || 'Token noetig - unten eintragen.';
  // KOPPLUNG: kam das Blatt, OHNE dass wir ein Token hatten, dann hing der
  // Browser am Zugangspunkt der Uhr - dort gilt das Token als erbracht. Wir
  // uebernehmen es jetzt, damit die Bedienung auch nach dem Umzug ins
  // Heimnetz weiterlaeuft. Niemand tippt etwas ab.
  if(x && !tok){
   const m = x.match(/TOKEN=(\S+)/);
   if(m){tok=m[1];localStorage.setItem('sw_tok',tok);
    $('tokm').textContent='Beim Koppeln uebernommen - kein Abtippen noetig.';}
  }
 }).catch(()=>{$('gb_blatt').textContent='nicht abrufbar'});
}

function neustart(){
 if(!confirm('Neustart? Die Uhr braucht danach eine Referenzfahrt.')) return;
 hol('/test/neustart').then(r=>r.text()).then(t=>{$('impm').textContent=t});
}
function werksreset(){
 if(!confirm('Werkseinstellungen?\n\nBenutzerwerte UND WLAN werden geloescht.\n'+
   'Der Hall-Offset bleibt - die Kalibrierung geht nicht verloren.\n\n'+
   'Ohne WLAN ist die Uhr danach nur noch am eigenen Zugangspunkt erreichbar.')) return;
 hol('/test/werksreset').then(r=>r.text()).then(t=>{$('impm').textContent=t});
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
// Nach dem Einrichten auf die neue Adresse wechseln - mit Ankuendigung und
// abbrechbar. Grund fuer den Umweg ueber einen Countdown: der Browser haengt
// noch am Zugangspunkt des Geraets, und ein sofortiger Sprung wuerde die
// Meldung wegnehmen, bevor sie gelesen ist. Wer die Adresse nur notieren will,
// drueckt "hier bleiben".
let umzug=null, rest=0;
function zeigNeu(ip,name){
 $('neu').style.display='block';
 const u='http://'+ip+'/', nm='http://'+name+'/';
 $('neu_ip').textContent=u; $('neu_ip').href=u;
 $('neu_nm').textContent=nm; $('neu_nm').href=nm;
 $('neu_go').onclick=()=>{location.href=u};
 if(umzug!==null||sessionStorage.getItem('sw_bleib')) return;
 rest=10; umzug=setInterval(()=>{
  rest--;
  $('neu_txt').innerHTML='Diese Seite wechselt in <b>'+rest+' s</b> dorthin. '+
   'Dein Ger&auml;t muss dazu im Heimnetz sein &mdash; h&auml;ngt es noch am Zugangspunkt '+
   'des Uhrwerks, w&auml;hle zuerst dein WLAN und komm dann zur&uuml;ck.';
  if(rest<=0){clearInterval(umzug);location.href=u}
 },1000);
 $('neu_txt').innerHTML='Diese Seite wechselt in <b>10 s</b> dorthin.';
 $('neu_stop').onclick=()=>{clearInterval(umzug);umzug=null;
  sessionStorage.setItem('sw_bleib','1');
  $('neu_txt').innerHTML='Bleibt hier. Adresse oben zum Notieren oder Anklicken.'};
}
function scan(){$('ns').textContent='suche …';
 fetch('/test/netze').then(r=>r.json()).then(l=>{
  l.sort((a,b)=>b.rssi-a.rssi);
  $('wn').innerHTML=l.map(n=>'<option value="'+n.ssid.replace(/"/g,'&quot;')+'">'+
   n.ssid+'  ('+n.rssi+' dBm'+(n.offen?', offen':'')+')</option>').join('');
  $('ns').textContent=l.length?l.length+' gefunden':'keins gefunden - nochmal';
 }).catch(()=>{$('ns').textContent='Suche fehlgeschlagen'})}
function wlan(){const n=$('wn').value;
 if(!n){$('ns').textContent='erst ein Netz waehlen';return}
 $('ns').textContent='verbinde mit '+n+' …';
 fetch('/test/wlan?ssid='+encodeURIComponent(n)+'&pass='+encodeURIComponent(v('wp')))
  .then(r=>r.text()).then(x=>{$('ns').textContent=x.split('\n')[0];$('wp').value=''})
  .catch(()=>{$('ns').textContent='Verbindung hierher beendet - siehe serielle Ausgabe'})}
function z(){fetch('/test/zustand').then(r=>r.json()).then(d=>{
 $('net').textContent=d.netz;
 const r=[['Uhrzeit',d.uhr+'  ('+d.modus+')'],['Zeitquelle',d.zeitquelle],['Zeiger','Schritt '+d.pos+'  &rarr; '+d.blatt+' auf dem Blatt  &rarr; zeigt '+d.zeigt],
  ['Ziel',d.ziel+(d.faehrt?'  F&Auml;HRT':'  steht')],['F&uuml;hrung',d.fuehrung],
  ['Abweichung',d.drift],['Endstufe',d.endstufe],['Referenz','<span class="'+kl(d.homing)+'">'+d.homing+'</span>'],
  ['R&uuml;ckfall',d.rueckfall],['Hall-Offset',d.offset+' Schritte'],['Hall D0',d.d0],['Hall A0',d.a0+' V  (Ruhe '+d.ruhe+' V)'],
  ['Hall-Spanne','<span class="'+d.spanne_kl+'">'+d.spanne+' mV</span>'],
  ['Hall-Fenster',d.fenster],['Licht',d.licht],['Effekt',d.effekt],['Timer',d.timer],
  ['Treiber',d.treiber]];
 $('z').innerHTML=r.map(x=>'<tr><td>'+x[0]+'</td><td class="v">'+x[1]+'</td></tr>').join('');
 $('tmin').textContent=(d.tmin/1000).toFixed(2)+' s je Umdrehung, bei '+
  d.tempo+' Schritten/s ('+(d.tempo/d.rev).toFixed(2)+' U/s) und Rampe '+d.rampe+'.';
 if(document.activeElement!==$('pf')) $('pf').value=d.tempo;
 if(document.activeElement!==$('pa')) $('pa').value=d.rampe;
 if(document.activeElement!==$('kk')) $('kk').value=d.kreise;
 if(document.activeElement!==$('tz')) $('tz').value=d.tz;
 $('gb_name').textContent=d.geraet;
 $('gb_serie').textContent=d.serie;
 $('keff').innerHTML = d.kreise>0
  ? '<b>'+d.kreise+' Runde'+(d.kreise>1?'n':'')+'</b> vor dem Ziel &rarr; ein Anfahren dauert '+
    (d.keff_min/1000).toFixed(2)+' bis '+(d.keff_max/1000).toFixed(2)+' s ('+
    (d.kreise*d.rev)+' bis '+((d.kreise+1)*d.rev)+' Schritte).'
  : 'Aus &mdash; Anfahrten gehen direkt auf das Ziel.';
 blatt(d);
 $('ez').innerHTML='<span class="'+kl(d.homing)+'">'+d.homing+'</span>';
 $('eo').textContent='Offset '+d.offset+' Schritte';
 // Das Feld zeigt den gelebten Wert - aber nie waehrend getippt wird, sonst
 // ueberschreibt der 1-Sekunden-Takt die Eingabe.
 if(document.activeElement!==$('oo')) $('oo').value=d.offset;
 if(d.heimnetz){zeigNeu(d.heimnetz,d.name);}
 else {$('neu').style.display='none';}})
 .catch(()=>{$('net').textContent='keine Verbindung'})}
function lg(){fetch('/test/log').then(r=>r.text()).then(x=>{const p=$('log');
 const u=p.scrollTop+p.clientHeight>=p.scrollHeight-24;p.textContent=x||' ';if(u)p.scrollTop=p.scrollHeight})}
z();lg();blatt_holen();setInterval(z,1000);setInterval(lg,1500);
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

bool Testweb::beginn(Antrieb *antrieb, Hall *hall, Licht *licht, Betrieb *betrieb,
                     Kommandofn kommando) {
  antrieb_ = antrieb;
  hall_ = hall;
  licht_ = licht;
  betrieb_ = betrieb;
  betrieb_zeiger = betrieb;
  kommando_ = kommando;
  aktiv = this;

  nvs.begin(NVS_BEREICH, false);
  const String gespeichert = nvs.getString(NVS_SSID, "");
  if (gespeichert.length() > 0) {
    // In NVS eingerichtete Zugangsdaten haben VORRANG vor zugangsdaten.h:
    // sie sind die spaetere, ausdrueckliche Angabe des Benutzers, und sie zu
    // uebergehen waere die unangenehmere Ueberraschung.
    const String pass = nvs.getString(NVS_PASS, "");
    konsole.printf("WLAN aus NVS: verbinde mit '%s' ", gespeichert.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(SENDELEISTUNG);
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
    static const char *koepfe[] = {"X-Starwatch-Token"};
    server.collectHeaders(koepfe, 1);
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
  WiFi.setTxPower(SENDELEISTUNG);  // VOR dem Verbinden, siehe SENDELEISTUNG
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
    static const char *koepfe[] = {"X-Starwatch-Token"};
    server.collectHeaders(koepfe, 1);
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

  // Rueckfall: eigener Zugangspunkt. Damit ist die Oberflaeche auch ohne
  // Konfiguration erreichbar - man soll den Aufbau pruefen koennen, ohne
  // vorher Zugangsdaten einzutragen.
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(SENDELEISTUNG);
  // SSID und Schluessel je Geraet (AP 6.2). Die Seriennummer im Namen ist
  // nicht Zierde: stehen beim Aufbau zwei Uhren nebeneinander, sind sie sonst
  // nicht auseinanderzuhalten.
  const String ap_name = String("StarWatch-") + betrieb_->seriennummer().c_str();
  const String ap_pw = betrieb_->ap_passwort().c_str();
  if (!WiFi.softAP(ap_name.c_str(), ap_pw.c_str())) {
    konsole.println("Zugangspunkt liess sich nicht aufspannen. Es bleibt die\n"
                    "serielle Konsole - die kann alles, was die Oberflaeche kann.");
    return false;
  }
  verbunden_ = true;
  eigener_ap_ = true;
  sta_aktiv_ = false;
  // Schluessel wie beim OTA-Passwort - siehe dort.
  Serial.printf("Zugangspunkt '%s', Schluessel '%s'.\n", ap_name.c_str(),
                ap_pw.c_str());
  if (g::ENTWICKLUNGSGERAET)
    konsole.printf("Zugangspunkt '%s' steht, Schluessel '%s'.\n", ap_name.c_str(),
                   ap_pw.c_str());
  else
    konsole.printf("Zugangspunkt '%s' steht (Schluessel im Geraeteblatt).\n",
                   ap_name.c_str());
  konsole.printf("Oberflaeche: http://%s/\n", WiFi.softAPIP().toString().c_str());
  routen_setzen();
  // Der ESP32-Webserver speichert NUR Kopfzeilen, die vorher angemeldet sind.
  // Ohne diese Zeile bliebe `server.header(...)` immer leer, und das Token
  // waere nur ueber den Abfrageteil zu uebergeben.
  {
    static const char *koepfe[] = {"X-Starwatch-Token"};
    server.collectHeaders(koepfe, 1);
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
  const float spanne_mv = hall_->spanne_v() * 1000.0f;

  const char *wer = "niemand";
  switch (betrieb_->fuehrung()) {
    case Fuehrung::homing:  wer = "Referenzfahrt"; break;
    case Fuehrung::hand:    wer = "Hand";          break;
    case Fuehrung::timer:   wer = "Timer";         break;
    case Fuehrung::uhr:     wer = "Uhrbetrieb";    break;
    case Fuehrung::niemand: break;
  }

  // Die Bewertung der Aussteuerung gehoert in die Anzeige, nicht in den Kopf
  // des Ablesenden: ausserhalb 250..550 mV ist der Index unsicher.
  const char *spanne_kl = "";
  if (spanne_mv > 1.0f)
    spanne_kl = (spanne_mv < g::HALL_SPANNE_MIN_V * 1000.0f)
                    ? "warn"
                    : ((spanne_mv > g::HALL_SPANNE_MAX_V * 1000.0f) ? "warn" : "ok");

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
                       : (String(hall_->fenster_schritte()) + " Schritte" +
                          (hall_->fenster_schritte() >= g::HALL_RUECKZUG_SCHRITTE
                               ? String(" - GROESSER ALS DER RUECKZUG (") +
                                     g::HALL_RUECKZUG_SCHRITTE + ")"
                               : String()));

  String s = "{";
  s += "\"netz\":\"" + json_text(netz) + "\",";
  // Die neue Adresse ausdruecklich als eigenes Feld: die Oberflaeche zeigt sie
  // gross an, sobald sie da ist. Ohne das muesste man sie am Kabel ablesen.
  s += "\"heimnetz\":\"" +
       String((sta_aktiv_ && WiFi.status() == WL_CONNECTED)
                  ? WiFi.localIP().toString()
                  : String()) + "\",";
  s += "\"name\":\"starwatch.local\",";
  s += "\"geraet\":\"" + json_text(String(betrieb_->geraetename().c_str())) + "\",";
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
       (soll < 0 ? String("- (kein Uhrbetrieb)")
                 : String(drift(pos, soll, g::REV)) + " Schritte") + "\",";
  s += "\"endstufe\":\"" +
       String(antrieb_->endstufe_an() ? "bestromt" : "AUS (Freilauf)") + "\",";
  s += "\"homing\":\"" +
       json_text(hominggrund_text(betrieb_->letztes_homing().grund)) + "\",";
  s += "\"offset\":" + String(betrieb_->hall_offset()) + ",";
  s += "\"d0\":\"" + String(hall_->magnet_erkannt() ? "MAGNET" : "frei") + "\",";
  s += "\"a0\":\"" + String(hall_->spannung_gefiltert(), 3) + "\",";
  s += "\"ruhe\":\"" + String(hall_->ruhe_v(), 3) + "\",";
  s += "\"spanne\":\"" + String(spanne_mv, 0) + "\",";
  s += "\"spanne_kl\":\"" + String(spanne_kl) + "\",";
  s += "\"fenster\":\"" + json_text(fenster) + "\",";
  s += "\"licht\":\"" + String(licht_->ist_an() ? "an" : "aus") + ", Regler " +
       String(licht_->soll_prozent(), 0) + " %, Ausgabe " +
       String(licht_->ausgabe_prozent(), 1) + " %\",";
  s += "\"effekt\":\"" + String(effekt_text(licht_->effekt())) + "\",";
  s += "\"timer\":\"" + String(betrieb_->timer_zustand_text()) +
       ", Rest " + String(betrieb_->timer_restzeit().c_str()) + "\",";
  // NICHT den Treiber abfragen: das blockiert 100 ms am UART, und diese
  // Antwort geht jede Sekunde raus. Der Knopf "Treiber auslesen" tut es.
  s += "\"treiber\":\"" +
       String(antrieb_->endstufe_an() ? "siehe 'Treiber auslesen'" : "Endstufe aus") +
       "\"}";
  return s;
}

void Testweb::ota_einrichten() {
  ArduinoOTA.setHostname("starwatch");
  ArduinoOTA.setPassword(betrieb_->ota_passwort().c_str());

  ArduinoOTA.onStart([]() {
    // ZUERST den Zeiger anhalten. Ein Update, das mitten in einer Fahrt
    // beginnt, laesst den Motor stehen und die Endstufe bestromt - und nach
    // dem Neustart weiss niemand, wo der Zeiger steht. Ausserdem reisst ein
    // Stillstand ohne Rampe dem Riemen einen Zahn ab, und das sind 18 Grad
    // = 36 min Anzeigefehler.
    if (aktiv != nullptr && aktiv->betrieb_ != nullptr)
      aktiv->betrieb_->stopp();
    if (aktiv != nullptr && aktiv->licht_ != nullptr) {
      aktiv->licht_->effekt_abbrechen();
      aktiv->licht_->an(false);
    }
    konsole.println("\nOTA: Update beginnt. Zeiger angehalten, Licht aus.");
  });

  ArduinoOTA.onEnd([]() {
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
    const char *t = "unbekannt";
    switch (fehler) {
      case OTA_AUTH_ERROR:    t = "Passwort falsch"; break;
      case OTA_BEGIN_ERROR:   t = "Start misslungen - passt das Image in die Partition?"; break;
      case OTA_CONNECT_ERROR: t = "Verbindung abgebrochen"; break;
      case OTA_RECEIVE_ERROR: t = "Empfang abgebrochen"; break;
      case OTA_END_ERROR:     t = "Abschluss misslungen"; break;
    }
    konsole.printf("\nOTA FEHLER: %s\n", t);
  });

  ArduinoOTA.begin();
  log_server.begin();
  log_server.setNoDelay(true);

  // mDNS fuer das Heimnetz. ArduinoOTA startet mDNS schon; der Dienst wird
  // hier ausdruecklich angemeldet, damit die Weboberflaeche auffindbar ist
  // und nicht nur der Update-Port.
  MDNS.addService("http", "tcp", 80);

  // WOHIN das Passwort gemeldet wird, haengt an der Fassung. `konsole` schreibt
  // auch in den Ringpuffer, und den liefert `/test/log` OFFEN aus - in der
  // Auslieferung waere das ein Selbsttor: wer es liest, spielt eigene Firmware
  // ein. In der ENTWICKLUNGSfassung muss es dort trotzdem stehen, sonst kommt
  // niemand mehr per OTA an ein Geraet, das nur ueber VPN erreichbar ist.
  Serial.printf("OTA steht (Passwort '%s').\n", betrieb_->ota_passwort().c_str());
  if (g::ENTWICKLUNGSGERAET)
    konsole.printf("OTA steht (Passwort '%s'). Log ueber Netz auf Port %u.\n",
                   betrieb_->ota_passwort().c_str(), LOG_PORT);
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
  if (aktiv != nullptr && aktiv->eigener_ap())
    return true;  // Kopplung am eigenen Zugangspunkt
  if (betrieb_zeiger == nullptr)
    return true;  // vor der Einrichtung nichts zu schuetzen
  const String soll = betrieb_zeiger->token().c_str();
  if (soll.length() == 0)
    return true;
  if (server.hasHeader("X-Starwatch-Token") &&
      server.header("X-Starwatch-Token") == soll)
    return true;
  if (server.hasArg("t") && server.arg("t") == soll)
    return true;
  return false;
}

/// Antwort auf einen Zugriff ohne Token.
void abgewiesen() {
  server.send(401, "text/plain; charset=utf-8",
              "Kein gueltiges Token.\n\n"
              "Diese Uhr verlangt fuer Bedienung und Geheimnisse ein Token "
              "(AP 1.3).\nEs steht im Geraeteblatt: beim Start auf der Konsole, "
              "oder in der\nOberflaeche, solange du am eigenen Zugangspunkt "
              "der Uhr haengst.\n");
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
        } else if (log_befugt || (aktiv != nullptr && aktiv->eigener_ap())) {
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
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", SEITE);
  });

  server.on("/test/zustand", HTTP_GET, []() {
    if (aktiv == nullptr)
      return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", aktiv->zustand_json());
  });

  server.on("/test/log", HTTP_GET, []() {
    if (server.hasArg("leeren"))
      konsole.log_leeren();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", konsole.log());
  });

  server.on("/test/cmd", HTTP_GET, []() {
    if (aktiv == nullptr) {
      server.send(500, "text/plain", "keine Kommandofunktion");
      return;
    }
    if (!befugt()) {
      abgewiesen();
      return;
    }
    String k = server.arg("k");
    if (k.length() == 0) {
      server.send(400, "text/plain", "kein Kommando");
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
    if (aktiv == nullptr || betrieb_zeiger == nullptr)
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

  server.on("/test/diagnose", HTTP_GET, []() {
    if (aktiv == nullptr)
      return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", aktiv->diagnose_text());
  });

  server.on("/test/export", HTTP_GET, []() {
    if (aktiv == nullptr)
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
    if (aktiv == nullptr)
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

  server.on("/test/neustart", HTTP_GET, []() {
    if (!befugt()) {
      abgewiesen();
      return;
    }
    server.send(200, "text/plain; charset=utf-8", "Neustart in 2 s.");
    konsole.println("\n[web] Neustart angefordert.");
    neustart_ab_ms = millis();
    neustart_mit_reset = false;
  });

  server.on("/test/werksreset", HTTP_GET, []() {
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

  server.on("/test/netze", HTTP_GET, []() {
    if (aktiv == nullptr)
      return;
    const int n = WiFi.scanNetworks();
    String j = "[";
    for (int i = 0; i < n; i++) {
      if (i) j += ",";
      j += "{\"ssid\":\"" + json_text(WiFi.SSID(i)) + "\",\"rssi\":" +
           String(WiFi.RSSI(i)) + ",\"offen\":" +
           (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    j += "]";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", j);
  });

  server.on("/test/wlan", HTTP_GET, []() {
    if (!befugt()) {
      abgewiesen();
      return;
    }
    if (aktiv == nullptr)
      return;
    // Erst antworten, dann umschalten: die Verbindung zum Browser bricht beim
    // Wechsel vom Zugangspunkt in das Heimnetz ohnehin ab.
    const String ssid = server.arg("ssid");
    const String pass = server.arg("pass");
    server.send(200, "text/plain",
                ssid.length()
                    ? "Verbinde mit '" + ssid +
                          "'. Diese Seite bleibt erreichbar - die neue Adresse "
                          "erscheint oben, sobald die Verbindung steht."
                    : "Zugangsdaten geloescht.");
    delay(200);
    aktiv->wlan_setzen(ssid, pass);
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
    konsole.printf("  %-32s %4d dBm  %s\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                   WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "offen" : "verschluesselt");
  if (n == 0)
    konsole.println("  (keins - im AP-Modus kann der Scan leer bleiben, nochmal versuchen)");
  return n;
}

String Testweb::gespeicherte_ssid() const {
  return nvs.getString(NVS_SSID, "");
}

bool Testweb::wlan_setzen(const String &ssid, const String &passwort) {
  if (ssid.length() == 0) {
    nvs.remove(NVS_SSID);
    nvs.remove(NVS_PASS);
    konsole.println("Zugangsdaten geloescht. Nach dem naechsten Start laeuft "
                    "wieder der eigene Zugangspunkt.");
    return true;
  }

  nvs.putString(NVS_SSID, ssid);
  nvs.putString(NVS_PASS, passwort);
  // NUR die SSID ins Log. Der Ringpuffer ist ueber die Weboberflaeche
  // abrufbar, und ein WLAN-Passwort hat dort nichts zu suchen.
  konsole.printf("\nGespeichert: '%s' (Passwort %d Zeichen, nicht im Log).\n",
                 ssid.c_str(), passwort.length());
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
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(SENDELEISTUNG);
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
  WiFi.disconnect();
  sta_aktiv_ = false;
  verbunden_ = true;
  eigener_ap_ = true;
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

  t += "REFERENZ UND HALL\n";
  t += "  Letzte Fahrt   " + String(hominggrund_text(betrieb_->letztes_homing().grund)) + "\n";
  if (betrieb_->letztes_homing().ok()) {
    t += "  Schwerpunkt    " + String(betrieb_->letztes_homing().schwerpunkt, 2) + "\n";
    t += "  Nachlauf       " + String(betrieb_->letztes_homing().nachlauf) + "\n";
    t += "  Aussteuerung   " + String(betrieb_->letztes_homing().spanne_v * 1000.0f, 0) +
         " mV (Soll " + String(g::HALL_SPANNE_MIN_V * 1000.0f, 0) + " bis " +
         String(g::HALL_SPANNE_MAX_V * 1000.0f, 0) + ")\n";
  }
  t += "  Fenster        " + String(hall_->fenster_schritte()) + " Schritte (Rueckzug " +
       String(g::HALL_RUECKZUG_SCHRITTE) + ")\n";
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
  j += "  \"hinweis\": \"Ohne Hall-Offset (Fertigungswert) und ohne "
       "WLAN-Passwort.\",\n";
  j += "  \"einstellungen\": {\n";
  j += "    \"tempo\": " + String(betrieb_->fahrtempo()) + ",\n";
  j += "    \"rampe\": " + String(antrieb_->beschleunigung()) + ",\n";
  j += "    \"kreise\": " + String(betrieb_->effekt_kreise()) + ",\n";
  j += "    \"modus\": " + String((int)betrieb_->zeitmodus()) + ",\n";
  j += "    \"licht\": " +
       String(licht_ != nullptr ? licht_->soll_prozent() : 0.0f, 1) + ",\n";
  j += "    \"wlan_ssid\": \"" + json_text(gespeicherte_ssid()) + "\"\n";
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
  if (json_zahl(datei, "licht", &v) && v >= 0 && v <= 100 && licht_ != nullptr) {
    licht_->soll_prozent((float)v);
    bericht += "Licht " + String((float)v, 1) + " %, ";
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
      MDNS.addService("http", "tcp", 80);
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
  server.handleClient();
  if (dns_laeuft)
    dns.processNextRequest();
  ArduinoOTA.handle();
  log_tick();
}

}  // namespace starwatch
