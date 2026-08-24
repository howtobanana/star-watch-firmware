// ---------------------------------------------------------------------------
// Star Watch - lib/betrieb: Uhr, Modi, Timer. AP 2.4 und 2.6.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Der Zustand des Geraets und die Entscheidung, was als Naechstes passiert.
// Diese Bibliothek kennt Antrieb, Hall und Licht - umgekehrt nicht.
//
// ===========================================================================
// ABSOLUT, NIE FORTGESCHRIEBEN (Architekturregel 8)
// ===========================================================================
// Die Sollposition wird JEDES MAL neu aus der Uhrzeit gerechnet, nie aus der
// vorherigen Sollposition. Nach einem Zeitsprung stellt sich die Uhr damit
// von selbst wieder richtig, statt einen Fehler mitzuschleppen.
//
// Die Rechnungen stehen in lib/zeiger und sind host-getestet. Diese
// Bibliothek RUFT sie, sie rechnet nicht selbst.
//
// ===========================================================================
// ZEITQUELLE - IN AP 2 NOCH VON HAND
// ===========================================================================
// AP 2 ist "ohne Netz". Die Uhrzeit wird deshalb ueber die Konsole gestellt
// und danach aus millis() fortgezaehlt. Das ist keine Uhr fuer die Wand -
// der Quarz des ESP32 laeuft um Sekunden je Tag daneben - aber es genuegt,
// um den Uhrbetrieb, die Gangarten und den Zeigerlauf zu pruefen.
//
// AP 3.2 ersetzt das durch configTzTime() mit POSIX-Zeitzone (Wien:
// CET-1CEST,M3.5.0,M10.5.0/3). AP 1.2 entscheidet, ob zusaetzlich ein
// Zeitchip kommt - ohne ihn kennt die Uhr nach einem Stromausfall ohne WLAN
// ihre Uhrzeit nicht.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include <antrieb.h>
#include <hall.h>
#include <licht.h>
#include <zeiger.h>

namespace starwatch {

enum class Zeitmodus {
  aus,            ///< der Zeiger wird nicht gestellt
  fliessend,      ///< ein Mikroschritt alle SEK_JE_SCHRITT
  minutensprung,  ///< einmal je Minute REV/720 Schritte
};

const char *zeitmodus_text(Zeitmodus m);

class Betrieb {
 public:
  void beginn(Antrieb *antrieb, Hall *hall, Licht *licht);

  // -------------------------------------------------------------------------
  // Zeit
  // -------------------------------------------------------------------------

  /// Uhrzeit stellen. Ab hier laeuft sie aus millis() weiter.
  void zeit_setzen(int stunde, int minute, int sekunde);
  bool zeit_gueltig() const { return zeit_gueltig_; }

  /// Sekunden seit Mitternacht, 0 .. 86399.
  int32_t tageszeit_sek() const;

  /// Uhrzeit als "HH:MM:SS", oder "--:--:--" ohne gestellte Zeit.
  std::string uhrzeit_text() const;

  void zeitmodus(Zeitmodus m);
  Zeitmodus zeitmodus() const { return zeitmodus_; }

  /// Sollposition zur aktuellen Zeit. Ohne gueltige Zeit -1.
  int32_t soll_jetzt() const;

  // -------------------------------------------------------------------------
  // Kalibrierung
  // -------------------------------------------------------------------------

  void hall_offset(int32_t schritte);
  int32_t hall_offset() const { return hall_offset_; }

  /// "Hier ist zwoelf Uhr": die Istposition wird zur Null, der Offset wird
  /// entsprechend gemerkt. Das ist der Knopf, mit dem eingemessen wird -
  /// jede gebaute Uhr hat ihren eigenen Offset, weil der Winkel zwischen
  /// Magnet und Klinge erst bei der Montage entsteht.
  ///
  /// SCHALTET DEN UHRBETRIEB AUS, wie jeder Handeingriff - siehe
  /// `uhr_wurde_abgeschaltet`. Ein Offset, der aus einer Position gerechnet
  /// wird, die die Uhr eine Sekunde spaeter weiterschiebt, ist falsch.
  ///
  /// @return true, wenn dafuer ein laufender Uhrbetrieb beendet wurde.
  bool hier_ist_zwoelf();

  /// Referenzfahrt. Blockiert, bis sie fertig ist; `pumpe` haelt dabei die
  /// Konsole am Leben und kann abbrechen.
  Homingergebnis referenzfahrt(Hall::Pumpe pumpe);
  const Homingergebnis &letztes_homing() const { return letztes_homing_; }
  bool homing_laeuft() const { return homing_laeuft_; }

  // -------------------------------------------------------------------------
  // Timer
  // -------------------------------------------------------------------------

  /// Der Zeiger macht aus der Laufzeit GENAU EINE Umdrehung: er startet auf
  /// zwoelf und kommt dort wieder an, wenn die Zeit um ist. Damit ist der
  /// Timer ohne Skala ablesbar - halb herum heisst halbe Zeit.
  ///
  /// Schaltet den Uhrbetrieb aus: beide brauchen den Zeiger als Anzeige.
  /// Gibt false zurueck bei Dauer 0 - das ist kein Timer.
  /// @param epoch_jetzt aktuelle Unixzeit, oder 0 wenn keine bekannt ist.
  ///        Ist sie bekannt, wird das ABSOLUTE Ende im NVS gemerkt und
  ///        ueberlebt damit einen Neustart.
  bool timer_starten(uint32_t sekunden, uint32_t epoch_jetzt = 0);

  // -------------------------------------------------------------------------
  // Timer ueber einen Neustart - AP 1.2 / Architektur "Timer ohne Zeitchip"
  //
  // GESPEICHERT WIRD DAS ENDE, NICHT DIE RESTZEIT. Eine Restzeit wuerde jeder
  // Neustart verschieben; ein absoluter Zeitpunkt laesst sich mit der
  // NTP-Zeit wieder einordnen. Genau deshalb braucht es dafuer KEINEN
  // Zeitchip - der ist am 24.08.2026 gestrichen worden.
  //
  // Was bleibt: kommt das Netz nicht zurueck, gibt es keine Zeit und der
  // Timer laesst sich nicht einordnen. Bewusst in Kauf genommen.
  // -------------------------------------------------------------------------

  /// Gemerktes Timerende als Unixzeit, 0 = keins.
  uint32_t timer_ende_gemerkt() const { return timer_ende_epoch_; }

  /// Nach dem Neustart aufrufen, sobald die Zeit steht.
  /// @return  >0 = Timer mit so vielen Sekunden Rest wieder aufgenommen,
  ///           0 = nichts gemerkt,
  ///          -1 = gemerkter Timer ist waehrend der Auszeit abgelaufen.
  int32_t timer_wiederaufnehmen(uint32_t epoch_jetzt);
  void timer_abbrechen();
  bool timer_laeuft() const { return timer_phase_ != Timerphase::aus; }
  /// Was der Timer GERADE tut: "aus", "faehrt auf zwoelf", "laeuft",
  /// "Endfahrt". Die Anfahrt ist ein eigener Zustand, weil die Messung
  /// dabei noch nicht laeuft.
  const char *timer_zustand_text() const;

  std::string timer_restzeit() const;

  // -------------------------------------------------------------------------
  // Fahren von Hand
  // -------------------------------------------------------------------------

  // WARUM JEDER HANDEINGRIFF DEN UHRBETRIEB AUSSCHALTET (seit 23.08.2026):
  //
  // Vorher gewann "Hand" nur, SOLANGE die Fahrt lief - danach nahm die Uhr
  // den Zeiger wieder. Weil sie nur vorwaerts faehrt und nur 8 Schritte
  // Vorlauf abwartet (VORLAUF_TOLERANZ), wurde aus einem Nachstellen um 50
  // Schritte eine fast ganze Umdrehung: 3200 - 50 = 3150 Schritte vorwaerts.
  // Am Aufbau war damit KEIN Einmessen moeglich - der Zeiger lief nach jedem
  // Knopfdruck weg.
  //
  // Die Kollisionsregel selbst war nicht falsch, nur ungleich angewandt: ein
  // Timerlauf schaltet den Uhrbetrieb seit immer aus (`timer_start`), ein
  // Handeingriff tat es nicht. Jetzt tun es beide. Damit entfaellt die
  // Vorbedingung "erst m 0" - sie stand in keiner Anleitung und war die
  // eigentliche Huerde beim Einmessen.

  /// Fahrtempo der Handfahrten und Anfahrten, in Schritten/s.
  ///
  /// EINSTELLBAR und nicht fest: bis 23.08.2026 stand hier die Konstante
  /// g::TEMPO_FAHRT, und `fahre_auf` hat sie bei JEDEM Aufruf gesetzt. Ein
  /// 'f 9600' von aussen war damit wirkungslos, sobald der naechste Sprung
  /// kam - es sah aus wie ein Tempo, das man setzen kann, war aber keins.
  void fahrtempo(uint32_t schritte_je_s);
  /// 0 bedeutet: beginn() lief noch nicht.
  uint32_t fahrtempo() const { return fahrtempo_; }

  /// Zielposition von aussen. Faehrt IMMER vorwaerts - nie den kuerzeren Weg,
  /// siehe delta_vorwaerts in lib/zeiger.
  ///
  /// @param kreise zusaetzliche VOLLE Umdrehungen vor dem Ziel - der
  ///        Schwungeffekt. 0 = direkt anfahren.
  ///
  /// WARUM DAS KEIN EIGENER FAHRMODUS IST: `kreise` volle Umdrehungen plus der
  /// Restweg sind EINE einzige laengere Fahrt. FastAccelStepper legt darueber
  /// von sich aus ein Trapez - anfahren, halten, abbremsen - und landet
  /// schrittgenau. Ein eigener Effektmodus mit selbstgebauter Rampe waere
  /// mehr Code, ruckeliger und nicht schrittgenau.
  ///
  /// @return true, wenn dafuer ein laufender Uhrbetrieb beendet wurde.
  bool fahre_auf(int32_t position, int32_t kreise = 0);

  /// Schwungkreise, die ein Sprung von sich aus dreht. 0 = aus.
  void effekt_kreise(int32_t n);
  int32_t effekt_kreise() const { return effekt_kreise_; }

  // -------------------------------------------------------------------------
  // Einstellungen - AP 3.1, zwei NVS-Bereiche. Begruendung in betrieb.cpp.
  // -------------------------------------------------------------------------

  // -------------------------------------------------------------------------
  // GERAETEIDENTITAET - AP 6.2. Fertigungsbereich.
  //
  // WARUM JEDE UHR EIGENE ZUGANGSDATEN BRAUCHT: bis 24.08.2026 hatten ALLE
  // Geraete dasselbe OTA-Passwort ("starwatch") und denselben
  // Zugangspunkt-Schluessel ("lichtschwert") - beides im Quelltext. Die erste
  // Uhr, die jemand aufschraubt oder deren Repository er liest, oeffnet damit
  // alle anderen.
  //
  // NACHTRAEGLICH GEHT DAS NICHT MEHR: sind die Uhren einmal verteilt,
  // muesste jede einzelne wieder angefasst werden. Deshalb jetzt.
  //
  // DIE SERIENNUMMER WIRD ABGELEITET, NICHT GEWUERFELT. Sie kommt aus der
  // MAC-Adresse und ist damit an das Silizium gebunden: sie laesst sich nicht
  // verlieren, auch nicht durch ein geloeschtes NVS, und zwei Geraete koennen
  // nie dieselbe haben.
  //
  // DIE PASSWOERTER dagegen sind ZUFAELLIG (esp_random, Hardware-Generator)
  // und werden im Fertigungsbereich abgelegt - sie ueberleben Werksreset und
  // Update und wandern nie in einen Export.
  // -------------------------------------------------------------------------

  /// "A7F3" - vier Zeichen aus der MAC. Stabil ueber die Lebensdauer.
  const std::string &seriennummer() const { return seriennummer_; }

  /// "starwatch-A7F3"
  const std::string &geraetename() const { return geraetename_; }

  /// Passwort fuer das Entwickler-OTA. Beim ersten Start gewuerfelt.
  const std::string &ota_passwort() const { return ota_passwort_; }

  /// Schluessel des eigenen Zugangspunkts. Beim ersten Start gewuerfelt.
  const std::string &ap_passwort() const { return ap_passwort_; }

  /// GERAETETOKEN (AP 1.3). Beim ersten Start gewuerfelt, Fertigungsbereich.
  ///
  /// Es ist NICHT dasselbe wie das OTA-Passwort: jenes schuetzt das Einspielen
  /// von Firmware, dieses den Zugriff auf die Bedienung. Ein Angreifer, der
  /// nur das eine hat, bekommt nicht das andere.
  const std::string &token() const { return token_; }

  /// Alles auf einmal, zum Aufschreiben. AP 6.2 verlangt ein Geraeteblatt je
  /// Seriennummer - ohne das ist eine Uhr in zwei Jahren nicht mehr
  /// erreichbar.
  std::string geraeteblatt() const;

  // -------------------------------------------------------------------------
  // Zeitzone. Benutzerwert, ueberlebt den Neustart.
  // -------------------------------------------------------------------------

  /// POSIX-Zeitzone, z. B. "CET-1CEST,M3.5.0,M10.5.0/3". Leer = Vorgabe.
  const std::string &zeitzone() const { return zeitzone_; }
  void zeitzone(const std::string &tz);

  /// Kam die Zeit aus dem Netz (NTP) oder von Hand?
  bool zeit_aus_netz() const { return zeit_aus_netz_; }
  void zeit_aus_netz(bool ja) { zeit_aus_netz_ = ja; }

  /// Die BENUTZER-Werte in den NVS schreiben: Tempo, Rampe, Schwungkreise,
  /// Zeitmodus, Helligkeit.
  ///
  /// AUSDRUECKLICH und nicht in jedem Setter: der Zeitmodus wird auch
  /// INTERN abgeschaltet, sobald von Hand gefahren wird. Wuerde das
  /// mitgeschrieben, waere nach jedem Nachstellen "Uhr aus" der gemerkte
  /// Zustand. Gesichert wird also, was der Bediener setzt - nicht, was der
  /// Betrieb daraus macht.
  void werte_sichern();

  /// Benutzerbereich loeschen. Der Fertigungsbereich - und damit der
  /// Hall-Offset - bleibt.
  void werksreset();

  // -------------------------------------------------------------------------
  // Werksreset durch Stromzyklen - der Weg zurueck ohne Token.
  // Begruendung bei den Kennzahlen in geraet.h.
  // -------------------------------------------------------------------------

  /// Beim Start aufrufen. Zaehlt ein echtes Einschalten mit.
  /// @param war_einschalten true nur bei ESP_RST_POWERON.
  /// @return true, wenn die Zahl erreicht ist und zurueckgesetzt werden soll.
  bool zyklus_zaehlen(bool war_einschalten);

  /// Aus der Hauptschleife: loescht den Zaehler, sobald die Uhr laenger als
  /// RESET_FENSTER_MS laeuft.
  void zyklus_tick();

  /// Wie viele Zyklen bisher gezaehlt sind - fuer die Anzeige.
  uint32_t zyklen() const { return zyklen_; }

  /// Der aus dem NVS geladene Zeitmodus, der auf eine gueltige Uhrzeit
  /// wartet. Ohne Zeit gibt es keine Sollposition.
  Zeitmodus modus_nach_zeit() const { return modus_nach_zeit_; }

  // -------------------------------------------------------------------------
  // Rueckfall in den Uhrbetrieb
  //
  // Die Zeit ist der Normalzustand; alles andere ist eine Episode. Nach
  // g::RUECKFALL_MS ohne Handlung nimmt die Uhr den Zeiger zurueck.
  //
  // MIT EINER SPERRE, und die ist der Grund, dass es hier drei Funktionen
  // gibt statt einer: beim Einmessen wird von Hand gestellt und dann 'o!'
  // gedrueckt. Griffe die Uhr dazwischen zu, waere der Offset falsch.
  // `verstelle_um` und `hier_ist_zwoelf` sperren daher; ein Anfahren oder ein
  // 'm' gibt wieder frei.
  // -------------------------------------------------------------------------

  /// Die Frist neu anlaufen lassen.
  void handlung_gesehen();

  /// Rueckfall aussetzen - bis `rueckfall_freigeben()`.
  void rueckfall_sperren();
  void rueckfall_freigeben();
  bool rueckfall_gesperrt() const { return rueckfall_gesperrt_; }

  /// Ziel des Rueckfalls: der gespeicherte Zeitmodus. Steht er auf "aus",
  /// gibt es keinen Rueckfall - dann will der Bediener die Uhr aus.
  Zeitmodus rueckfall_modus() const { return rueckfall_modus_; }
  void rueckfall_modus(Zeitmodus m) { rueckfall_modus_ = m; }

  /// Einmal true, nachdem der Rueckfall gegriffen hat - damit die Konsole es
  /// melden kann, ohne dass Betrieb selbst schreibt.
  bool rueckfall_abholen() {
    const bool r = rueckfall_geschehen_;
    rueckfall_geschehen_ = false;
    return r;
  }

  /// Restzeit der Frist in ms, 0 wenn sie abgelaufen oder gesperrt ist.
  uint32_t rueckfall_rest_ms() const;

  /// Zeiger um Schritte verstellen. Fuer das Einmessen des Offsets; hier ist
  /// rueckwaerts erlaubt, weil von Hand nachgestellt wird.
  ///
  /// @return true, wenn dafuer ein laufender Uhrbetrieb beendet wurde.
  bool verstelle_um(int32_t schritte);

  /// Alles anhalten: Timer aus, Uhrbetrieb aus, Zeiger mit Rampe stoppen.
  void stopp();

  Fuehrung fuehrung() const;

  // -------------------------------------------------------------------------
  // Aus der Hauptschleife
  // -------------------------------------------------------------------------
  void tick();

 private:
  enum class Timerphase {
    aus,
    auf_zwoelf,  ///< faehrt die Startlage an
    laeuft,
    endfahrt,    ///< die letzten Schritte auf zwoelf
  };

  /// Nimmt die Uhr aus der Fuehrung. Wird von JEDEM Handeingriff gerufen -
  /// fahre_auf, verstelle_um, hier_ist_zwoelf.
  /// @return true, wenn dabei ein laufender Uhrbetrieb beendet wurde.
  bool hand_uebernimmt();

  void uhr_stellen();
  void timer_tick();

  Antrieb *antrieb_ = nullptr;
  Hall *hall_ = nullptr;
  Licht *licht_ = nullptr;

  // Zeitbasis. Unsigned-Differenz auf millis() ist ueber den Ueberlauf nach
  // 49 Tagen hinweg richtig, deshalb wird sie nie ausgeglichen.
  bool zeit_gueltig_ = false;
  int32_t basis_sek_ = 0;
  uint32_t basis_ms_ = 0;

  Zeitmodus zeitmodus_ = Zeitmodus::aus;
  uint32_t uhr_letzter_tick_ = 0;

  int32_t hall_offset_ = 0;
  // 0 heisst "noch nicht gesetzt". Den Startwert setzt beginn() aus geraet.h -
  // dieser Header bindet geraet.h bewusst NICHT ein, damit die Klasse nicht an
  // der Geraetekonfiguration haengt.
  uint32_t fahrtempo_ = 0;
  int32_t effekt_kreise_ = 0;
  Zeitmodus modus_nach_zeit_ = Zeitmodus::aus;
  Zeitmodus rueckfall_modus_ = Zeitmodus::aus;
  uint32_t letzte_handlung_ms_ = 0;
  bool rueckfall_gesperrt_ = false;
  bool rueckfall_geschehen_ = false;

  /// Licht-Zustand vor der Leerfahrt des Timers, um ihn danach
  /// wiederherzustellen.
  bool licht_vor_leerfahrt_ = false;
  bool licht_gemerkt_ = false;
  uint32_t timer_ende_epoch_ = 0;

  void timer_ende_merken(uint32_t epoch_ende);
  std::string zeitzone_;
  std::string seriennummer_;
  std::string geraetename_;
  std::string ota_passwort_;
  std::string ap_passwort_;
  std::string token_;
  uint32_t zyklen_ = 0;
  bool zyklen_geloescht_ = false;

  void identitaet_laden();
  bool zeit_aus_netz_ = false;

  void rueckfall_pruefen();
  bool homing_laeuft_ = false;
  Homingergebnis letztes_homing_;

  Timerphase timer_phase_ = Timerphase::aus;
  int32_t timer_start_pos_ = 0;
  uint32_t timer_ab_ms_ = 0;
  uint32_t timer_dauer_ms_ = 0;
  uint32_t timer_phase_ab_ms_ = 0;

  bool licht_war_an_ = false;
};

}  // namespace starwatch
