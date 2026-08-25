// ---------------------------------------------------------------------------
// Star Watch - Geraetedaten: Pins, Kennzahlen, Treiberwerte. EINE Quelle.
//
// Warum diese Datei eine BIBLIOTHEK ist und nicht in include/ liegt: eine
// Bibliothek unter lib/ sieht include/ NICHT. Wuerden die Pins dort liegen,
// koennte lib/antrieb sie nicht lesen und muesste eigene Zahlen fuehren -
// genau das, was hier verhindert wird.
//
// Alles hier ist constexpr und haengt an NICHTS: kein Arduino.h, kein
// ESP-IDF, keine Register. Nur so laesst sich die Datei auch im native-Build
// uebersetzen und im Host-Test pruefen (test/test_geraet).
//
// HERKUNFT JEDER ZAHL steht als Kommentar dabei. Ohne Herkunft kommt keine
// Zahl hier herein - eine geratene Konstante an einer zentralen Stelle ist
// schlimmer als eine geratene Konstante an einer einzelnen.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace starwatch {
namespace geraet {

// ===========================================================================
// Pins. Quelle: docs/Arbeitsstand.md Abschnitt 1, alle neun Adern am Aufbau
// verbaut und geprueft (22.08.2026).
// ===========================================================================

// UART ist EIN Draht: TX ueber 1 kOhm, RX direkt, beide auf DASSELBE PDN-Pad.
constexpr uint8_t PIN_UART_TX = 17;
constexpr uint8_t PIN_UART_RX = 16;

constexpr uint8_t PIN_STEP = 25;
constexpr uint8_t PIN_DIR  = 32;  // seit 08.08.2026, war GPIO26. Auf dem
                                  // D1 Mini liegt 32 neben STEP und EN.
constexpr uint8_t PIN_EN   = 27;  // ENN, LOW-aktiv. 10 kOhm nach 3,3 V.

// ### DREHRICHTUNG - GEMESSEN AM GERAET, NICHT ABGELEITET ##################
//
// Bedeutet: zaehlt der Zaehler AUFWAERTS, wenn DIR auf HIGH liegt?
//
// Das ganze Projekt haengt an einer Zusicherung: VORWAERTS IST IM
// UHRZEIGERSINN. Darauf bauen `delta_vorwaerts` in lib/zeiger (der Zeiger
// korrigiert nie rueckwaerts), das Riemenspiel (bleibt so immer auf derselben
// Seite, siehe Briefing Abschnitt 6) und jede Zielrechnung. Stimmt die
// Richtung nicht, ist nicht "eine Fahrt falsch" - dann ist das Blatt
// gespiegelt: 3 und 9 tauschen die Plaetze, 12 und 6 bleiben, wo sie sind.
//
// GENAU DAS TRAT AM 23.08.2026 AUF. Die Ursache liegt am 21.08.2026: dort
// wurde die Spulenpaarung korrigiert (die beiden mittleren Adern am
// Treiberende getauscht, siehe Aenderungsblatt). Das war richtig und hat den
// Motor erst zum Laufen gebracht - aber die Reihenfolge der Phasen A und B
// bestimmt eben auch die Drehrichtung, und die kam dabei verkehrt heraus.
//
// HIER UND NICHT AM KABEL: ein getauschtes Adernpaar am Treiber wuerde
// dasselbe bewirken, aber der Verkabelungsplan stimmt seit dem 21.08. mit dem
// Datenblatt des Motors ueberein. Diese eine Zeile ist umkehrbar, sie ist
// dokumentiert, und sie gilt fuer beide Umgebungen (Betriebsfirmware und
// Messmenue), weil beide diese Konstante lesen.
//
// FOLGE FUER DEN OFFSET: der Hall-Offset ist die Strecke vom Magneten bis
// zwoelf, IN FAHRTRICHTUNG gemessen. Dreht die Richtung, wird daraus
// REV - alt. Ein vor dieser Aenderung eingemessener Offset ist damit
// ungueltig und muss neu gesetzt werden ('o!').
constexpr bool DIR_HIGH_ZAEHLT_AUFWAERTS = false;
// ##########################################################################

// INDEX ist verdrahtet und liefert bei DIESEM Aufbau nichts Verwertbares
// (Arbeitsstand Abschnitt 4). Die Betriebsfirmware braucht ihn nicht:
// FastAccelStepper zaehlt seine eigenen Impulse in Hardware (PCNT). Der Pin
// bleibt als kostenlose Selbstueberwachung stehen, nicht als Positionsquelle.
constexpr uint8_t PIN_INDEX = 35;  // input-only, der Treiber treibt aktiv

constexpr uint8_t PIN_HALL_D0 = 34;  // digital, externer 10-kOhm-Pull-up
constexpr uint8_t PIN_HALL_A0 = 39;  // SS49E analog. ADC1 ist PFLICHT -
                                     // ADC2 ist bei aktivem WLAN unbrauchbar.
constexpr uint8_t PIN_LED_PWM = 33;  // LD24AJTA (PT4115), Strom am Trimmer

// ===========================================================================
// Antrieb. Riemen 1:1 - keine Untersetzung. Ein uebersprungener Zahn sind
// 18 Grad = 36 min Anzeigefehler.
// ===========================================================================

constexpr int32_t VOLLSCHRITTE = 200;  // NEMA 17, 1,8 Grad, Riemen 1:1

// ### OFFENE ENTSCHEIDUNG AP 1.1 ###########################################
// Briefing Abschnitt 5 schlaegt 1/64 vor: der Zeiger zuckt dann alle 3,4 s um
// 60 um an der Spitze statt alle 13,5 s um 240 um. Feineres Microstepping
// verbessert NICHT die Genauigkeit - unterhalb 1/16 dominieren Rastmoment und
// Reibung, und der groesste Einzelfehler ist ohnehin die Nabenexzentrizitaet
// mit 1,57 Grad. Es ist eine Frage der Anmutung, nicht der Aufloesung.
//
// Steht auf 16, weil das der am Aufbau gefahrene Wert ist. Wird die
// Entscheidung anders, ist DIESE Zeile die einzige, die sich aendert -
// REV und SEK_JE_SCHRITT rechnen sich daraus.
constexpr int32_t MIKROSCHRITTE = 16;
// ##########################################################################

/// Mikroschritte einer vollen Zeigerumdrehung = 12 Stunden.
constexpr int32_t REV = VOLLSCHRITTE * MIKROSCHRITTE;

/// Sekunden je Mikroschritt. 12 h = 43200 s.
constexpr float SEK_JE_SCHRITT = 43200.0f / static_cast<float>(REV);

/// Winkel je Mikroschritt in Grad. Nach aussen (API) werden Grad gereicht,
/// nie Mikroschritte - App-und-HA-Architektur.md Abschnitt 4.
constexpr float GRAD_JE_SCHRITT = 360.0f / static_cast<float>(REV);

// ===========================================================================
// TMC2209. Modul ist eine V2.0, NICHT die BigTreeTech V1.2, von der die
// aelteren Unterlagen ausgehen.
// ===========================================================================

/// Sense-Widerstaende. Aufdruck "R110" am 08.08.2026 am Bauteil bestaetigt.
/// Daraus: Vollausschlag 1,77 A RMS, Vref 0,55 V fuer 0,4 A RMS.
constexpr float R_SENSE = 0.11f;

constexpr uint32_t TMC_BAUD = 115200;
// 500000 war zu hoch: der TMC2209 taktet seinen UART aus dem internen
// RC-Oszillator, und die Eindrahtverbindung ueber den 1 kOhm verschleift die
// Flanken. 115200 laeuft stabil (21.08.2026).

// ### OFFEN BIS AP 0.6 #####################################################
// 0x00 ist der IST-Zustand: MS1 und MS2 sind am Modul nicht beschaltet, die
// internen Pull-downs ziehen sie auf LOW. Mit 0x03 antwortete der Treiber nie
// ("Unable to read IC version"), mit 0x00 sofort.
// AP 0.6 legt beide Pins fest auf 3,3 V - DANACH gilt wieder 0x03, und die
// Adresse haengt nicht mehr an zwei offenen Eingaengen.
constexpr uint8_t TMC_ADDR = 0x00;
// ##########################################################################

constexpr uint16_t STROM_LAUF = 400;  // mA RMS
constexpr uint16_t STROM_HALT = 300;  // mA RMS. NIE 0 - ohne Haltestrom
                                      // kippt die Klinge aus ihrer Lage.
constexpr uint8_t TOFF = 4;           // 0 waere die Endstufe aus

// ===========================================================================
// TEMPO UND RAMPE
// ===========================================================================
// Bis 23.08.2026 stand das Fahrtempo ueberall fest auf 800 Schritte/s - das
// ist 1/4 Umdrehung je Sekunde, und mehr gab die Firmware nicht her. Die
// Grenzsuche pruefte ausserdem nur bis 3200 (1 U/s), also nicht einmal bis an
// den Bereich, um den es geht.
//
// DREI DINGE MUSSTEN DAFUER ZUSAMMENKOMMEN, nicht nur die Zahl:
//
//   1 Das Tempo selbst (hier).
//   2 Die Rampe. Mit den alten 2000 Schritten/s^2 braucht der Zeiger bis
//     19200 Schritte/s ganze 9,6 s und dabei 29 Umdrehungen - die Zahl allein
//     zu erhoehen haette gar nichts gebracht.
//   3 Den Chopper. Der Treiber lief ausschliesslich in stealthChop
//     (`en_spreadCycle(false)`, TPWMTHRS unbesetzt = nie umschalten).
//     StealthChop ist leise, verliert aber mit steigendem Tempo sein
//     Drehmoment. Ohne die Umschaltung nach spreadCycle bleibt der Motor
//     oberhalb von etwa 1 U/s einfach stehen - und der Impulszaehler merkt
//     davon NICHTS.
//
// WAS HIER STEHT, IST DIE OBERGRENZE DER KOMMANDOS, NICHT EIN MESSWERT.
// Was die Mechanik wirklich mitmacht, sagt die Grenzsuche 'g' - sie zaehlt
// dafuer Hall-Flanken und nicht Impulse. Der belastbare Wert gehoert nach
// 06_Fertigung/Messprotokoll.md und danach in TEMPO_FAHRT.

/// Hoechstes Tempo, das ein Kommando annimmt. 8 U/s = 480 min^-1 am NEMA 17.
///
/// WARUM GERADE 8 UND NICHT MEHR: die Grenzsuche weist das Drehen ueber
/// Hall-Flanken nach, und D0 wird 10 ms entprellt (D0_ENTPRELL_MS in
/// hall.cpp). Das Sensorfenster ist rund 530 Schritte breit, bei 8 U/s also
/// noch 20,7 ms - doppelt so lang wie die Entprellung. Schneller wuerde die
/// MESSMETHODE unsicher, nicht der Antrieb: eine verpasste Flanke sieht aus
/// wie ein Schrittverlust. Wer hoeher messen will, muss zuerst die
/// Entprellung anfassen.
constexpr uint32_t TEMPO_MAX = 8 * REV;

/// Tempo der Handfahrten und Anfahrten.
///
/// GEMESSEN AM 23.08.2026 mit der Grenzsuche: alle acht Stufen von 1 bis 8 U/s
/// drehten mit vollstaendigen Hall-Flanken (3/3), und die gemessene Fahrzeit
/// deckte sich auf 20 ms mit der gerechneten. Die Mechanik ist bei 8 U/s
/// (480 min^-1) NICHT am Ende - dort endet nur die Messmethode, siehe
/// TEMPO_MAX. 6 U/s laesst damit rund ein Viertel Abstand nach oben.
constexpr uint32_t TEMPO_FAHRT = 6 * REV;

/// Tempo des Uhrbetriebs - bewusst LANGSAM, nicht aus Vorsicht, sondern wegen
/// der Lautstaerke: unterhalb von SPREAD_AB_TEMPO bleibt der Treiber in
/// stealthChop und ist still. Die Uhr fuehrt einen Mikroschritt alle 13,5 s,
/// da ist Tempo ohne Wert - aber ein Uhrwerk, das dabei klackt, ist ein
/// Fehler. Holt sie nach einem Eingriff Rueckstand auf, dauert eine ganze
/// Umdrehung damit 8 s; das ist hinnehmbar und bleibt leise.
constexpr uint32_t TEMPO_UHR = REV / 8;

/// Beschleunigung. Aus Tempo und gewuenschter Anlaufzeit GERECHNET, nicht
/// getippt: aus dem Stand auf TEMPO_MAX in einer halben Sekunde. Der Weg
/// dafuer ist v^2/(2a), also rund 1,5 Umdrehungen.
constexpr uint32_t ANLAUF_MS = 500;
constexpr uint32_t BESCHLEUNIGUNG = TEMPO_MAX * 1000 / ANLAUF_MS;

/// Tempo, ab dem der Treiber von stealthChop auf spreadCycle umschaltet.
/// Darunter leise, darueber kraeftig.
constexpr uint32_t SPREAD_AB_TEMPO = REV / 2;  // 0,5 U/s

/// Interner Takt des TMC2209.
constexpr uint32_t TMC_FCLK = 12000000;

/// TPWMTHRS ist eine ZEIT, kein Tempo: der Treiber vergleicht sie mit TSTEP,
/// dem gemessenen Abstand zweier 1/256-Mikroschritte in Einheiten von 1/fCLK.
/// Ein m-Mikroschritt sind 256/m davon, also
///   TSTEP = fCLK * m / (Tempo * 256).
/// Deshalb steht hier die Rechnung und keine Zahl - sie zieht mit
/// MIKROSCHRITTE mit, und ein Wechsel auf 1/64 verstellt die Schwelle sonst
/// unbemerkt um den Faktor 4.
constexpr uint32_t TPWMTHRS_WERT =
    TMC_FCLK * MIKROSCHRITTE / (SPREAD_AB_TEMPO * 256);

/// Hoechstzahl der Schwungkreise vor einem Ziel (Effekt "Lichtschwert").
///
/// Die Grenze ist NICHT thermisch und nicht mechanisch - bei 6 U/s sind 20
/// Kreise gut drei Sekunden, und der Motor haelt Dauerlauf aus. Sie ist gegen
/// den Vertipper: 'k 200' waere eine halbe Minute Kreisen, in der die Uhr
/// nichts anzeigt und kein Kommando durchkommt.
constexpr int32_t EFFEKT_KREISE_MAX = 20;

// ===========================================================================
// Kennung fuer Export und Import (AP 3.7.2)
// ===========================================================================

/// Geraetetyp. Ein Import prueft ihn und lehnt fremde Dateien ab.
constexpr const char *GERAETETYP = "star-watch";

/// Firmware-Stand als SEMVER.
///
/// MUSS semver sein, seit es Updates gibt (AP 3.7.1): das Manifest vergleicht
/// Versionen, und "AP2-2026-08-23" laesst sich nicht vergleichen. Beim Erhoehen
/// gilt die uebliche Regel - letzte Stelle fuer Fehlerbehebungen, mittlere fuer
/// neue Funktionen.
///
/// 0.x heisst: noch nicht ausgeliefert.
constexpr const char *FIRMWARE_VERSION = "0.9.5";

// ===========================================================================
// UPDATES AUS DEM NETZ (AP 3.7.1)
// ===========================================================================

/// Wo das Manifest liegt. Vorgabe: das OEFFENTLICHE Firmware-Repository.
///
/// WARUM EIN ZWEITES REPOSITORY (entschieden 25.08.2026): das Projektrepo ist
/// privat, weil die Konstruktionsunterlagen darin liegen. Ein privates Repo
/// antwortet einem Geraet ohne Anmeldung mit 404 - die Uhr koennte also nie
/// ein Update finden. Der Quelltext der Firmware ist dagegen unbedenklich
/// (er verraet die Bauweise nicht) und liegt offen.
///
/// AENDERBAR ZUR LAUFZEIT und im NVS gemerkt - ein Umzug des Hostings darf
/// kein Update erzwingen. Die URL ist ABSICHTLICH NICHT mitsigniert
/// (werkzeuge/signieren.py sagt warum): der Schutz liegt am Hash im
/// signierten Block, nicht am Weg.
constexpr const char *UPDATE_URL_VORGABE =
    "https://raw.githubusercontent.com/howtobanana/star-watch-firmware/main/"
    "updates/manifest.json";

/// DIE ALTE ADRESSE, die auf das PRIVATE Repo zeigte und dort immer 404
/// lieferte. Sie steht auf jedem Geraet im NVS, das vor dem 25.08.2026
/// gelaufen ist - und ein gemerkter Wert gewinnt gegen jede neue Vorgabe.
/// Eine neue Firmware allein haette das also NICHT geheilt: das Geraet haette
/// weiter die tote Adresse gefragt. Am 25.08. am Aufbau gesehen, der
/// Diagnosebericht meldete "Kanal: eigene URL".
constexpr const char *UPDATE_URL_STILLGELEGT =
    "https://raw.githubusercontent.com/howtobanana/star-watch/main/"
    "firmware/updates/manifest.json";

/// Dasselbe fuer den Vorabkanal. Die eigene Uhr steht auf `beta` und zieht
/// Versuche mit, ausgelieferte Uhren bleiben auf `stabil`.
constexpr const char *UPDATE_URL_BETA =
    "https://raw.githubusercontent.com/howtobanana/star-watch-firmware/main/"
    "updates/manifest-beta.json";

/// Wie oft geprueft wird, wenn "automatisch" eingestellt ist.
constexpr uint32_t UPDATE_PRUEFTAKT_MS = 24u * 3600u * 1000u;

/// Stunde, zu der automatisch geprueft und eingespielt wird. Nachts, weil die
/// Uhr waehrend des Updates STEHT und danach eine Referenzfahrt braucht.
constexpr int UPDATE_STUNDE = 3;

/// So lange nach dem Start wird ein neues Image NICHT bestaetigt, sondern
/// erst geprueft. Laeuft es bis dahin nicht sauber, rollt der Bootloader
/// zurueck (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y).
constexpr uint32_t UPDATE_BEWAEHRUNG_MS = 120000;

// ===========================================================================
// SELBSTHEILUNG DER REFERENZFAHRT (24.08.2026)
// ===========================================================================
// Zwei Luecken, die beide erst auffallen, wenn die Uhr NICHT mehr auf dem
// eigenen Schreibtisch steht:
//
//   1 Schlug die Referenzfahrt beim Start fehl, geschah nie wieder etwas.
//     Die Uhr stand fuer immer und sagte es nur auf der Konsole, die niemand
//     liest. Der wahrscheinlichste Ausloeser ist banal: ein beim Transport
//     verrutschter Magnet. Beim Beschenkten heisst das ein Anruf.
//
//   2 Eine TAEGLICHE Referenzfahrt behauptete hall.cpp im Kommentar seit
//     jeher ("laeuft beim Start und einmal taeglich"), gebaut war sie nie.
//     Ohne sie summiert sich jeder verlorene Schritt dauerhaft auf.

/// Abstand zwischen zwei Versuchen, solange die Referenzfahrt fehlschlaegt.
constexpr uint32_t HOMING_WIEDERHOLUNG_MS = 5u * 60u * 1000u;

/// Nach so vielen Fehlschlaegen wird der Abstand groesser. Ist der Magnet
/// wirklich abgefallen, soll der Motor nicht bis in alle Ewigkeit alle fuenf
/// Minuten eine Umdrehung suchen.
constexpr uint32_t HOMING_VERSUCHE_KURZ = 3;
constexpr uint32_t HOMING_WIEDERHOLUNG_LANG_MS = 30u * 60u * 1000u;

/// Stunde der taeglichen Referenzfahrt. Nach der Update-Stunde, damit sich
/// beide nicht in die Quere kommen, und nachts, weil der Zeiger dabei
/// weglaeuft.
constexpr int HOMING_STUNDE = 4;

// ===========================================================================
// WERKSRESET DURCH STROMZYKLEN (24.08.2026)
// ===========================================================================
// DER WEG ZURUECK, WENN NICHTS MEHR GEHT. Ohne ihn gibt es einen Kreis, aus
// dem niemand herauskommt: ohne Token keine Bedienung, und der Werksreset -
// der helfen wuerde - verlangt selbst das Token. Ein Gehaeuse ohne Taster
// bietet sonst keinen Ausweg.
//
// STROM IST DER TASTER. Wer die Uhr aus- und einstecken kann, steht davor -
// und genau das ist die richtige Berechtigungsstufe fuer einen Werksreset.
// Aus der Ferne laesst sich das nicht ausloesen.
//
// NUR ECHTES EINSCHALTEN ZAEHLT (ESP_RST_POWERON). Ein Neustart per Software,
// ein Update oder ein Watchdog duerfen den Zaehler NICHT hochtreiben - sonst
// loescht eine Reihe von OTA-Versuchen die Uhr.

/// So oft aus- und wieder einschalten.
constexpr uint32_t RESET_ZYKLEN = 5;

/// Bleibt die Uhr so lange an, war es kein Zaehlversuch - der Zaehler faellt
/// zurueck. Kurz genug, dass fuenf Zyklen zuegig gehen; lang genug, dass ein
/// normaler Start ihn immer loescht.
constexpr uint32_t RESET_FENSTER_MS = 10000;

/// Schemaversion der Exportdatei. Wird sie erhoeht, lehnt eine aeltere
/// Firmware die neue Datei ab statt sie halb zu verstehen - genau darum gibt
/// es die Zahl.
constexpr int KONFIG_SCHEMA = 1;

/// Laengstes Kommando, das die Konsole und die Weboberflaeche annehmen.
///
/// GEFUNDEN AM 24.08.2026, und zwar an einem echten Schaden: die drei
/// Eingabepuffer standen auf 64 bzw. 80 Zeichen und schnitten laengere
/// Kommandos STILLSCHWEIGEND ab - ausgefuehrt wurde dann der Stummel. Beim
/// Setzen der Manifest-URL kam so
///     yu https://raw.githubusercontent.com/.../firmware/updat
/// heraus statt der vollen Adresse, ohne jede Fehlermeldung.
///
/// Betroffen sind genau die Kommandos mit langen Argumenten: `yu <url>` und
/// `ns <ssid> <pass>`. 240 traegt beide bequem; laengeres wird jetzt
/// ABGEWIESEN statt gekuerzt - ein Kommando, das etwas anderes tut als
/// verlangt, ist schlimmer als eines, das gar nichts tut.
constexpr size_t KOMMANDO_MAX = 240;

// ===========================================================================
// ENTWICKLUNGSGERAET ODER AUSLIEFERUNG
// ===========================================================================
// Gesetzt ueber -DSTARWATCH_ENTWICKLUNG in platformio.ini; dort steht auch die
// Begruendung. Kurz: am eigenen Aufbau wird ueber VPN gearbeitet, ohne
// serielle Konsole. Ein Token, das NUR dort steht, sperrt den Entwickler aus -
// und eine Sicherung, die den letzten Weg zum Geraet zumauert, ist keine
// Sicherung, sondern ein Ausfall.
//
// Der Unterschied betrifft AUSSCHLIESSLICH, wohin Geheimnisse gemeldet werden.
// Geprueft wird in beiden Faellen gleich; der Zugriffsschutz ist NICHT
// abgeschaltet.
#ifdef STARWATCH_ENTWICKLUNG
constexpr bool ENTWICKLUNGSGERAET = true;
#else
constexpr bool ENTWICKLUNGSGERAET = false;
#endif

// ===========================================================================
// AUSLIEFERUNGSZUSTAND und RUECKFALL
// ===========================================================================

/// Helligkeit eines frischen Geraets und nach dem Werksreset.
///
/// WERKSVORGABE, NICHT ZWANG BEI JEDEM START. Ein gespeicherter Wert gewinnt -
/// sonst waere die Zusage "alle Einstellungen ueberleben den Neustart"
/// gebrochen, und zwar genau bei der Groesse, die man am haeufigsten anfasst.
constexpr float LICHT_VORGABE_PROZENT = 50.0f;

/// Zeitmodus eines frischen Geraets: 2 = Minutensprung. Ebenfalls nur Vorgabe.
constexpr uint8_t MODUS_VORGABE = 2;

/// So lange nach der letzten Handlung faellt die Uhr von selbst in den
/// Uhrbetrieb zurueck.
///
/// WARUM ES DAS GIBT: die Zeit ist der Normalzustand. Alles andere - ein
/// angefahrenes Ziel, ein abgelaufener Timer - ist eine Episode. Ohne
/// Rueckfall bliebe der Zeiger dort stehen, wo die letzte Aktion ihn liess,
/// und die Uhr waere stumm.
///
/// WARUM ES EINE SPERRE BRAUCHT: beim Einmessen wird von Hand gestellt und
/// dann 'o!' gedrueckt. Griffe der Uhrbetrieb zehn Sekunden nach dem
/// Nachstellen zu, waere der Offset falsch - und das Einmessen unmoeglich.
/// Genau dieser Fehler ist am 23.08.2026 schon einmal aufgetreten, damals
/// wegen der Fuehrungsregel. `v` und `o!` sperren den Rueckfall deshalb, bis
/// ein 'm' oder ein Anfahren ihn wieder freigibt.
constexpr uint32_t RUECKFALL_MS = 10000;

// ===========================================================================
// ZEIT AUS DEM NETZ (AP 3.2)
// ===========================================================================

/// Zeitzone als POSIX-Zeichenkette. Wien, Sommerzeit inbegriffen.
///
/// WARUM POSIX UND NICHT "Europe/Vienna": der ESP32 hat keine
/// Zeitzonendatenbank. Die Regel muss vollstaendig in der Zeichenkette stehen:
///   CET-1        Normalzeit, 1 h OSTWAERTS von UTC (POSIX zaehlt umgekehrt)
///   CEST         Name der Sommerzeit
///   M3.5.0       Beginn: Maerz, 5. (= letzte) Woche, Sonntag
///   M10.5.0/3    Ende:   Oktober, letzte Woche, Sonntag, 3 Uhr
/// Damit rechnet die Uhr die Umstellung selbst - ohne Netz, ohne Tabelle.
constexpr const char *ZEITZONE_VORGABE = "CET-1CEST,M3.5.0,M10.5.0/3";

/// Zeitserver. Zwei verschiedene Betreiber, nicht zwei Namen desselben.
constexpr const char *NTP_SERVER_1 = "pool.ntp.org";
constexpr const char *NTP_SERVER_2 = "time.cloudflare.com";

/// Takt, in dem die Uhr ihre Zeitbasis aus der Systemzeit nachzieht.
///
/// WARUM UEBERHAUPT NACHZIEHEN: die Zeitbasis in `Betrieb` rechnet mit
/// `millis()`, und dessen Quarz laeuft um einige ppm daneben. SNTP korrigiert
/// die SYSTEMzeit von selbst, aber nicht die Basis der Uhr - ohne dieses
/// Nachziehen wandert die Anzeige langsam von der Systemzeit weg.
constexpr uint32_t NTP_NACHSTELLEN_MS = 60000;

/// Takt, solange noch NIE eine Zeit da war.
///
/// WARUM ZWEI TAKTE: seit dem 24.08.2026 ist entschieden, dass **kein
/// Zeitchip** verbaut wird (AP 1.2). NTP ist damit die EINZIGE Zeitquelle -
/// nach jedem Stromausfall haengt die Anzeige daran, wie schnell sie kommt.
/// Mit den 60 s des Nachstelltakts haette der erste Versuch bis zu eine Minute
/// gebraucht, in der die Uhr steht. Zwei Sekunden sind der Unterschied
/// zwischen "kurz dunkel" und "sie geht nicht".
constexpr uint32_t NTP_ERSTVERSUCH_MS = 2000;

// ===========================================================================
// Hall-Sensor KY-024 mit SS49E.
// ===========================================================================

/// Bei erkanntem Magnet liegt D0 HIGH, nicht LOW.
///
/// GEMESSEN AM GERAET am 21.08.2026, gegen die Theorie: im Pruefstand steht
/// deshalb `inverted: false`; mit `true` meldete die Entity bei erkanntem
/// Magnet OFF und im freien Feld ON. Massgeblich ist das Bauteil.
///
/// NUR NOCH FUER DIE ANZEIGE. Die Grobsuche der Referenzfahrt laeuft seit dem
/// 22.08.2026 ueber A0, siehe D0_VORHANDEN.
constexpr bool D0_AKTIV_HIGH = true;

// ### D0 IST OPTIONAL ######################################################
// Die Grobsuche der Referenzfahrt suchte bis zum 22.08.2026 die Flanke des
// Komparators auf D0. Sie sucht jetzt ueber A0 - die Schwelle steht in
// Software (HALL_GROB_SCHWELLE_ANTEIL).
//
// DREI GRUENDE, alle am Aufbau gelernt:
//
// 1. **Das Poti bringt nichts.** Es stellt nur die Komparatorschwelle ein,
//    A0 geht daran vorbei. Die Genauigkeit kommt aus dem Schwerpunkt ueber
//    A0, also war D0 nie an der Genauigkeit beteiligt - nur an der Frage, WO
//    die Grobsuche stehenbleibt.
// 2. **D0 flatterte.** Der LM393 hat keine Hysterese; im Stillstand zaehlte
//    die Firmware 16071 Flanken. Entprellt ist das beherrschbar, aber die
//    Grobsuche endet an der ERSTEN Flanke - und die kann eine Zufallsflanke
//    sein.
// 3. **Die kleinen Sensoren haben kein D0.** Ein nackter SS49E, der SS495A
//    und der DRV5055 sind reine Analogsensoren mit drei Beinen. Wer auf so
//    einen wechselt - und genau das ist der Plan, weil die KY-024 zu viel
//    Platz braucht - haette ohne diese Umstellung keine Grobsuche mehr.
//
// Auf false setzen, sobald das Modul weg ist: dann wird GPIO34 nicht mehr
// gelesen, und die Anzeige verschweigt D0 statt einen offenen Pin zu melden.
constexpr bool D0_VORHANDEN = true;

// ##########################################################################

/// Ruhespannung des SS49E. Nennwert 1,65 V (halbe Versorgung), am Aufbau
/// 1,658 V gemessen. Die Firmware MISST sie bei jeder Referenzfahrt und
/// nimmt sie nie an - dieser Wert ist nur der Startwert.
constexpr float HALL_RUHE_V = 1.65f;

/// Zulaessige Aussteuerung der Feldkurve gegen die Ruhelinie.
/// Weniger heisst zu grosser Magnetabstand, mehr heisst abgeschnittene Kurve.
/// Beides macht den Index unsicher, beides ist Mechanik und keine Software.
///
/// Die Obergrenze stand auf 0,550 - das war der Wert des Pruefstands mit dem
/// SS49E an 3,3 V und einem Oe3x2-Magneten. Empfindlichere Sensoren (SS495A
/// 3,125 mV/G, DRV5055A3 2,5 mV/G) liefern mit demselben Magneten mehr, ohne
/// dass etwas abgeschnitten wird: der SS495A saettigt bei 3,3 V erst bei rund
/// 703 G, der DRV5055A3 bei 850 G. 0,900 laesst diesen Sensoren Raum und
/// warnt weiterhin, bevor die Kurve wirklich klippt.
///
/// WIRD DER SENSOR GEWECHSELT, gehoert diese Zahl geprueft: sie ist keine
/// Vorliebe, sondern die Saettigungsgrenze des verbauten Bauteils.
constexpr float HALL_SPANNE_MIN_V = 0.250f;
constexpr float HALL_SPANNE_MAX_V = 0.900f;

/// Schwelle der Grobsuche, als Anteil der zulaessigen Mindestaussteuerung.
///
/// Bewusst NICHT als Anteil der gemessenen Gipfelhoehe: die Grobsuche laeuft,
/// BEVOR der Gipfel bekannt ist. Bezug ist HALL_SPANNE_MIN_V - die Schwelle
/// liegt damit deutlich unter jeder Aussteuerung, die das Projekt zulaesst,
/// und weit ueber dem gemessenen Rauschen von 8 mV.
///
/// Ergibt 87 mV bei der Untergrenze von 250 mV.
constexpr float HALL_GROB_SCHWELLE_ANTEIL = 0.35f;
constexpr float HALL_GROB_SCHWELLE_V = HALL_SPANNE_MIN_V * HALL_GROB_SCHWELLE_ANTEIL;

/// Rauschgrenze des Schwerpunktverfahrens, als Anteil der Gipfelhoehe.
/// Messpunkte darunter gehen nicht ein, und die Schwelle wird von den
/// Gewichten abgezogen, damit der Rand nicht mitzieht.
constexpr float HALL_SCHWELLE_ANTEIL = 0.20f;

// ### GEMESSEN AM 22.08.2026 (AP 0.1) ######################################
// Rueckzugsweg der Referenzfahrt: nach der Grobsuche faehrt der Zeiger um
// diesen Weg zurueck, damit die langsame Fahrt das Sensorfenster GANZ
// ueberstreicht. Er MUSS groesser sein als die Fensterbreite.
//
// GEMESSENE FENSTERBREITE: 429 Schritte (Magnet 2 mm vor dem Sensor).
// Hier stand 160 - ein geratener Wert, und er war um den Faktor 2,7 zu klein.
//
// DIE FOLGE WAR NICHT HARMLOS: mit 160 landete der Rueckzug MITTEN im
// Sensorfenster. Die Ruhelinie wird direkt danach gemessen - also im Randfeld
// des Magneten statt im freien Feld. Sie kam mit 1,564 V heraus statt 1,650,
// und weil die Auslenkung gegen diese abgesenkte Linie gerechnet wird, sah
// die Aussteuerung mit 233 mV zu klein aus. Gegen die echte Ruhelinie sind es
// 287 mV und damit im Fenster. Ein zu kurzer Rueckzug laesst also den
// MAGNETEN schwach erscheinen - und man kauft einen groesseren, der nichts
// aendert.
//
// NEU GEMESSEN AM 23.08.2026: nach dem Verringern des Magnetabstands meldete
// die Referenzfahrt in fuenf Laeufen 520 bis 567 Schritte - also MEHR als die
// 500, die hier standen. Damit lief die Falle von oben erneut an: in den vier
// Laeufen mit Fenster 562..567 kam die Aussteuerung mit 123..131 mV heraus,
// und die Firmware verlangte einen kleineren Magnetabstand, obwohl nur der
// Rueckzug zu kurz war.
//
// 650 ist die groesste gemessene Breite (567) plus Reserve. Wird der
// Magnetabstand geaendert, aendert sich die Breite - dann neu messen; die
// Referenzfahrt meldet sie von selbst.
//
// MITZIEHEN: FEIN_NACH_FLANKE und FEIN_MAX_PUNKTE in hall.cpp haengen an
// diesem Wert. Der Puffer MUSS mitwachsen, sonst verwirft die Feinanfahrt das
// Ende des Wegs stillschweigend.
constexpr int32_t HALL_RUECKZUG_SCHRITTE = 650;
// ##########################################################################

// ===========================================================================
// Licht. LD24AJTA (PT4115): der Strom steht am Trimmer, die Helligkeit
// kommt aus dem Tastverhaeltnis.
// ===========================================================================

constexpr uint32_t LED_PWM_HZ = 20000;  // 20 kHz, ausserhalb des Hoerbaren

/// Untergrenze des Helligkeitsreglers in Prozent.
/// ### GERATEN, MISST AP 0.5 ###
/// AP 0.5 stellt fest, ab welchem Prozentwert die LED erstmals glimmt. Bis
/// dahin ist 1 nur ein Platzhalter und keine gemessene Schwelle.
constexpr float LED_MIN_PROZENT = 1.0f;

// ===========================================================================
// Was hier NICHT stimmen kann, faellt beim Uebersetzen auf.
// ===========================================================================

// Der ESP32-Zweig und der Host-Zweig muessen dieselbe Sprache sprechen, sonst
// beweist ein gruener Host-Test nichts ueber das Geraet. Erzwungen wird der
// Standard NICHT in platformio.ini - ein Framework auf ein bestimmtes -std zu
// zwingen faengt bei jeder neuen Core-Version von vorn an. Geprueft wird er
// hier, und diese Datei liest jede Umgebung.
//
// C++20 ist die Untergrenze, weil sich unterhalb davon die Semantik
// unterscheidet: Arduino-Core 2.0.x uebersetzte mit gnu++11 und schwieg zu
// `volatile x++`, Core 3.3.11 uebersetzt mit gnu++2b und warnt zu Recht.
// Genau solche Unterschiede sollen nicht zwischen den beiden Zweigen liegen.
static_assert(__cplusplus >= 202002L,
              "Dieses Projekt braucht mindestens C++20. Der ESP32-Zweig "
              "bekommt seinen Standard vom Arduino-Core, der native-Zweig "
              "aus platformio.ini - hier ist einer der beiden zurueckgefallen.");

static_assert(MIKROSCHRITTE == 16 || MIKROSCHRITTE == 32 ||
                  MIKROSCHRITTE == 64 || MIKROSCHRITTE == 128 ||
                  MIKROSCHRITTE == 256,
              "MIKROSCHRITTE: der TMC2209 kennt nur 16/32/64/128/256 "
              "(1/8 und darunter braucht MRES-Werte, die dieses Projekt "
              "nicht benutzt).");

static_assert(REV == VOLLSCHRITTE * MIKROSCHRITTE,
              "REV muss aus VOLLSCHRITTE und MIKROSCHRITTE gerechnet sein, "
              "nie getippt.");

static_assert(SEK_JE_SCHRITT > 0.0f && GRAD_JE_SCHRITT > 0.0f,
              "Die abgeleiteten Schrittgroessen muessen positiv sein.");

static_assert(STROM_HALT > 0,
              "Haltestrom 0 heisst Freilauf, und dann kippt die Klinge aus "
              "ihrer Lage. Der Treiber bleibt bestromt.");

static_assert(STROM_HALT <= STROM_LAUF,
              "Haltestrom groesser als Laufstrom ist keine sinnvolle "
              "Einstellung.");

static_assert(HALL_SPANNE_MIN_V < HALL_SPANNE_MAX_V,
              "Das Aussteuerungsfenster ist verdreht.");

static_assert(HALL_GROB_SCHWELLE_V > 0.020f && HALL_GROB_SCHWELLE_V < HALL_SPANNE_MIN_V,
              "Die Schwelle der Grobsuche muss ueber dem Rauschen (gemessen "
              "8 mV) und unter der kleinsten zulaessigen Aussteuerung liegen - "
              "sonst findet sie entweder Rauschen oder gar nichts.");

static_assert(HALL_SCHWELLE_ANTEIL > 0.0f && HALL_SCHWELLE_ANTEIL < 1.0f,
              "Die Rauschschwelle ist ein Anteil der Gipfelhoehe, also "
              "zwischen 0 und 1.");

static_assert(TMC_ADDR == 0x00 || TMC_ADDR == 0x01 || TMC_ADDR == 0x02 ||
                  TMC_ADDR == 0x03,
              "Die UART-Adresse kommt aus MS1/MS2 und kann nur 0..3 sein.");

static_assert(PIN_HALL_A0 >= 32 && PIN_HALL_A0 <= 39,
              "Der analoge Hall-Eingang MUSS auf ADC1 liegen (GPIO32..39). "
              "ADC2 ist bei aktivem WLAN unbrauchbar.");

// GPIO34..39 des ESP32 sind EINGANGSSEITIG. Sie haben keine Ausgangstreiber
// und keine internen Pull-ups. Ein Ausgangssignal dort ist ein Fehler, der
// sich am Aufbau als "Pin tut nichts" zeigt und lange gesucht wird.
constexpr bool nur_eingang(uint8_t pin) { return pin >= 34 && pin <= 39; }

static_assert(!nur_eingang(PIN_STEP) && !nur_eingang(PIN_DIR) &&
                  !nur_eingang(PIN_EN) && !nur_eingang(PIN_LED_PWM) &&
                  !nur_eingang(PIN_UART_TX),
              "Ein Ausgang liegt auf GPIO34..39. Diese Pins koennen nicht "
              "treiben - siehe die Pinliste in Arbeitsstand Abschnitt 1.");

static_assert(PIN_UART_TX != PIN_UART_RX && PIN_STEP != PIN_DIR &&
                  PIN_STEP != PIN_EN && PIN_DIR != PIN_EN &&
                  PIN_HALL_D0 != PIN_HALL_A0 && PIN_LED_PWM != PIN_STEP &&
                  PIN_LED_PWM != PIN_DIR && PIN_LED_PWM != PIN_EN,
              "Zwei Signale liegen auf demselben Pin.");

}  // namespace geraet
}  // namespace starwatch
