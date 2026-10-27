// ---------------------------------------------------------------------------
// Star Watch - Firmware aus dem Netz holen, pruefen und einspielen.
//
// AP 3.7.1 (Autoupdate) auf der Grundlage von AP 3.6 (OTA mit Rollback) und
// AP 3.7.5 (Signatur). Das Verfahren stammt NICHT von hier - es steht in
// werkzeuge/signieren.py, und dieses Modul ist die Gegenseite dazu. Wer eines
// von beiden aendert, muss das andere mitaendern; `signieren.py pruefen` macht
// genau das, was hier passiert, und ist die Gegenprobe ohne Geraet.
//
// ===========================================================================
// WARUM EINE SIGNATUR UND NICHT NUR HTTPS
// ===========================================================================
// Die Uhr holt Firmware selbstaendig von einer URL. Wer DNS oder das Netz
// kontrolliert, koennte damit beliebigen Code in eine fremde Wohnung spielen.
// HTTPS bindet das Geraet an ein Zertifikat mit Ablaufdatum - in zwei Jahren
// ist die Wurzel abgelaufen und die Uhr nimmt keine Updates mehr an, obwohl
// nichts kaputt ist.
//
// Die Signatur ist vom Transportweg unabhaengig. Deshalb - und NUR deshalb -
// darf die TLS-Verbindung hier ohne Zertifikatspruefung laufen
// (`setInsecure()`): der Angreifer, der den Transport kontrolliert, bekommt
// trotzdem kein Image durch, weil er den signierten Block nicht faelschen
// kann. Ohne Signatur waere das grob fahrlaessig.
//
// ===========================================================================
// WAS GEPRUEFT WIRD, UND IN WELCHER REIHENFOLGE
// ===========================================================================
//   1 Schema und Signatur des MANIFESTS      - billig, vor jedem Download
//   2 Version neuer als die laufende         - kein Downgrade
//   3 laufende Version >= `mindest`          - kein Sprung ueber eine
//                                              Zwischenfassung, die etwas
//                                              wandeln muss
//   4 SHA-256 des Images WAEHREND des Ladens - der Hash steht im signierten
//                                              Block, die URL nicht
//   5 erst danach wird die neue Partition aktiviert
//
// Fehlt einer der Schritte, wird NICHT eingespielt. Es gibt keinen Schalter,
// der die Pruefung abschaltet.
//
// ===========================================================================
// ROLLBACK - UND WARUM DAS BESTAETIGEN STRENG SEIN MUSS
// ===========================================================================
// `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` ist im Kern gesetzt (nachgesehen
// in framework-arduinoespressif32-libs/esp32/sdkconfig). Ein frisch
// eingespieltes Image startet damit im Zustand PENDING_VERIFY. Bestaetigt es
// sich nicht selbst, faehrt der Bootloader beim naechsten Start die vorige
// Fassung.
//
// DAS BESTAETIGUNGSKRITERIUM IST DER GANZE WERT DIESES MECHANISMUS. Eine
// Firmware, die bootet, aber deren Antrieb nicht anlaeuft, wuerde sich sonst
// selbst freigeben - und das Rollback waere wertlos. Bestaetigt wird deshalb
// erst, wenn WLAN steht, die Zeit gueltig ist UND eine Referenzfahrt gelaufen
// ist. Genau so steht es im Umsetzungsplan AP 3.7.1.
// ---------------------------------------------------------------------------
#pragma once

#include <Arduino.h>

#include <cstdint>
#include <string>

#include <zeiger.h>  // semver_vergleich - reine Rechnung, host-getestet

namespace starwatch {

/// Wann die Uhr von sich aus nach Firmware sieht.
enum class Updatemodus {
  aus,          ///< nie. Auslieferungszustand.
  manuell,      ///< nur auf Knopfdruck
  automatisch,  ///< einmal taeglich, nachts
};

const char *updatemodus_text(Updatemodus m);

/// Welcher Kanal - also welches Manifest gelesen wird.
///
/// KEIN EIGENER SPEICHERWERT. Der Kanal wird aus der URL ABGELEITET, damit
/// es nicht zwei Werte gibt, die einander widersprechen koennen: eine Uhr,
/// die "beta" gespeichert haette, aber auf die stabile URL zeigt, waere ein
/// Fehler, den niemand sieht. So kann es ihn nicht geben.
enum class Updatekanal {
  stabil,  ///< ausgelieferte Uhren
  beta,    ///< die eigene Uhr, zieht Versuche mit
  eigen,   ///< eine von Hand gesetzte URL (`yu`)
};

const char *updatekanal_text(Updatekanal k);

/// Was bei der letzten Pruefung herauskam - fuer die Anzeige.
enum class Updatestand {
  unbekannt,      ///< noch nie geprueft
  aktuell,        ///< nichts Neues
  verfuegbar,     ///< es gibt eine neuere Fassung
  laeuft,         ///< wird gerade geladen
  fehler,         ///< siehe `meldung()`
  eingespielt,    ///< fertig, Neustart steht an
};

class Aktualisierung {
 public:
  /// @param version die laufende Firmware-Version (semver).
  void beginn(const char *version);

  /// Wird gerufen, wenn das Laden beginnt (true) und wenn es endet (false).
  /// Damit kann der Aufrufer alles andere stilllegen, was am Netz haengt.
  ///
  /// WARUM DAS NOETIG IST, am Geraet gemessen: eine offene Weboberflaeche
  /// fragt im Sekundentakt ab, und jede Anfrage belegt Socket und Speicher
  /// neben dem Update-Strom. Beim Entwickler-OTA brach die Uebertragung
  /// dreimal hintereinander ab; mit geschlossener Seite lief sie auf Anhieb
  /// durch. Das Produkt-Update laedt 1,3 MB - dort waere es dasselbe, nur
  /// nachts und unbemerkt.
  void ruhe_hook(void (*f)(bool)) { ruhe_ = f; }

  /// Aus der Hauptschleife. Uebernimmt das taegliche Pruefen und das
  /// Bestaetigen eines frisch eingespielten Images.
  ///
  /// @param stunde       Stunde 0..23, oder <0 wenn keine Zeit bekannt ist.
  /// @param darf_stoeren false, solange ein Timer laeuft oder gefahren wird -
  ///                     dann wird nichts eingespielt.
  void tick(int stunde, bool darf_stoeren);

  /// Jetzt nachsehen, ob es etwas Neues gibt. Blockiert einige Sekunden.
  /// @return true, wenn eine neuere, gueltig signierte Fassung bereitsteht.
  bool pruefen();

  /// Die zuletzt gefundene Fassung laden und einspielen. Blockiert lange.
  /// Nach Erfolg MUSS neu gestartet werden.
  bool einspielen();

  /// Bestaetigen, dass die laufende Fassung taugt - damit der Bootloader sie
  /// nicht zurueckrollt. Ruft `tick()` selbst, sobald die Bedingungen
  /// stimmen; hier oeffentlich fuer den Ausnahmefall.
  void bewaehrt_melden();

  /// Laeuft gerade ein Image auf Bewaehrung?
  bool auf_bewaehrung() const { return auf_bewaehrung_; }

  /// Klartext: welche Partition laeuft, und in welchem Zustand.
  /// Fuer die Diagnose - ohne das laesst sich nicht sagen, OB das Rollback
  /// ueberhaupt scharf ist.
  std::string partition_text() const;

  Updatemodus modus() const { return modus_; }
  void modus(Updatemodus m);

  const std::string &url() const { return url_; }
  void url(const std::string &u);

  /// Aus der URL abgelesen, nicht gespeichert.
  Updatekanal kanal() const;
  /// Setzt die zum Kanal gehoerende URL. `eigen` tut nichts - dafuer `url()`.
  void kanal(Updatekanal k);

  Updatestand stand() const { return stand_; }
  const std::string &meldung() const { return meldung_; }
  const std::string &gefundene_version() const { return gefunden_; }
  const char *version() const { return version_; }

 private:
  bool manifest_holen(std::string *roh);
  bool manifest_pruefen(const std::string &roh);
  bool signatur_pruefen(const std::string &block, const std::string &sig_b64);
  bool image_laden(const std::string &bild_url, size_t groesse,
                   const std::string &sha_soll);

  const char *version_ = "0.0.0";
  Updatemodus modus_ = Updatemodus::aus;
  Updatestand stand_ = Updatestand::unbekannt;
  std::string url_;
  std::string meldung_;
  std::string gefunden_;
  std::string bild_url_;
  std::string sha_soll_;
  size_t groesse_ = 0;

  void (*ruhe_)(bool) = nullptr;
  bool auf_bewaehrung_ = false;
  uint32_t start_ms_ = 0;
  uint32_t letzte_pruefung_ms_ = 0;
  int letzte_stunde_ = -1;
};


}  // namespace starwatch
