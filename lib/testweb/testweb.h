// ---------------------------------------------------------------------------
// Star Watch - lib/testweb: WLAN und Test-Weboberflaeche.
//
// ===========================================================================
// DAS IST EIN MESSGERAET, KEIN PRODUKT
// ===========================================================================
// Diese Bibliothek ist das Gegenstueck zu esphome/test.yaml: eine Oberflaeche,
// mit der man am Aufbau arbeiten kann, ohne am USB-Kabel zu haengen. Sie ist
// NICHT die Schnittstelle der Uhr.
//
// Deshalb heissen die Endpunkte `/test/...` und ausdruecklich NICHT
// `/api/v1/...`:
//
//   - Die echte API ist der Vertrag mit der App und mit jedem ausgelieferten
//     Geraet. Sie entsteht in AP 3.4 nach `docs/api/starwatch-v1.yaml`, und
//     die Datei gibt es noch nicht (AP 1.6, haengt an 1.3 und 1.5).
//   - Nachtraeglich geaenderte Endpunkte kosten ein `v2`, und zwar fuer immer.
//     Was hier steht, darf sich jederzeit aendern - genau darum trägt es
//     einen anderen Pfad.
//   - Nach aussen reicht die echte API GRAD als Fliesskommazahl. Diese
//     Oberflaeche zeigt bewusst MIKROSCHRITTE, weil man am Aufbau in
//     Schritten denkt und misst.
//
// AP 3.5 baut die eingebettete Weboberflaeche des Produkts - eine Seite, die
// dieselbe API spricht wie die App. Dann kann diese Bibliothek weg.
//
// ===========================================================================
// WARUM DER EINGEBAUTE WebServer UND NICHT ESPAsyncWebServer
// ===========================================================================
// `WebServer.h` steckt im Arduino-Core, kostet also keine Abhaengigkeit und
// keinen Fork. Er ist synchron: `handleClient()` laeuft in der Hauptschleife.
//
// Das war im ESPHome-Pruefstand ein Problem, weil dort die SCHRITTE in der
// Hauptschleife entstehen - jede Verzoegerung wurde zu einem Ruckeln. Hier
// erzeugt MCPWM die Impulse in Hardware, die Schleife hat mit dem Lauf des
// Zeigers nichts zu tun. Fuer ein Messgeraet ist das die einfachere und
// haltbarere Wahl. AP 3 nimmt dann ESPAsyncWebServer, wo es um Dauerbetrieb
// und SSE geht.
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include <antrieb.h>
#include <betrieb.h>
#include <hall.h>
#include <lux.h>
#include <licht.h>
#include <aktualisierung.h>

namespace starwatch {

/// Ausgabe, die gleichzeitig auf die serielle Konsole und in einen
/// Ringpuffer schreibt.
///
/// Der Grund ist eine Regel, die AP 3.4 fuer die echte API aufstellt und die
/// hier schon gilt: **alle Schnittstellen bedienen denselben Zustand, kein
/// doppelter Code.** Konsole und Weboberflaeche fuehren dieselbe
/// Kommandofunktion aus und sehen dieselbe Ausgabe. Sonst laufen die beiden
/// Bedienwege auseinander, und man debuggt am Ende die Oberflaeche statt die
/// Uhr.
class Doppelausgabe : public Print {
 public:
  size_t write(uint8_t c) override;
  size_t write(const uint8_t *puffer, size_t laenge) override;

  /// Der Ringpuffer als Text, aeltestes zuerst.
  String log() const;
  void log_leeren();

 private:
  static constexpr size_t GROESSE = 4096;
  char ring_[GROESSE] = {0};
  size_t schreib_ = 0;
  bool umgelaufen_ = false;
};

/// Diese Ausgabe benutzen ALLE Kommandos - nie direkt `Serial`.
extern Doppelausgabe konsole;

/// Signatur der Kommandofunktion aus main.cpp.
using Kommandofn = void (*)(char *zeile);

class Testweb {
 public:
  /// Sendeleistung fuer diesen Start zuruecknehmen - VOR beginn() rufen.
  /// setup() tut das nach einem Brownout: siehe SENDELEISTUNG in testweb.cpp.
  static void funk_drosseln();
  static bool funk_gedrosselt();

  /// WLAN aufbauen und den Webserver starten.
  ///
  /// Sucht das in `zugangsdaten.h` eingetragene Netz. Fehlt die Datei oder
  /// bleibt die Verbindung aus, spannt das Geraet einen EIGENEN
  /// Zugangspunkt auf - dann ist die Oberflaeche auch ohne Konfiguration
  /// erreichbar, und man muss nicht erst Zugangsdaten eintragen, um den
  /// Aufbau zu pruefen.
  ///
  /// Gibt false zurueck, wenn beides misslingt. Der Rest der Firmware laeuft
  /// dann trotzdem weiter - ueber die serielle Konsole.
  bool beginn(Antrieb *antrieb, Hall *hall, Licht *licht, Lux *lux, Betrieb *betrieb,
              Kommandofn kommando);

  /// Aus der Hauptschleife aufrufen.
  void tick();

  // -------------------------------------------------------------------------
  // WLAN-Aufsicht. Der Arduino-Kern verbindet von selbst wieder; diese
  // Funktionen machen es SICHTBAR und stossen die Zeit danach an.
  // -------------------------------------------------------------------------

  /// Einmal true, nachdem das Heimnetz zurueckgekommen ist. Der Aufrufer
  /// stellt damit die Zeit sofort neu, statt auf den SNTP-Takt zu warten.
  bool wlan_rueckkehr_abholen();

  /// Verbindung mit den GESPEICHERTEN Daten neu aufbauen. Trennt kurz - damit
  /// laesst sich der Ausfallpfad pruefen, ohne den Router anzufassen.
  void neu_verbinden();

  /// Wie lange das Heimnetz schon fehlt, in ms. 0 = es steht.
  uint32_t wlan_weg_ms() const;

  bool verbunden() const { return verbunden_; }
  bool eigener_ap() const { return eigener_ap_; }

  /// Laeuft der Zugangspunkt OHNE Schluessel?
  ///
  /// Das unterscheidet die beiden Faelle, in denen die Uhr ein eigenes Netz
  /// aufspannt, und sie sind NICHT dasselbe:
  ///
  ///   offen      - es sind noch gar keine WLAN-Zugangsdaten bekannt. Die Uhr
  ///                ist im Auslieferungszustand, es gibt nichts zu schuetzen,
  ///                und ein Schluessel, den niemand kennen kann, sperrt nur
  ///                den Besitzer aus. Dort ist AUSSCHLIESSLICH die
  ///                WLAN-Einrichtung frei - kein Motor, kein Licht, kein
  ///                Geraeteblatt, kein Update.
  ///   gesichert  - Zugangsdaten sind bekannt, das Netz ist nur gerade nicht
  ///                erreichbar. Das ist der Rettungsweg, und dort gilt wie
  ///                bisher: wer den Schluessel aus dem Geraeteblatt hat, darf
  ///                alles.
  bool offener_ap() const { return offener_ap_; }
  String adresse() const;

  /// Der Zustand als JSON fuer `/test/zustand`. Oeffentlich, weil die
  /// Routen-Lambdas des WebServers keine Member sind.
  String zustand_json();

  /// Ein Kommando ausfuehren, als kaeme es von der Konsole.
  void ausfuehren(char *zeile);

  // -------------------------------------------------------------------------
  // Einstellungen als Datei - AP 3.7.2
  //
  // Oeffentlich aus demselben Grund wie `zustand_json()`: die Routen-Lambdas
  // des WebServers sind keine Member.
  //
  // WAS NICHT DARIN STEHT, UND WARUM:
  //   Hall-Offset      Fertigungswert. Eine Datei von einer ANDEREN Uhr wuerde
  //                    damit deren Kalibrierung mitbringen (AP 3.1).
  //   WLAN-Passwort    Der Endpunkt ist ungesichert, solange AP 1.3 fehlt. Wer
  //                    im Netz ist, koennte den Schluessel abrufen.
  // -------------------------------------------------------------------------

  /// Die Aktualisierung nachtraeglich anbinden. Getrennt von `beginn()`,
  /// weil die Weboberflaeche ohne sie laufen koennen muss - sie ist fuer den
  /// Diagnosebericht da, nicht fuer den Betrieb.
  void aktualisierung_anbinden(Aktualisierung *a) { aktiv_update_ = a; }

  /// Der vollstaendige Diagnosebericht als KLARTEXT (AP 6.3).
  ///
  /// Bewusst nicht JSON: er wird von jemandem kopiert und verschickt, der die
  /// Uhr geschenkt bekommen hat. Er muss ohne Werkzeug lesbar sein - und
  /// gleichzeitig alles enthalten, was eine Ferndiagnose braucht, damit aus
  /// "sie spinnt" keine zwanzig Rueckfragen werden.
  String diagnose_text();

  /// Die Benutzerwerte als JSON-Datei.
  String einstellungen_json() const;

  /// Eine Einstellungsdatei einlesen. Prueft Schema und Geraetetyp, uebernimmt
  /// nur bekannte Werte in gueltigen Grenzen und sichert danach in NVS.
  /// @param meldung Klartext fuer den Bediener, in jedem Fall gesetzt.
  /// @return false, wenn die Datei abgelehnt wurde.
  bool einstellungen_uebernehmen(const String &datei, String *meldung);

  // -------------------------------------------------------------------------
  // WLAN einrichten, OHNE neu zu flashen
  //
  // Vorstufe von AP 6.1 (Ersteinrichtung ueber Access Point): das Geraet
  // spannt sein eigenes Netz auf, man waehlt dort das Heimnetz aus und gibt
  // das Passwort ein. Beides landet in NVS und wird beim naechsten Start
  // benutzt.
  //
  // STAND 24.08.2026: das CAPTIVE PORTAL IST GEBAUT - Catch-all-DNS auf jeden
  // Namen, dazu die Erkennungsadressen von iOS/macOS, Android, Windows und
  // Firefox. Wer sich mit dem Zugangspunkt verbindet, bekommt das
  // "Anmelden"-Fenster wie im Hotel-WLAN. Dieser Kommentar behauptete das
  // Gegenteil, war aber schlicht aelter als der Code.
  //
  // Was fuer AP 6.1 wirklich noch fehlt, haengt beides an AP 1.3
  // (Zugriffsschutz, noch nicht entschieden): die Uebergabe des Geraetetokens
  // und ein dokumentiertes Kopplungsprotokoll. Solange die entstehen, ist das
  // hier eine Bequemlichkeit fuer den Aufbau und noch keine Ersteinrichtung
  // fuer Kunden.
  // -------------------------------------------------------------------------

  /// Erreichbare Netze suchen. Blockiert rund zwei Sekunden.
  /// Gibt die Anzahl zurueck.
  int netze_suchen();

  /// Adresse im Heimnetz: DHCP oder fest (26.09.2026).
  struct Adresswahl {
    bool fest = false;
    String ip, maske, gw, dns;  ///< dns leer = Gateway
  };

  /// Prueft eine feste Adresse, BEVOR irgendetwas gespeichert wird: gueltige
  /// Adressen, lueckenlose Maske, IP und Gateway im selben Netz, keine Netz-
  /// oder Rundrufadresse. `fehler` sagt, was nicht stimmt.
  static bool adresse_pruefen(const Adresswahl &a, String *fehler);

  /// Zugangsdaten (und optional die Adresse) speichern und verbinden. Ein
  /// leeres `ssid` loescht beides.
  ///
  /// SCHEITERT DIE VERBINDUNG, KOMMT DER ALTE STAND ZURUECK - und aus dem
  /// Heimnetz heraus auch gleich die alte Verbindung.
  ///
  /// Das PASSWORT wird nie ins Log geschrieben - der Ringpuffer ist ueber die
  /// Weboberflaeche abrufbar, und ein WLAN-Passwort hat dort nichts zu suchen.
  bool wlan_setzen(const String &ssid, const String &passwort,
                   const Adresswahl *adresse = nullptr);

  /// Gespeichertes Passwort - NUR fuer "gleiches Netz, nur die Adresse
  /// aendern", damit niemand dafuer das WLAN-Passwort neu tippen muss. Geht
  /// nie ueber eine Route oder ins Log hinaus.
  String gespeichertes_passwort() const;

  /// Gespeicherte Adresseinstellung.
  Adresswahl gespeicherte_adresse() const;

  /// Gespeicherte SSID, oder leer. Das Passwort gibt es NICHT als Getter.
  String gespeicherte_ssid() const;

  /// Den eigenen Zugangspunkt abschalten. Geht nur, wenn das Heimnetz steht -
  /// sonst waere das Geraet nur noch am Kabel erreichbar.
  void ap_abschalten();

  /// Adresse im Heimnetz, oder leer.
  String heimnetz_adresse() const;

  // -------------------------------------------------------------------------
  // OTA und Log ueber Netz - fuer die ARBEIT am Aufbau
  //
  // ===========================================================================
  // DAS IST NICHT DAS OTA AUS AP 3.6
  // ===========================================================================
  // AP 3.6 und 3.7.5 verlangen fuer das PRODUKT: zwei Partitionen mit
  // Rollback, ein signiertes Image, Pruefung der Signatur VOR dem Aktivieren
  // und einen Downgrade-Schutz. Nichts davon ist hier drin.
  //
  // Was hier drin ist, ist ArduinoOTA: das Entwickler-OTA des Cores, mit
  // Passwort, ohne Signatur. Es spart das Kabel, waehrend am Aufbau
  // gearbeitet wird - und es faellt mit dieser Bibliothek weg, wenn AP 3.6
  // das richtige OTA baut.
  //
  // Ein Update ueber diesen Weg prueft NICHT, woher es kommt. Wer im Netz
  // ist und das Passwort hat, kann Code einspielen. Fuer ein Messgeraet auf
  // dem Schreibtisch ist das die richtige Abwaegung, fuer ein verkauftes
  // Geraet nicht.
  // -------------------------------------------------------------------------

  /// Log ueber TCP mitlesen (Port 23). Damit geht
  ///     pio device monitor --port socket://<ip>:23
  /// Nur EIN Zuhoerer gleichzeitig - mehr braucht niemand, und mehr kostet
  /// Speicher, den die Uhr fuer den Antrieb braucht.
  static constexpr uint16_t LOG_PORT = 23;

 private:
  void ota_einrichten();

 public:
  /// Waehrend eines Updates niemanden bedienen - siehe update_laeuft.
  static void stumm(bool an);

 private:
  void log_tick();
  void dns_starten();
  void dns_beenden();
  void routen_setzen();

  void wlan_aufsicht();

  Aktualisierung *aktiv_update_ = nullptr;
  Antrieb *antrieb_ = nullptr;
  Hall *hall_ = nullptr;
  Lux *lux_ = nullptr;
  bool offener_ap_ = false;
  Licht *licht_ = nullptr;
  Betrieb *betrieb_ = nullptr;
  Kommandofn kommando_ = nullptr;

  bool verbunden_ = false;
  bool eigener_ap_ = false;
  bool sta_aktiv_ = false;
};

}  // namespace starwatch
