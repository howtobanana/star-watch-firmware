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

#include <cstddef>
#include <cstdint>

namespace starwatch {
namespace geraet {

// ===========================================================================
// DIE PLATINE: ESP32-C3-WROOM-02 (ein Kern, RISC-V), DREI LED-Stufen (RGB)
// hinter je einem AO3400A - DIM ist damit aktiv LOW. Bestellt am 18.09.2026,
// JLCPCB W2026091808365529 (Projektordner: HauptplatineF_RGB).
//
// ES GIBT NUR DIESE EINE (28.09.2026). Die Platinen davor - B (ESP32 D1 Mini,
// eine LED-Stufe) und E - waren Alphas; ihr Code ist entfernt. Der letzte
// Stand, der sie noch baut, liegt im Git-Tag `alpha-b`.
// ===========================================================================

// ===========================================================================
// Pins. HERKUNFT: ausgelesen am 25.09.2026 aus
// 03_Elektronik/Platine/HauptplatineF_RGB/HauptplatineF_RGB.kicad_pcb
// (Pad -> Netz) und dem Symbol RF_Module:ESP32-C3-WROOM-02, so wie es im
// Schaltplan eingebettet ist UND wie es in der KiCad-Bibliothek steht
// (Pad -> GPIO, beide gleich). Nicht aus der Arbeitsliste abgeschrieben -
// sie stimmt aber mit TODO_Firmware-Anpassung_E.md 2.1 und 13.1 ueberein.
// ===========================================================================

// UART ist EIN Draht: TX ueber R6 1 kOhm, RX direkt, beide auf U2.14 PDN.
constexpr uint8_t PIN_UART_TX = 21;  // U1 Pad 12 = IO21/TXD -> TX_VOR_R -> R6
constexpr uint8_t PIN_UART_RX = 20;  // U1 Pad 11 = IO20/RXD -> UART -> U2.14

constexpr uint8_t PIN_STEP = 0;  // U1 Pad 18 = IO0 -> STEP_ESP -> R8 100 Ohm -> U2.16
constexpr uint8_t PIN_DIR  = 1;  // U1 Pad 17 = IO1 -> DIR -> U2.19
constexpr uint8_t PIN_EN   = 3;  // U1 Pad 15 = IO3 -> EN_TMC -> U2.2, R7 10 k

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
//
// NICHT GEMESSEN. Der Wert ist von der Alpha B uebernommen, weil es keinen
// besseren gibt - J3 traegt `1=MOT_A2 2=MOT_A1 3=MOT_B1 4=MOT_B2`, und
// ob das die Polung des Aufbaus trifft, sagt nur der Zeiger. ERSTE FAHRT AM
// NEUEN BRETT: 'u 3' und hinsehen - dreht er GEGEN den Uhrzeigersinn, hier
// auf true stellen (TODO_Firmware-Anpassung_E.md 9).
constexpr bool DIR_HIGH_ZAEHLT_AUFWAERTS = false;
// ##########################################################################

// U1 Pad 10 = IO10 -> INDEX -> U2.12. Nur Selbstueberwachung.
constexpr uint8_t PIN_INDEX = 10;

// ROT. U1 Pad 5 = IO6 -> LED_PWM -> Gate Q1 (AO3400A), Drain an U4.3 DIM.
// Der Netzname traegt keine Farbe, weil er von der Alpha E stammt, wo es
// nur diese eine Stufe gab. Gruen und Blau stehen unten bei "Licht".
constexpr uint8_t PIN_LED_PWM = 6;

// ===========================================================================
// I2C
// ===========================================================================
//
// Am Bus haengen drei Steckplaetze: J5 (TMAG5273), J7 (SCD41), J8 (VEML7700).
// U1 Pad 3 = IO4 = SDA, Pad 4 = IO5 = SCL, mit Pull-ups auf dem Brett
// (R11/R12 4k7 nach +3V3) - der Bus laeuft also auch ohne gestecktes Modul.
constexpr uint8_t PIN_SDA = 4;   // J5 Pad 4, J7 Pad 3, J8 Pad 3, R11
constexpr uint8_t PIN_SCL = 5;   // J5 Pad 3, J7 Pad 2, J8 Pad 2, R12

/// Takt des Busses. 100 kHz zum Anfang: die Adern zu den Modulen sind
/// Steckleitungen von rund 20 cm, und die Pull-ups liegen parallel auf den
/// Modulen statt gebuendelt auf der Platine. Schneller erst, wenn die
/// Flanken am Oszilloskop nachgemessen sind.
constexpr uint32_t I2C_HZ = 100000;

/// INT des TMAG5273: U1 Pad 6 = IO7 -> HALL_INT -> J5.2, R13 10 kOhm nach
/// +3V3.
///
/// WIRD VORERST NICHT AUSGEWERTET. Der INT des TMAG5273 ist kein
/// "Magnet da"-Signal, wie D0 der KY-024 es war, sondern ein Umwandlungs-
/// bzw. Schwellwert-Interrupt (Datenblatt SLYS045C, INT_CONFIG_1). Die
/// Referenzfahrt braucht ihn nicht. Der Pin steht hier, damit niemand ihn
/// versehentlich anderweitig belegt.
constexpr uint8_t PIN_HALL_INT = 7;

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

/// Sense-Widerstaende: R4 (U2.23 SENSE_A) und R5 (U2.27 SENSE_B),
/// 0R47, C859932, 1 %, 0,5 W. Vollausschlag nach TMCStepper
/// (CS+1)/32 * 0,325 V / (sqrt2 * (R + 0,02)) bei CS 31: 0,469 A RMS.
/// STROM_LAUF 400 mA liegt damit auf CS 26 (STROM_HALT seit 01.10.2026 150 mA) -
/// der Laufstrom im Fenster 16..31, das das Datenblatt S. 25 fuer IRUN
/// empfiehlt; der Haltestrom liegt bei CS 9.
/// Ohne diese Zeile meldet ma_aus_cs() den Strom um Faktor 3,8 zu hoch.
constexpr float R_SENSE = 0.47f;

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
//
// U2 Pad 9 (MS1/AD0) und Pad 10 (MS2/AD1) liegen beide fest auf
// GND (am Brett nachgesehen). 0x00 ist dort verdrahtet, nicht Zufall.
constexpr uint8_t TMC_ADDR = 0x00;
// ##########################################################################

constexpr uint16_t STROM_LAUF = 400;  // mA RMS
/// Untergrenze fuer den einstellbaren Haltestrom ('ih', 01.10.2026).
constexpr uint16_t HALTESTROM_MIN = 50;
/// Haltestrom. NIE 0 - ohne Haltestrom kippt die Klinge aus ihrer Lage.
///
/// GEMESSEN 01.10.2026 an C4E8: Klinge waagrecht (3:20), Strom gesetzt,
/// Referenzfahrt, 75 s halten, Referenzfahrt. Bei 300/200/150/120/100 mA und
/// bei 80/60 (der Treiber liefert dort schon ~100 mA) lag die Abweichung bei
/// +-3 Schritten - das ist die Streuung der Referenzfahrt, kein Verrutschen.
/// 150 mA ist das Anderthalbfache des getesteten Minimums. Vorher 300 mA;
/// die Verluste im Motor wachsen mit dem Quadrat und fallen damit auf rund
/// ein Viertel. Je Geraet einstellbar mit 'ih' (gespeichert).
constexpr uint16_t STROM_HALT = 150;  // mA RMS
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

/// Tempo beim Steuern von aussen ('ps', Neigungssteuerung der App).
///
/// NICHT TEMPO_FAHRT, und das ist der Unterschied zwischen einem Sprung und
/// einem Strom: eine Handfahrt hat EIN Ziel und darf schnell hin, das Steuern
/// bekommt 50 neue Ziele je Sekunde. Mehr Tempo bringt dort nichts - der
/// Zeiger ist ohnehin am naechsten Ziel, bevor er austoeren koennte -, macht
/// aber jeden Richtungswechsel zu einem Schlag auf den Riemen.
///
/// GEMESSEN AM 21.10.2026: nach einem Steuerversuch mit 6 U/s meldete die
/// Referenzfahrt 5 Schritte Abweichung, also KEINEN Schrittverlust (der waere
/// eine ganze Periode von 64). Die Grenze ist hier nicht der Antrieb, sondern
/// das Verhalten: bei 2 U/s laeuft der Zeiger der Hand ruhig nach, statt zu
/// zucken. Wer es haertet, misst wieder mit 'hv'.
constexpr uint32_t TEMPO_STEUERN = 2 * REV;

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
constexpr const char *FIRMWARE_VERSION = "0.28.0";

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
///
/// Liegt dort noch ein Manifest der Alpha B (Schema fw-1), weist die Uhr es
/// am Schema ab - siehe manifest_pruefen(). Solange nichts veroeffentlicht
/// ist, meldet sie schlicht 404 = kein Update.
constexpr const char *UPDATE_URL_VORGABE =
    "https://raw.githubusercontent.com/howtobanana/star-watch-firmware/main/"
    "updates/manifest.json";

/// Der Ordner, in dem beide Manifeste liegen. Zeigt eine GEMERKTE Adresse
/// hierher, aber auf keinen der heutigen Namen, stellt die Uhr sie einmalig
/// um (update_url_umstellen) - die Manifeste hiessen bis 0.18.0 anders.
constexpr const char *UPDATE_ORDNER =
    "https://raw.githubusercontent.com/howtobanana/star-watch-firmware/main/"
    "updates/";

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

/// Die beiden Kanaladressen muessen im Ordner liegen, sonst stellte die
/// Umstellung eine richtige Adresse auf sich selbst um - oder nie.
constexpr bool beginnt_mit(const char *s, const char *anfang) {
  return *anfang == '\0' || (*s == *anfang && beginnt_mit(s + 1, anfang + 1));
}
static_assert(beginnt_mit(UPDATE_URL_VORGABE, UPDATE_ORDNER) &&
                  beginnt_mit(UPDATE_URL_BETA, UPDATE_ORDNER),
              "Kanaladressen muessen in UPDATE_ORDNER liegen.");

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

/// Eine elektrische Periode des Motors in Zaehlschritten: 4 Vollschritte.
/// Verliert der Motor Schritte, dann immer ganze Perioden (05.10.2026, siehe
/// `schrittverlust()` in lib/zeiger).
constexpr int32_t SCHRITTE_JE_PERIODE = 4 * MIKROSCHRITTE;

/// Streuung der Referenzfahrt: gemessen +-3 Schritte (01.10.2026, siehe
/// STROM_HALT). Erst darueber zaehlt eine Abweichung als Verlust.
constexpr int32_t HOMING_STREUUNG = 8;

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
// NUR STROM ZAEHLT (ESP_RST_POWERON und ESP_RST_BROWNOUT). Ein Neustart per
// Software, ein Update oder ein Watchdog duerfen den Zaehler NICHT
// hochtreiben - sonst loescht eine Reihe von OTA-Versuchen die Uhr.
//
// WIE BEI EINER LAMPE (seit 26.09.2026): fuenfmal kurz aus und an, etwa eine
// Sekunde an, eine Sekunde aus. Vorher stand hier "nur POWERON" und gezaehlt
// wurde erst nach dem Hochfahren der Sensoren - das verlangte je Zyklus rund
// drei Sekunden an und fuenf aus, und so macht es niemand. Warum BROWNOUT
// mitzaehlt und warum das keine Schleife ausloest: main.cpp, Anfang setup().

/// So oft aus- und wieder einschalten.
constexpr uint32_t RESET_ZYKLEN = 5;

/// Bleibt die Uhr so lange an, war es kein Zaehlversuch - der Zaehler faellt
/// zurueck. Lang genug fuer das Zaehlblinken (vier Blitze sind 1,4 s), kurz
/// genug, dass ein normales Einstecken nie mitzaehlt.
constexpr uint32_t RESET_FENSTER_MS = 5000;

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

/// GEMEINSAMES ENTWICKLUNGSPASSWORT FUER DAS OTA (28.09.2026). Jede Test-Uhr
/// mit eigenem Passwort hiess: bei jedem Upload nachschlagen und eintippen.
/// Das Passwort je Uhr schuetzt aber nur das Entwickler-OTA, und das gibt es
/// ausschliesslich in der Entwicklungsfassung - die es obendrein offen ins
/// Netz-Log schreibt. In der Auslieferung ist es abgeschaltet.
///
/// Deshalb setzt platformio.ini beim Bauen der Entwicklungsfassung den Inhalt
/// von STARWATCH_OTA_PW ein (bei Fabian aus dem macOS-Schluesselbund, ueber
/// ~/.zshenv) - dieselbe Variable, mit der der Upload sich anmeldet. Jede so
/// gebaute Uhr nimmt damit dasselbe Passwort. Ist die Variable beim Bauen
/// leer, gilt wie bisher das Passwort der einzelnen Uhr.
#if defined(STARWATCH_ENTWICKLUNG) && defined(STARWATCH_OTA_PW_ENTW)
constexpr const char *OTA_PASSWORT_ENTWICKLUNG = STARWATCH_OTA_PW_ENTW;
#else
constexpr const char *OTA_PASSWORT_ENTWICKLUNG = "";
#endif
constexpr bool OTA_PASSWORT_GEMEINSAM = OTA_PASSWORT_ENTWICKLUNG[0] != '\0';

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

/// Nach einem Timer geht es SCHNELLER zurueck auf die Uhrzeit: der Zeiger
/// steht auf zwoelf und behauptet damit eine Zeit, die nicht stimmt. Zehn
/// Sekunden sind dafuer zu lang - drei reichen, und die drei Blitze des
/// Alarms passen mit 0,75 s bequem hinein.
constexpr uint32_t RUECKFALL_TIMER_MS = 3000;

/// So lange blinkt der Zeiger vor dem Start herunter: drei Blitze, drei
/// Sekunden. Der Zeiger steht dabei schon auf zwoelf.
constexpr uint32_t TIMER_COUNTDOWN_S = 3;

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

/// Vorgabeport des MQTT-Brokers. 1883 ist der unverschluesselte Standardport;
/// Mosquitto als Home-Assistant-Zusatz hoert dort. TLS (8883) ist bewusst
/// NICHT vorgesehen: der Verkehr bleibt im Heimnetz, und ein Zertifikatspeicher
/// auf dem Geraet waere Pflege ohne Gegenwert.
constexpr uint16_t MQTT_PORT_VORGABE = 1883;

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
// Hall-Sensor TMAG5273A2 ueber I2C (seit 27.08.2026)
// ===========================================================================
//
// Vorher: KY-024 mit SS49E, D0 als Komparator und A0 als analoge Feldkurve.
// Jetzt: ein 3D-Hall-Sensor, der Bx, By und Bz selbst wandelt und ueber den
// Bus schickt. Das aendert, WOHER die Feldzahl kommt - nicht, wie die
// Referenzfahrt rechnet. Sie holt ihre Genauigkeit weiter aus dem
// Schwerpunkt ueber die Feldkurve (lib/zeiger, host-getestet).
//
// DREI DINGE WERDEN DADURCH BESSER:
//
// 1. Die Ruhelinie ist ECHT NULL. Der SS49E ist ratiometrisch, seine
//    Nulllinie lag bei der halben Versorgung und musste bei jeder Fahrt neu
//    gemessen werden. Der TMAG5273 liefert vorzeichenbehaftete Millitesla.
// 2. Die Polung des Magneten ist endgueltig gleichgueltig. Gerechnet wird
//    mit dem BETRAG des Vektors sqrt(Bx^2+By^2+Bz^2); der ist immer positiv
//    und haengt nicht daran, welche Polflaeche am Sensor vorbeizieht.
// 3. Kein ADC-Rauschen mehr. Der SS49E brauchte Median aus drei Mittelwerten
//    zu je 16 Proben, weil der ESP32-ADC einzelne Ausreisser liefert. Der
//    TMAG5273 mittelt in Hardware (CONV_AVG), eine Messung genuegt.
//
// ADRESSE: 0x35. Datenblatt SLYS045C Tabelle 8-15 - I2C_ADDRESS, Reset 35h
// fuer die Varianten A1 und A2. Verbaut ist laut Verkabelungsplan Z. 227 die
// Variante A2. Die Firmware sucht den Bus trotzdem ab, statt die Adresse zu
// glauben.
constexpr uint8_t TMAG_ADRESSE = 0x35;

/// Messbereich der X/Y/Z-Achsen in Millitesla.
///
/// A2 mit X_Y_RANGE = 0 und Z_RANGE = 0: +/-133 mT (SLYS045C Tabelle 8-6).
/// Der andere Bereich waere +/-266 mT und hilft hier nicht - er halbiert nur
/// die Aufloesung. Bei 16 Bit auf 2*133 mT sind das 4,06 uT je Schritt,
/// also rund das Achtzigfache dessen, was das Erdfeld ausmacht.
///
/// WIRD EINE A1 VERBAUT, gehoert hier 40 hin - sonst rechnet die Firmware
/// jede Feldstaerke um den Faktor 3,3 zu hoch. Der Sensor sagt selbst, was
/// er ist: DEVICE_ID Bit 1:0, 1h = +/-40/80 mT, 2h = +/-133/266 mT
/// (Tabelle 8-16). Die Firmware liest das beim Start und meldet einen
/// Widerspruch, statt still falsch zu rechnen.
constexpr float TMAG_BEREICH_MT = 133.0f;

/// Mittelung im Sensor. 5h = 32x, 0,4 kSPS ueber drei Achsen (Tabelle 8-3).
///
/// WARUM DIE LANGSAMSTE STUFE: die Feinanfahrt misst alle 20 ms, das sind
/// 50 Messungen je Sekunde - bei 400 Wandlungen je Sekunde stehen also acht
/// fertige Mittelwerte je Messung bereit. Die Mittelung kostet hier nichts
/// und ersetzt genau die Software-Mittelung, die der ADC vorher brauchte.
constexpr uint8_t TMAG_CONV_AVG = 5;

// ### DIE VIER SCHWELLEN SIND NOCH NICHT GEMESSEN ##########################
//
// Sie standen bis zur Alpha B in VOLT und gehoerten zum SS49E: Ruhelinie
// 1,65 V, Aussteuerung 0,250 bis 0,900 V. Diese Zahlen sind NICHT
// umgerechnet worden, und das ist Absicht: die Empfindlichkeit des SS49E war
// nie genau bekannt (Datenblattwert 1,4 mV/G bei 5 V, ratiometrisch, mit
// grosser Streuung), der Magnetabstand wurde am 23.08.2026 veraendert, und
// aus zwei unsicheren Zahlen wird durch Teilen keine sichere.
//
// SO WERDEN SIE GEMESSEN: eine Referenzfahrt laufen lassen ('h' im
// Messmenue). Sie meldet die tatsaechliche Aussteuerung als `spanne_mt` -
// das ist die Gipfelhoehe des Magneten in Millitesla. Danach:
//   HALL_SPANNE_MIN_MT  ~ halbe gemessene Gipfelhoehe
//   HALL_SPANNE_MAX_MT  ~ doppelte gemessene Gipfelhoehe, aber unter 133
// Bis dahin sind die Werte unten bewusst WEIT gestellt: sie sollen eine
// erste Fahrt nicht blockieren, aber auch nichts bestaetigen.
constexpr float HALL_RUHE_MT = 0.0f;
constexpr float HALL_SPANNE_MIN_MT = 3.0f;
constexpr float HALL_SPANNE_MAX_MT = 100.0f;
// ##########################################################################

/// Schwelle der Grobsuche, als Anteil der zulaessigen Mindestaussteuerung.
///
/// Bewusst NICHT als Anteil der gemessenen Gipfelhoehe: die Grobsuche laeuft,
/// BEVOR der Gipfel bekannt ist. Bezug ist HALL_SPANNE_MIN_MT - die Schwelle
/// liegt damit deutlich unter jeder Aussteuerung, die das Projekt zulaesst.
///
/// Ergibt 1,05 mT bei der Untergrenze von 3 mT. Das ist rund das
/// Zwanzigfache des Erdfelds (etwa 0,05 mT) und liegt ueber dem
/// Nullpunktfehler, den der Sensor im Datenblatt zugesteht - eine Grobsuche,
/// die auf das Erdfeld anspringt, bliebe an einer beliebigen Stelle stehen.
constexpr float HALL_GROB_SCHWELLE_ANTEIL = 0.35f;
constexpr float HALL_GROB_SCHWELLE_MT = HALL_SPANNE_MIN_MT * HALL_GROB_SCHWELLE_ANTEIL;

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
// SEIT 27.09.2026 GILT DIE REGEL "GROESSER ALS DIE FENSTERBREITE" NICHT
// MEHR. Die Feinfahrt beginnt nicht mehr am Ende des Rueckzugs, sondern
// FEIN_VORLAUF vor der Flanke, und endet an der Kurve statt nach fester
// Strecke (hall.cpp). Der Rueckzug hat nur noch EINE Aufgabe: die Ruhelinie
// im freien Feld messen. Dafuer muss er aus dem Randfeld VOR der Flanke
// heraus, nicht ueber das ganze Fenster. Die Flanke liegt bei 1,05 mT ueber
// dem Grundfeld; an C4E8 (Gauss, sigma ~ 199 Schritte, 9,85 mT Gipfel) ist
// das Feld 650 Schritte davor unter 0,001 mT.
constexpr int32_t HALL_RUECKZUG_SCHRITTE = 650;
// ##########################################################################

// ===========================================================================
// Licht. LD24AJTA (PT4115): der Strom steht am Trimmer, die Helligkeit
// kommt aus dem Tastverhaeltnis.
// ===========================================================================

// ---------------------------------------------------------------------------
// DREI Stufen, je ein PT4115 (U4 rot, U5 gruen, U6 blau) mit
// 0,68 Ohm = 294 mA, davor je ein AO3400A (Q1..Q3) als Umkehrstufe.
//
// HIGH = AUS, LOW = AN. Das ist keine Feinheit, sondern der Normalfall: der
// Transistor zieht DIM nach Masse, sobald sein Gate hoch liegt. Wer den Wert
// direkt durchreicht, bekommt bei 0 volle Helligkeit. Die Umkehrung
// geschieht an GENAU EINER Stelle - in lib/licht beim Schreiben des
// Tastverhaeltnisses. Nirgends sonst.
//
// DIE RUHELAGE STIMMT VON SELBST: R10/R15/R14 ziehen die drei Gates mit
// 10 kOhm auf +3V3, solange der ESP hochohmig ist - alle drei Farben sind
// beim Hochlauf aus. Deshalb vor ledcAttach() NIE pinMode(OUTPUT) mit LOW.
// ---------------------------------------------------------------------------

/// GRUEN. U1 Pad 16 = IO2 -> LED_G_PWM -> Gate Q2, R15 10 k nach +3V3.
/// IO2 IST EIN STRAPPING-PIN (C3-Datenblatt Tab. 4-3): beim Reset muss er
/// HIGH liegen, das leistet R15. Erst nach dem Start auf Ausgang schalten.
constexpr uint8_t PIN_LED_G_PWM = 2;
/// BLAU. U1 Pad 7 = IO8 -> LED_B_PWM -> Gate Q3, R14 10 k nach +3V3.
/// Ebenfalls Strapping (Download-Modus braucht ihn HIGH).
constexpr uint8_t PIN_LED_B_PWM = 8;

constexpr bool LED_INVERTIERT = true;

/// 2 kHz. Der PT4115E nimmt am DIM-Eingang 100 Hz bis 20 kHz an (CR Powtech,
/// Datenblatt "FDIM_MIN 0,1 kHz / FDIM_MAX 20 kHz"); seine Oszillogramme fuer
/// PWM-Dimmen stehen bei 2 kHz, mit 50 % UND 1 % Tastverhaeltnis. Oben am
/// Rand (20 kHz) waeren 1 % nur 0,5 us - kuerzer als der Regler braucht,
/// um anzulaufen, und die unteren Prozente gingen verloren. Das ist fuer
/// die Farbmischung wichtig: ein Kanal auf 10 % des
/// Reglers bei 20 % Helligkeit sind 2 % Tastverhaeltnis.
/// Pruefbericht F N2 empfiehlt 1..5 kHz; 2 kHz ist die Stelle, fuer die das
/// Datenblatt selbst Messungen zeigt. PFEIFT DIE DROSSEL HOERBAR, ist 4 kHz
/// der naechste Versuch - nicht ueber 5.
constexpr uint32_t LED_PWM_HZ = 2000;

/// LEISTUNGSGRENZE IN WATT, nicht in Prozent. Festgelegt am 25.09.2026 auf
/// 80 %, am 27.10.2026 auf 95 % ihrer 3 W angehoben (Fabian: die Kuehlung
/// ueber die Alu-Scheibe ist sehr gut, das Licht wirkte zu dunkel). Alles, was darunter bleibt, laeuft ungebremst: Rot allein
/// bei 100 % ist unter 1 W und bekommt volle 100 %, ebenso Gruen, Blau und
/// jede Mischung aus ZWEI Farben. Erst Weiss (alle drei) stoesst an die
/// Grenze und wird heruntergeregelt - gleichmaessig auf allen drei Kanaelen,
/// damit der Farbton bleibt.
///
/// DIE ANZEIGE BLEIBT DER REGLERWERT. Oberflaeche und Home Assistant zeigen
/// 100 %, auch wenn die Firmware bei Weiss intern auf rund 98 % geht.
///
/// GERECHNET MIT DEN UNGUENSTIGSTEN WERTEN, damit die Schaetzung nie zu
/// niedrig liegt:
///   Strom   PT4115E I = V_CSN / R, V_CSN hoechstens 206 mV (CR Powtech,
///           "Mean current sense threshold 194/200/206 mV"), R9/R16/R17
///           0,68 Ohm mit 1 % nach unten -> 0,306 A.
///   Spannung JH-1RGB14G30-S2A, VF max bei 350 mA: R 2,4 V, G 3,4 V,
///           B 3,4 V, dazu die Messtoleranz 0,1 V. Bei unseren ~300 mA
///           liegt VF darunter - die Rechnung ist also eher zu hoch.
///   Grenze  Datenblatt "Power Dissipation PD 3000 mW" fuer die ganze LED.
/// Ergibt bei 100 %: Rot 0,77 W, Gruen/Blau je 1,07 W, Gruen+Blau 2,14 W,
/// Weiss 2,91 W -> auf 2,85 W, also 98 % je Kanal.
///
/// GILT AUF JEDEM WEG - Regler, Effekte, Blinkzeichen und pwm_direkt ('w').
constexpr float LED_STROM_MAX_A = 0.206f / (0.68f * 0.99f);
constexpr float LED_UF_MAX_V[3] = {2.4f + 0.1f, 3.4f + 0.1f, 3.4f + 0.1f};
constexpr float LED_WATT_VOLL[3] = {LED_STROM_MAX_A * LED_UF_MAX_V[0],
                                    LED_STROM_MAX_A * LED_UF_MAX_V[1],
                                    LED_STROM_MAX_A * LED_UF_MAX_V[2]};
constexpr float LED_PD_DATENBLATT_W = 3.0f;
constexpr float LED_MAX_ANTEIL_PD = 0.95f;  ///< 95 % der 3 W
constexpr float LED_MAX_W = LED_PD_DATENBLATT_W * LED_MAX_ANTEIL_PD;  // 2,85 W

/// Farbe eines frischen Geraets und nach dem Werksreset: ROT - die Klinge
/// eines Lichtschwerts. Als 0xRRGGBB.
constexpr uint32_t LED_FARBE_VORGABE = 0xFF0000;

/// WEISS EINER FRISCHEN UHR, 255/215/100 - abgestimmt an C4E8 (27.09.2026).
/// Die drei LEDs streuen von Uhr zu Uhr; deshalb hat jede ihr eigenes Weiss
/// im Fertigungsbereich des NVS ('lw', 27.10.2026; BE40: 100/255/150).
constexpr uint32_t LED_WEISS_VORGABE = 0xFFD764;

/// Untergrenze des Helligkeitsreglers in Prozent.
/// ### GERATEN, MISST AP 0.5 ###
/// AP 0.5 stellt fest, ab welchem Prozentwert die LED erstmals glimmt. Bis
/// dahin ist 1 nur ein Platzhalter und keine gemessene Schwelle.
constexpr float LED_MIN_PROZENT = 1.0f;

/// Wie lange die Klinge nach erfolgreicher Referenzfahrt auf die Zeit wartet,
/// bevor sie ohne sie zuendet.
///
/// WARUM UEBERHAUPT WARTEN: steht die Zeit schon, wenn sie angeht, zeigt der
/// Zeiger im selben Augenblick das Richtige. Zuendete sie vorher, stuende sie
/// kurz leuchtend auf zwoelf und spraenge dann - das sieht nach Fehler aus,
/// obwohl alles richtig laeuft. Nach einem frischen WLAN-Start liefert SNTP
/// ueblicherweise in wenigen Sekunden.
///
/// WARUM NICHT UNBEGRENZT: dieses Geraet ist mehr als eine Uhr. Timer, Licht,
/// Home Assistant und die Sensoren arbeiten ohne Zeit weiter - nur der Zeiger
/// nicht. Eine Klinge, die ohne Netz fuer immer dunkel bliebe, machte aus
/// einer fehlenden Zeitanzeige ein totes Geraet. Das war am 24.09.2026 erst
/// anders entschieden und noch am selben Abend berichtigt.
///
/// Ohne erfolgreiche Referenzfahrt zuendet sie weiterhin GAR NICHT - dann
/// kennt der Zeiger seine Lage nicht, und das ist etwas anderes als eine
/// fehlende Zeit.
constexpr uint32_t ZUEND_FRIST_OHNE_ZEIT_MS = 30000;

// ===========================================================================
// Umgebungslicht. VEML7700 an J8 (optional gesteckt)
// ===========================================================================
//
// Der Sensor ist OPTIONAL. Steckt kein Modul an J8, meldet der Bus-Suchlauf
// das einmal und die Uhr laeuft ohne ihn weiter - mit der von Hand
// eingestellten Helligkeit und ohne Nachtabschaltung. Ein fehlender
// Zusatzsensor darf eine Uhr nicht anhalten.
//
// ADRESSE: 0x10, fest. Vishay 84286 Rev. 1.8, Abschnitt "I2C slave address" -
// der VEML7700 hat keinen Adresswahl-Pin.
constexpr uint8_t LUX_ADRESSE = 0x10;

/// Aufloesung in Lux je Zaehlschritt bei Verstaerkung x2 und 800 ms
/// Integrationszeit. Vishay 84286 Tabelle "Refresh time, IDD and resolution
/// relation": 0,0042 lx/bit. Alle anderen Kombinationen rechnen sich daraus -
/// die Aufloesung verdoppelt sich mit jeder Halbierung von Zeit oder
/// Verstaerkung. Probe: x1/8 und 25 ms geben 0,0042 * 32 * 16 = 2,15 lx/bit,
/// mal 65535 Zaehlschritte = 141 000 lx. Das Datenblatt nennt als groesste
/// messbare Beleuchtungsstaerke 140 000 lx - die Rechnung trifft sie.
constexpr float LUX_AUFLOESUNG_BASIS = 0.0042f;

// --- Was die Uhr mit dem Wert macht ---------------------------------------
//
// ### DIE VIER SCHWELLEN SIND GESETZT, NICHT GEMESSEN ######################
// Sie stammen aus den ueblichen Groessenordnungen der Beleuchtungsstaerke
// (Vollmondnacht rund 0,3 lx, Wohnzimmer abends 50 bis 150 lx, helles
// Buero 500 lx) und nicht aus einer Messung im Zimmer, in dem die Uhr
// haengt. Sie sind der Startpunkt zum Einstellen, nicht das Ergebnis.
// Die gemessene Beleuchtungsstaerke steht als eigene Anzeige zur
// Verfuegung; die vier Werte sind zur Laufzeit ueber die Oberflaeche
// verstellbar und werden dort dauerhaft gespeichert.

/// Unterhalb dieser Beleuchtungsstaerke laeuft die Klinge auf dem kleinsten
/// Anteil, oberhalb auf dem vollen eingestellten Wert. Dazwischen wird
/// LOGARITHMISCH ueberblendet, nicht linear: das Auge empfindet
/// Helligkeitsunterschiede als Verhaeltnisse. Linear ueberblendet waere der
/// ganze Weg von 1 auf 50 lx - also der ganze Abend - in den untersten
/// Prozenten der Skala zusammengedraengt.
constexpr float LUX_DUNKEL = 1.0f;
constexpr float LUX_HELL = 20.0f;   ///< Vorgabe 27.10.2026, vorher 300

/// Kleinster Anteil des eingestellten Werts, auf den die Automatik
/// herunterregelt. NICHT 0: die Abschaltung ist Sache des Nachtschalters,
/// und eine Regelung, die bis auf null geht, macht ihn unsichtbar.
constexpr float LUX_ANTEIL_MIN = 0.30f;  ///< Vorgabe 27.10.2026, vorher 0,15

/// Nachtabschaltung, mit Hysterese. Zwei verschiedene Schwellen sind hier
/// keine Feinheit: mit einer einzigen schaltet die Klinge in der Daemmerung
/// im Minutentakt, weil die Beleuchtungsstaerke genau dann langsam durch die
/// Schwelle wandert und dabei rauscht.
constexpr float LUX_NACHT_AUS = 0.2f;  ///< darunter aus; Vorgabe 27.10.2026, vorher 0,5
/// darueber wieder an - wie nacht_an_lux(): Faktor 1,35 (28.09.2026, vorher 6)
constexpr float LUX_NACHT_AN = LUX_NACHT_AUS * 1.35f;

/// Wie lange es dunkel sein muss, bevor die Nachtabschaltung die Klinge
/// einfaehrt. Gegen den kurzen Schatten, der ueber den Sensor faellt. 30 s
/// (28.09.2026, vorher 60 s - nach Vorgabe: Licht aus, und die Klinge soll
/// bald folgen).
constexpr uint32_t LUX_NACHT_VERZUG_MS = 30000;

/// Und beim EINschalten: keiner (28.09.2026). Wird es hell, zuendet die
/// Klinge mit dem naechsten Messwert - Lichtanschalten und eine Minute
/// dunkle Klinge wirkte, als funktioniere die Automatik nicht.
constexpr uint32_t LUX_NACHT_AN_VERZUG_MS = 0;

/// Messtakt. Der Sensor selbst braucht bei 800 ms Integrationszeit rund
/// 1,3 s je Wert (Vishay 84286, Refresh time); oefter zu lesen liefert
/// denselben Wert noch einmal.
constexpr uint32_t LUX_TAKT_MS = 2000;

/// Zeitkonstante, mit der die Klinge dem Umgebungslicht folgt (27.09.2026),
/// JE STUFE - es sind zwei hintereinander (28.09.2026, main.cpp). Vorher
/// sprang sie mit jedem Messwert, also alle 2 s, sichtbar auf die neue
/// Helligkeit. Mit zwei Stufen zu je 1 s ist ein Wechsel nach rund 5 s durch
/// (95 %), setzt sanft ein und laeuft sanft aus.
constexpr uint32_t LUX_BLENDE_MS = 1000;

/// So oft wird der Dimmfaktor dabei nachgefuehrt. 50 Schritte je Sekunde
/// sind fuer das Auge stufenlos.
constexpr uint32_t LUX_BLENDE_TAKT_MS = 20;

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

static_assert(HALL_SPANNE_MIN_MT < HALL_SPANNE_MAX_MT,
              "Das Aussteuerungsfenster ist verdreht.");

static_assert(HALL_SPANNE_MAX_MT < TMAG_BEREICH_MT,
              "Die obere Grenze der Aussteuerung liegt ausserhalb des "
              "Messbereichs des Sensors. Dort ist die Feldkurve abgeschnitten, "
              "und ein abgeschnittener Gipfel hat keinen Schwerpunkt mehr.");

static_assert(HALL_GROB_SCHWELLE_MT > 0.200f &&
                  HALL_GROB_SCHWELLE_MT < HALL_SPANNE_MIN_MT,
              "Die Schwelle der Grobsuche muss ueber dem Erdfeld (rund "
              "0,05 mT) und unter der kleinsten zulaessigen Aussteuerung "
              "liegen - sonst bleibt sie an einer beliebigen Stelle stehen "
              "oder findet gar nichts.");

static_assert(TMAG_BEREICH_MT == 40.0f || TMAG_BEREICH_MT == 80.0f ||
                  TMAG_BEREICH_MT == 133.0f || TMAG_BEREICH_MT == 266.0f,
              "Der TMAG5273 kennt nur diese vier Messbereiche: +/-40 und "
              "+/-80 mT (Varianten A1..D1), +/-133 und +/-266 mT (A2..D2). "
              "Datenblatt SLYS045C Tabelle 8-6.");

static_assert(TMAG_CONV_AVG <= 5,
              "CONV_AVG ist drei Bit breit und kennt nur 0..5 (1x bis 32x). "
              "Datenblatt SLYS045C Tabelle 8-3.");

// --- Umgebungslicht -------------------------------------------------------

static_assert(LUX_DUNKEL > 0.0f && LUX_DUNKEL < LUX_HELL,
              "Die Ueberblendung rechnet mit dem Logarithmus der "
              "Beleuchtungsstaerke. Null oder eine verdrehte Spanne gibt es "
              "dort nicht.");

static_assert(LUX_ANTEIL_MIN > 0.0f && LUX_ANTEIL_MIN <= 1.0f,
              "Der kleinste Anteil ist ein Faktor auf den eingestellten "
              "Wert, also groesser als 0 und hoechstens 1. Abgeschaltet wird "
              "ueber den Nachtschalter, nicht ueber die Regelung.");

static_assert(LUX_NACHT_AUS < LUX_NACHT_AN,
              "Die Ausschaltschwelle muss UNTER der Einschaltschwelle "
              "liegen. Ist sie es nicht, gibt es keine Hysterese, und die "
              "Klinge schaltet in der Daemmerung im Minutentakt.");

static_assert(LUX_NACHT_AN <= LUX_HELL,
              "Eine Einschaltschwelle oberhalb der Helligkeit, ab der die "
              "Klinge voll leuchtet, ergibt keinen Zustand mehr, in dem "
              "geregelt wird.");

static_assert(HALL_SCHWELLE_ANTEIL > 0.0f && HALL_SCHWELLE_ANTEIL < 1.0f,
              "Die Rauschschwelle ist ein Anteil der Gipfelhoehe, also "
              "zwischen 0 und 1.");

static_assert(TMC_ADDR == 0x00 || TMC_ADDR == 0x01 || TMC_ADDR == 0x02 ||
                  TMC_ADDR == 0x03,
              "Die UART-Adresse kommt aus MS1/MS2 und kann nur 0..3 sein.");

// Der analoge Hall-Eingang ist mit dem KY-024 entfallen; an seine Stelle
// tritt der I2C-Bus. Was BLEIBT, ist die Pruefung, dass der INT-Eingang ein
// Eingang sein darf - und dass die beiden Busadern es NICHT sind: SDA und
// SCL muessen beide Richtungen koennen.

// Beim C3 gibt es keine reinen Eingaenge. Was es gibt, sind Pins, die beim
// Start eine Bedeutung haben (Strapping 2/8/9), zwei, die am USB haengen
// (18/19 - die einzige Konsole dieses Bretts), und die des internen Flashs
// (11..17). Wer die belegt, verliert den Start, die Konsole oder das Programm.
//
// IO2 UND IO8 SIND BEWUSST BELEGT - mit Gruen und Blau, als Ausgang,
// mit 10-k-Pull-up auf dem Brett, der sie beim Reset HIGH haelt. Das ist
// genau der eine Gebrauch, den die Arbeitsliste 2.1 fuer diese beiden Pins
// zulaesst. Deshalb steht die Pruefung hier ZWEIGETEILT: alle anderen
// Signale duerfen keinen Strapping-Pin treffen, die zwei LED-Kanaele
// muessen genau auf 2 und 8 liegen.
constexpr bool nicht_frei(uint8_t pin) {
  return pin == 2 || pin == 8 || pin == 9 || pin == 18 || pin == 19 ||
         (pin >= 11 && pin <= 17) || pin > 21;
}
static_assert(!nicht_frei(PIN_STEP) && !nicht_frei(PIN_DIR) &&
                  !nicht_frei(PIN_EN) && !nicht_frei(PIN_LED_PWM) &&
                  !nicht_frei(PIN_UART_TX) && !nicht_frei(PIN_UART_RX) &&
                  !nicht_frei(PIN_INDEX) && !nicht_frei(PIN_SDA) &&
                  !nicht_frei(PIN_SCL) && !nicht_frei(PIN_HALL_INT),
              "Ein Pin liegt auf einem Strapping-, USB- oder Flash-Pin des "
              "C3 (2/8/9, 18/19, 11..17).");
static_assert(PIN_LED_G_PWM == 2 && PIN_LED_B_PWM == 8,
              "Gruen und Blau sind die EINZIGEN Signale, die auf den "
              "Strapping-Pins 2 und 8 liegen duerfen - weil R15/R14 sie beim "
              "Reset HIGH halten. Ein anderes Signal dort muss dasselbe "
              "leisten, sonst startet der C3 nicht.");
constexpr bool nur_eingang(uint8_t) { return false; }

static_assert(!nur_eingang(PIN_STEP) && !nur_eingang(PIN_DIR) &&
                  !nur_eingang(PIN_EN) && !nur_eingang(PIN_LED_PWM) &&
                  !nur_eingang(PIN_UART_TX),
              "Ein Ausgang liegt auf GPIO34..39. Diese Pins koennen nicht "
              "treiben - siehe die Pinliste in Arbeitsstand Abschnitt 1.");

static_assert(!nur_eingang(PIN_SDA) && !nur_eingang(PIN_SCL),
              "SDA und SCL liegen auf GPIO34..39. Diese Pins haben keine "
              "Ausgangstreiber - ein I2C-Bus kann darauf nicht laufen, weil "
              "beide Adern vom Master aktiv nach LOW gezogen werden muessen.");

// Alle belegten Pins gegeneinander. Als Feld statt als Kette von
// Ungleichungen: die Kette wurde bei jedem neuen Signal laenger und war
// zuletzt unvollstaendig - PIN_INDEX und die beiden UART-Adern kamen darin
// gar nicht vor.
constexpr uint8_t ALLE_PINS[] = {PIN_UART_TX, PIN_UART_RX, PIN_STEP,
                                 PIN_DIR,     PIN_EN,      PIN_INDEX,
                                 PIN_LED_PWM, PIN_SDA,     PIN_SCL,
                                 PIN_HALL_INT
                                 , PIN_LED_G_PWM, PIN_LED_B_PWM
};

static_assert(LED_MAX_W > 0.0f, "Die Leistungsgrenze muss positiv sein.");
static_assert(LED_MAX_W <= 0.95f * LED_PD_DATENBLATT_W + 1e-4f,
              "Hoechstens 95 % der 3 W - sonst ueberhitzt die LED. "
              "Vorgabe 27.10.2026, nicht anheben.");
static_assert(LED_WATT_VOLL[0] <= LED_MAX_W && LED_WATT_VOLL[1] <= LED_MAX_W &&
                  LED_WATT_VOLL[2] <= LED_MAX_W,
              "Jede reine Farbe muss bei 100 % unter der Grenze bleiben.");
static_assert(LED_WATT_VOLL[0] + LED_WATT_VOLL[1] <= LED_MAX_W &&
                  LED_WATT_VOLL[0] + LED_WATT_VOLL[2] <= LED_MAX_W &&
                  LED_WATT_VOLL[1] + LED_WATT_VOLL[2] <= LED_MAX_W,
              "Jede Mischung aus zwei Farben muss bei 100 % unter der Grenze "
              "bleiben - so ist es vorgegeben (25.09.2026).");
static_assert(LED_FARBE_VORGABE <= 0xFFFFFF, "Farbe ist 0xRRGGBB.");

/// Farbe der Klinge, solange ein Timer laeuft - samt Alarm (27.09.2026,
/// Vorgabe: "beim Timer orange"). Am Geraet gemessen an den Voreinstellungen:
/// 255/128/0 ist dort schon Gelb, Orange liegt also deutlich roter.
constexpr uint32_t TIMER_FARBE = 0xFF3000;

// ZUEND_WARM (Warmweiss #FFD764, 28.09.2026) ist am 01.10.2026 entfallen:
// die Zuendung zieht die Farbe nicht mehr Richtung Weiss, sondern zu ihrer
// eigenen waermeren Variante (zeiger.h, waermer()).

/// Weiche Fahrt: so lange dauert auch der kleinste Sprung mindestens, und
/// ueber hoechstens so viele Schritte setzt die Beschleunigung ein.
constexpr uint32_t WEICH_MIN_MS = 180;
constexpr uint32_t WEICH_EINSATZ_MAX = 800;

constexpr bool pins_sind_verschieden() {
  for (size_t i = 0; i < sizeof(ALLE_PINS); i++)
    for (size_t j = i + 1; j < sizeof(ALLE_PINS); j++)
      if (ALLE_PINS[i] == ALLE_PINS[j])
        return false;
  return true;
}

static_assert(pins_sind_verschieden(),
              "Zwei Signale liegen auf demselben Pin.");

}  // namespace geraet
}  // namespace starwatch
