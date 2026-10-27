// ---------------------------------------------------------------------------
// VORLAGE. Diese Datei nach `zugangsdaten.h` kopieren und ausfuellen:
//
//   cp lib/testweb/zugangsdaten.example.h lib/testweb/zugangsdaten.h
//
// `zugangsdaten.h` steht in .gitignore und gehoert NICHT ins Repository -
// dieselbe Regel wie bei esphome/secrets.yaml. Vor jedem Commit pruefen.
//
// OHNE diese Datei laeuft die Firmware trotzdem: sie spannt dann einen
// eigenen Zugangspunkt auf, und die Test-Weboberflaeche ist unter
// http://192.168.4.1 erreichbar. Zum Pruefen am Aufbau genuegt das.
// Der Weg ueber das Heimnetz ist bequemer, weil das Handy dabei online bleibt.
//
// SEIT 23.09.2026 IST DIESER ZUGANGSPUNKT OFFEN, solange noch keine
// WLAN-Zugangsdaten bekannt sind - weder hier einkompiliert noch im NVS
// gespeichert. Kein Schluessel, kein Abtippen von der seriellen Konsole.
// Dafuer ist dort AUSSCHLIESSLICH das Eintragen des WLANs frei; Motor, Licht,
// Geraeteblatt und Update verlangen auch dort das Token.
//
// Legst du diese Datei an, gibt es den Zugangspunkt bei der Einrichtung gar
// nicht erst: die Uhr haengt nach dem Flashen von selbst im Netz.
// ---------------------------------------------------------------------------
#pragma once

// --- Heimnetz -------------------------------------------------------------
#define STARWATCH_WLAN_SSID "MeinNetz"
#define STARWATCH_WLAN_PASSWORT "MeinPasswort"

// Feste IP, optional. Auskommentiert lassen fuer DHCP.
//
// ACHTUNG, AM PRUEFSTAND GELERNT: bei fester IP MUSS ein DNS-Server gesetzt
// werden. Ohne ihn bleibt die NTP-Zeit ungueltig - das trifft diese Firmware
// noch nicht (die Zeit wird von Hand gestellt), aber ab AP 3.2 sofort.
// #define STARWATCH_IP        192, 168, 1, 42
// #define STARWATCH_GATEWAY   192, 168, 1, 1
// #define STARWATCH_MASKE     255, 255, 255, 0
// #define STARWATCH_DNS       192, 168, 1, 1

// --- Eigener Zugangspunkt, falls das Heimnetz nicht erreichbar ist --------
//
// UEBERHOLT SEIT AP 6.2 (24.08.2026): Name und Schluessel des eigenen
// Zugangspunkts kommen NICHT mehr von hier. Jede Uhr wuerfelt sie beim ersten
// Start selbst und legt sie im Fertigungsbereich ab - der Name enthaelt die
// Seriennummer ("StarWatch-089C"). Nachzulesen im Geraeteblatt: beim Start auf
// der Konsole und in der Weboberflaeche unter "Geraet".
//
// UND SEIT 23.09.2026 wird der Schluessel bei der Ersteinrichtung gar nicht
// mehr gebraucht - siehe oben. Er gilt nur noch fuer den Rettungsweg: wenn
// Zugangsdaten bekannt sind, das Netz aber nicht erreichbar ist.
//
// Die beiden Zeilen bleiben nur stehen, damit alte Kopien dieser Datei noch
// uebersetzen. Sie werden nicht mehr benutzt.
#define STARWATCH_AP_SSID "StarWatch-Test"
#define STARWATCH_AP_PASSWORT "unbenutzt-siehe-AP-6.2"
