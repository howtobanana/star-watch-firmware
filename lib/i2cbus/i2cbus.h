// ---------------------------------------------------------------------------
// Star Watch - lib/i2cbus: der I2C-Bus und seine drei Steckplaetze.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Den I2C-Bus einrichten, ihn absuchen und Register lesen und schreiben.
// Mehr nicht. Was die Register BEDEUTEN, weiss diese Bibliothek nicht - das
// steht im Treiber des jeweiligen Bauteils (lib/hall, lib/lux).
//
// ===========================================================================
// WARUM EINE EIGENE BIBLIOTHEK UND NICHT ZWEIMAL DASSELBE
// ===========================================================================
// Am Bus haengen drei Steckplaetze: J5 (TMAG5273, Hall), J7
// (SCD41, CO2) und J8 (VEML7700, Licht). Jeder Treiber braucht dieselben
// vier Zugriffe. Zweimal geschrieben heisst zweimal dieselbe Fehlerbehandlung
// - und beim zweiten Mal wird sie vergessen.
//
// ===========================================================================
// DER SUCHLAUF IST KEINE SPIELEREI
// ===========================================================================
// Zwei der drei Module sind optional und moeglicherweise gar nicht gesteckt,
// und das vierte Geraet am Bus - der TMAG5273 - laesst sich auf eine andere
// Adresse umstellen (Datenblatt SLYS045C Tabelle 8-15). Ein Suchlauf beim
// Start, der meldet was er findet, spart jede spaetere Ratestunde: steht
// nichts im Protokoll, liegt es an der Verdrahtung und nicht am Treiber.
//
// ===========================================================================
// WAS DIESE BIBLIOTHEK NICHT TUT
// ===========================================================================
// Sie wiederholt keinen fehlgeschlagenen Zugriff. Ein I2C-Fehler auf dieser
// Platine heisst Steckverbinder, Ader oder Modul - nichts davon wird durch
// einen zweiten Versuch besser, und ein stiller Wiederholungsversuch
// verschleiert genau den Fehler, den man sehen will. Der Aufrufer bekommt
// false und entscheidet selbst.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

namespace starwatch {
namespace i2cbus {

/// Bus starten. Pins und Taktrate stehen in geraet.h.
/// Gibt false zurueck, wenn der Treiber den Bus nicht einrichten konnte -
/// dann ist an den Pins etwas grundsaetzlich falsch.
bool beginn();

/// Antwortet unter dieser Adresse ein Baustein?
/// Bus pruefen und freimachen (27.09.2026, Befehl 'ib'). Gibt den Treiber
/// frei, liest SDA/SCL als Eingang (Pull-ups R11/R12 auf dem Brett), taktet
/// SCL neunmal, falls ein Baustein SDA festhaelt, und richtet den Bus neu
/// ein. Die Pegel VOR dem Freitakten gehen an den Aufrufer: beide hoch =
/// Leitungen frei, SDA tief = Kurzschluss oder haengender Baustein, beide
/// tief = Masseschluss oder verdreht gesteckt.
bool pruefen(bool *sda_hoch, bool *scl_hoch, bool *sda_nach_takten);

bool vorhanden(uint8_t adresse);

/// Den ganzen Bus absuchen (0x08 bis 0x77 - darunter und darueber liegen
/// reservierte Adressen). Schreibt die gefundenen Adressen nach `gefunden`
/// und liefert ihre Anzahl.
size_t absuchen(uint8_t *gefunden, size_t platz);

/// Klartextname zu einer Adresse, soweit sie auf dieser Platine vorgesehen
/// ist. Sonst nullptr - dann haengt etwas am Bus, das dort nicht hingehoert,
/// und genau das soll das Protokoll sagen.
const char *name(uint8_t adresse);

// ---------------------------------------------------------------------------
// Register
// ---------------------------------------------------------------------------

/// Ein Byte in ein Register schreiben.
bool byte_schreiben(uint8_t adresse, uint8_t reg, uint8_t wert);

/// Ein Byte aus einem Register lesen.
bool byte_lesen(uint8_t adresse, uint8_t reg, uint8_t *wert);

/// Mehrere aufeinanderfolgende Register am Stueck lesen. Der TMAG5273 zaehlt
/// dabei von selbst weiter, so dass X, Y und Z in EINER Uebertragung kommen -
/// das ist wichtig, weil drei einzelne Lesevorgaenge drei verschiedene
/// Wandlungen treffen wuerden und der Betrag dann aus Werten entstuende, die
/// nicht zusammengehoeren.
bool lesen(uint8_t adresse, uint8_t reg, uint8_t *puffer, size_t anzahl);

/// 16-Bit-Wort, NIEDERWERTIGES Byte zuerst. So legt der VEML7700 seine
/// Register ab (Vishay 84286, "the data byte is transmitted LSB first").
bool wort_le_schreiben(uint8_t adresse, uint8_t reg, uint16_t wert);
bool wort_le_lesen(uint8_t adresse, uint8_t reg, uint16_t *wert);

}  // namespace i2cbus
}  // namespace starwatch
