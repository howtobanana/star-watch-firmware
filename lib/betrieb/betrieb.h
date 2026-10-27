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

/// Wie der Zeiger WAEHREND eines Timers laeuft (27.09.2026). Eine
/// Einstellung, gespeichert; sie gilt auch fuer einen laufenden Timer sofort.
enum class Timerlauf {
  gleitend,      ///< ein Zug mit gerechnetem Tempo, gleichmaessig
  sekundentakt,  ///< eine Sekunde stehen, dann springen - wie ein Quarzwerk
};

const char *timerlauf_text(Timerlauf l);

class Betrieb {
 public:
  void beginn(Antrieb *antrieb, Hall *hall, Licht *licht);

  // -------------------------------------------------------------------------
  // Zeit
  // -------------------------------------------------------------------------

  /// Uhrzeit stellen. Ab hier laeuft sie aus millis() weiter.
  void zeit_setzen(int stunde, int minute, int sekunde);
  bool zeit_gueltig() const { return zeit_gueltig_; }

  /// Kennt der Zeiger seine Lage? Nur dann darf die Uhr ihn fuehren.
  ///
  /// ===================================================================
  /// WARUM DAS EINE EIGENE GROESSE IST (24.09.2026)
  /// ===================================================================
  /// `soll_jetzt()` pruefte bis heute nur, ob eine ZEIT da ist. Ob der
  /// Zaehler ueberhaupt auf etwas Wirkliches zeigt, fragte niemand. Eine Uhr
  /// ohne gelungene Referenzfahrt fuhr damit los und zeigte eine Position,
  /// die aus dem Zaehlerstand beim Einschalten kam - also aus nichts.
  ///
  /// DAS IST DIE TEUERSTE ART VON FEHLER, die dieses Geraet kennt: sie sieht
  /// aus wie Betrieb. Ein stehender Zeiger faellt auf und wird gemeldet;
  /// ein falsch gehender wird geglaubt.
  bool referenz_gueltig() const { return referenz_gueltig_; }

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
  ///
  /// `anlass` landet im Homing-Verlauf (05.10.2026): JEDE Fahrt wird dort mit
  /// ihrer Abweichung zur vorigen Referenz festgehalten, im NVS, damit sie
  /// einen Neustart ueberlebt.
  Homingergebnis referenzfahrt(Hall::Pumpe pumpe, Hominganlass anlass);
  const Homingergebnis &letztes_homing() const { return letztes_homing_; }

  /// Abweichung der letzten Fahrt zur vorigen Referenz, in Schritten. Nur
  /// aussagekraeftig, wenn `letzte_vergleichbar()`: vorher galt eine
  /// Referenz, und die Endstufe war seitdem nie aus.
  int32_t letzte_abweichung() const { return letzte_abweichung_; }
  bool letzte_vergleichbar() const { return letzte_vergleichbar_; }

  /// Die letzten Referenzfahrten und Offsetaenderungen, neueste zuerst.
  const Homingverlauf &homingverlauf() const { return verlauf_; }
  /// Eine Zeile je Eintrag, fuer Konsole und Diagnose.
  std::string homingeintrag_text(const Homingeintrag &e) const;
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

  Timerlauf timerlauf() const { return timerlauf_; }
  void timerlauf(Timerlauf l);

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

  /// Ziel von aussen, laufend nachgeschoben - fuer das Steuern von Hand
  /// (Neigungssteuerung der App, 21.10.2026). Nimmt den KUERZEREN Weg und
  /// faehrt damit auch rueckwaerts; das ist hier richtig, waehrend es bei
  /// `fahre_auf` falsch waere.
  ///
  /// RUECKWAERTS KOSTET GENAUIGKEIT, aber nur vorruebergehend: das Riemenspiel
  /// von rund 7 Grad liegt nach einem Richtungswechsel auf der anderen Seite,
  /// der Zeiger haengt also bis zu 7 Grad hinter dem Zaehler (delta_vorwaerts
  /// in lib/zeiger erklaert, warum der Uhrbetrieb das nie tut). Die erste
  /// Vorwaertsfahrt nimmt das Spiel wieder auf, und der Uhrbetrieb faehrt
  /// immer vorwaerts - der Zaehler bleibt richtig, die Referenz gilt weiter.
  ///
  /// OHNE WEICHEN EINSATZ und ohne Mindestdauer, anders als `fahre_auf`:
  /// beides streckt kurze Spruenge, und genau das wirkt beim Steuern traege.
  ///
  /// @return true, wenn dafuer ein laufender Uhrbetrieb beendet wurde.
  bool steuere_auf(int32_t position);

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

  /// NAME DER UHR, vom Benutzer vergeben ("Wohnzimmer"). Leer = keiner.
  ///
  /// Nur zum ANZEIGEN: Weboberflaeche, Browsertab, Geraetename in Home
  /// Assistant. Alles, woran eine Verbindung haengt, bleibt bei
  /// geraetename() und seriennummer() - MQTT-Client, unique_id, Zugangspunkt,
  /// OTA. Ein Umbenennen darf keine Verbindung kosten.
  const std::string &alias() const { return alias_; }
  /// Prueft mit alias_pruefen(); false heisst abgelehnt, nichts geaendert.
  bool alias(const std::string &name);
  /// Der Name, den ein Mensch sehen soll: der Alias, sonst geraetename().
  const std::string &anzeigename() const {
    return alias_.empty() ? geraetename_ : alias_;
  }

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

  /// WEISS DIESER UHR als 0xRRGGBB (27.10.2026). Die drei LEDs streuen von
  /// Uhr zu Uhr: an C4E8 ist 255/215/100 weiss, an BE40 100/255/150. Liegt im
  /// Fertigungsbereich wie der Offset - es ist eine Eigenschaft der LED, keine
  /// Vorliebe, und ueberlebt deshalb den Werksreset.
  uint32_t weiss() const { return weiss_; }
  void weiss(uint32_t rgb);

  /// Rueckfall in den Uhrbetrieb SOFORT statt nach der Frist - nach dem
  /// Steuern aus der App ('hu'), wo der Zeiger sonst zehn Sekunden auf zwoelf
  /// stuende. Alle anderen Bedingungen von rueckfall_pruefen() gelten weiter.
  void rueckfall_sofort();

  /// Alles auf einmal, zum Aufschreiben. AP 6.2 verlangt ein Geraeteblatt je
  /// Seriennummer - ohne das ist eine Uhr in zwei Jahren nicht mehr
  /// erreichbar.
  std::string geraeteblatt() const;

  // -------------------------------------------------------------------------
  // Zeitzone. Benutzerwert, ueberlebt den Neustart.
  // -------------------------------------------------------------------------

  /// POSIX-Zeitzone, z. B. "CET-1CEST,M3.5.0,M10.5.0/3". Leer = Vorgabe.
  const std::string &zeitzone() const { return zeitzone_; }

  /// Lichtautomatik (27.09.2026): Einstellungen gespeichert, Nachtschalter
  /// hier und nicht in main.cpp, damit Oberflaeche und MQTT seinen Zustand
  /// zeigen koennen. Die Setzfunktion begrenzt, uebernimmt die Schwellen in
  /// den Nachtschalter und schreibt ins NVS.
  const Lichtautomatik &lichtautomatik() const { return automatik_; }
  void lichtautomatik_setzen(const Lichtautomatik &a);

  /// Puls-Periode (ms) und -Minimum (Anteil) setzen, begrenzen, speichern.
  /// Wirkt sofort und ohne Sprung auf einen laufenden Puls (0.20.0).
  void puls_setzen(uint32_t periode_ms, float min_anteil);

  /// Haltestrom setzen, begrenzen, speichern (01.10.2026).
  void haltestrom_setzen(uint16_t ma);
  Nachtschalter &nachtschalter() { return nachtschalter_; }
  void zeitzone(const std::string &tz);

  // -------------------------------------------------------------------------
  // MQTT-Broker. Benutzerwerte: sie ueberleben den Neustart und fallen mit
  // dem Werksreset. Das ist Absicht - eine weitergegebene Uhr darf nicht im
  // Broker des Vorbesitzers weiterreden.
  //
  // WARUM ES HIER EINEN PASSWORT-GETTER GIBT UND BEIM WLAN NICHT: das
  // WLAN-Passwort haelt der WiFi-Stack, niemand im Programm braucht es je
  // wieder. Das Broker-Passwort dagegen muss die Firmware bei jedem
  // Verbindungsaufbau selbst vorzeigen. Der Schutz liegt deshalb woanders -
  // es wird NICHT exportiert und NICHT in den Zustand gegeben.
  // -------------------------------------------------------------------------

  /// Soll ueberhaupt eine Verbindung versucht werden? Wer keinen Broker hat,
  /// soll auch keine Verbindungsversuche im Log stehen haben.
  bool mqtt_an() const { return mqtt_an_; }
  void mqtt_an(bool ja);

  const std::string &mqtt_host() const { return mqtt_host_; }
  void mqtt_host(const std::string &h);

  uint16_t mqtt_port() const { return mqtt_port_; }
  void mqtt_port(uint16_t p);

  const std::string &mqtt_benutzer() const { return mqtt_benutzer_; }
  void mqtt_benutzer(const std::string &b);

  const std::string &mqtt_passwort() const { return mqtt_passwort_; }
  void mqtt_passwort(const std::string &p);

  /// Eingeschaltet UND eine Adresse vorhanden. Ohne beides gibt es nichts zu
  /// versuchen.
  bool mqtt_bereit() const { return mqtt_an_ && !mqtt_host_.empty(); }

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

  /// ALS ERSTES im Start aufrufen, vor beginn() - braucht es nicht.
  /// @param war_einschalten true bei ESP_RST_POWERON und ESP_RST_BROWNOUT.
  /// @return true, wenn die Zahl erreicht ist und zurueckgesetzt werden soll.
  ///         Der Zaehler bleibt dann stehen, bis zyklen_loeschen() kommt.
  bool zyklus_zaehlen(bool war_einschalten);

  /// Nach dem ausgefuehrten Werksreset - und wenn die Uhr laenger als das
  /// Fenster laeuft.
  void zyklen_loeschen();

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
  // -------------------------------------------------------------------------
  // Effekte, die von selbst laufen
  // -------------------------------------------------------------------------
  // Beide sind EINSTELLUNGEN, keine Ausloeser: sie sagen, ob die Uhr den
  // jeweiligen Effekt selbst startet. Von Hand ausloesen geht weiter ueber
  // 'lz' und 'ls' - das ist Werkstattsache und bleibt davon unberuehrt.

  /// Zuendet die Klinge beim Einschalten? (Der Flackereffekt.)
  bool effekt_start() const { return effekt_start_; }
  void effekt_start(bool an);

  /// Laeuft zur vollen Stunde der Stundeneffekt?
  bool effekt_stunde() const { return effekt_stunde_; }
  void effekt_stunde(bool an);

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

  /// Auf zwoelf fahren und dort HALTEN, bis 'o!' bestaetigt (23.10.2026).
  /// Der Knopf "Auf zwoelf" beim Einmessen: nach 'u 12' griff der Uhrbetrieb
  /// nach zehn Sekunden wieder zu, bevor zwoelf bestaetigt war.
  bool auf_zwoelf_halten();

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
    countdown,   ///< steht auf zwoelf und blinkt drei Sekunden herunter
    laeuft,
    endfahrt,    ///< die letzten Schritte auf zwoelf
  };

  /// Nimmt die Uhr aus der Fuehrung. Wird von JEDEM Handeingriff gerufen -
  /// fahre_auf, verstelle_um, hier_ist_zwoelf.
  /// @return true, wenn dabei ein laufender Uhrbetrieb beendet wurde.
  bool hand_uebernimmt();

  void uhr_stellen();
  void timer_tick();
  /// Die zwei Laufarten der Phase `laeuft`. Beide fahren nur vorwaerts.
  void timer_gleiten(uint32_t verstrichen_ms);
  void timer_springen(uint32_t verstrichen_ms);

  Antrieb *antrieb_ = nullptr;
  Hall *hall_ = nullptr;
  Licht *licht_ = nullptr;

  // Zeitbasis. Unsigned-Differenz auf millis() ist ueber den Ueberlauf nach
  // 49 Tagen hinweg richtig, deshalb wird sie nie ausgeglichen.
  bool zeit_gueltig_ = false;
  bool referenz_gueltig_ = false;
  int32_t basis_sek_ = 0;
  uint32_t basis_ms_ = 0;

  Zeitmodus zeitmodus_ = Zeitmodus::aus;
  uint32_t uhr_letzter_tick_ = 0;

  int32_t hall_offset_ = 0;
  uint32_t weiss_ = 0xFFD764;
  // 0 heisst "noch nicht gesetzt". Den Startwert setzt beginn() aus geraet.h -
  // dieser Header bindet geraet.h bewusst NICHT ein, damit die Klasse nicht an
  // der Geraetekonfiguration haengt.
  uint32_t fahrtempo_ = 0;
  int32_t effekt_kreise_ = 0;
  Zeitmodus modus_nach_zeit_ = Zeitmodus::aus;
  Zeitmodus rueckfall_modus_ = Zeitmodus::aus;
  /// Wie lange die Uhr nach einem Handeingriff wartet. Nach einem Timer
  /// kuerzer - siehe RUECKFALL_TIMER_MS.
  ///
  /// DER WERT STEHT HIER ALS ZAHL, weil dieser Header `geraet.h` bewusst
  /// nicht einbindet. Ein static_assert in betrieb.cpp haelt ihn mit
  /// g::RUECKFALL_MS zusammen - auseinanderlaufen koennen sie also nicht.
  uint32_t rueckfall_frist_ms_ = 10000;
  bool effekt_start_ = true;
  bool effekt_stunde_ = true;
  /// Die Stunde, in der der Stundeneffekt zuletzt lief. Ohne das liefe er
  /// eine Minute lang immer wieder neu an - der Takt ist schneller als der
  /// Effekt. Dieselbe Falle wie beim taeglichen Homing, siehe main.cpp.
  int effekt_letzte_stunde_ = -1;
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
  Lichtautomatik automatik_;
  Nachtschalter nachtschalter_{0.5f, 3.0f, 60000};
  bool mqtt_an_ = false;
  std::string mqtt_host_;
  uint16_t mqtt_port_ = 1883;  // g::MQTT_PORT_VORGABE, siehe static_assert
  std::string mqtt_benutzer_;
  std::string mqtt_passwort_;
  std::string seriennummer_;
  std::string geraetename_;
  std::string alias_;
  std::string ota_passwort_;
  std::string ap_passwort_;
  std::string token_;
  uint32_t zyklen_ = 0;
  bool zyklen_geloescht_ = false;

  void identitaet_laden();
  bool zeit_aus_netz_ = false;

  void rueckfall_pruefen();
  void stundeneffekt_pruefen();
  bool homing_laeuft_ = false;
  Homingergebnis letztes_homing_;

  // Homing-Verlauf (05.10.2026)
  Homingverlauf verlauf_;
  int32_t letzte_abweichung_ = 0;
  bool letzte_vergleichbar_ = false;
  /// Freilaeufe der Endstufe beim Setzen der Referenz - aendert sich die
  /// Zahl, ist die naechste Abweichung kein Schrittverlust.
  uint32_t freilaeufe_bei_referenz_ = 0;
  /// `o <n>` verschiebt den Offset, ohne den Zaehler mitzunehmen: die
  /// naechste Abweichung misst dann die Verschiebung, keinen Verlust.
  bool offset_von_hand_ = false;
  void verlauf_eintragen(Hominganlass anlass, const Homingergebnis *e,
                         bool vergleichbar, int32_t abweichung);
  void offset_setzen(int32_t schritte);

  Timerphase timer_phase_ = Timerphase::aus;
  int32_t timer_start_pos_ = 0;
  uint32_t timer_ab_ms_ = 0;
  uint32_t timer_dauer_ms_ = 0;
  uint32_t timer_phase_ab_ms_ = 0;

  Timerlauf timerlauf_ = Timerlauf::gleitend;
  /// Steht gerade EINE Fahrt bis zum Ende mit gerechnetem Tempo an? Faellt
  /// beim Umschalten auf den Sekundentakt und am Ende jedes Laufs.
  bool timer_gleitet_ = false;
  uint32_t timer_gleit_mhz_ = 0;
  uint32_t timer_nachgefuehrt_ms_ = 0;
  /// Die Sekunde, fuer die zuletzt gesprungen wurde.
  uint32_t timer_sekunde_ = 0;

  bool licht_war_an_ = false;
};

}  // namespace starwatch
