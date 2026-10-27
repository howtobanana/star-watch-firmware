// ---------------------------------------------------------------------------
// Star Watch - lib/licht: LED und Effekte. AP 2.5.
//
// ===========================================================================
// VERANTWORTUNG
// ===========================================================================
// Das Tastverhaeltnis am LD24AJTA (PT4115) auf GPIO33, LEDC. Der STROM steht
// am Trimmer des Moduls und ist keine Sache der Firmware - hier kommt nur die
// Helligkeit her. DIM ist active HIGH: unter 0,3 V aus, ueber 2,5 V voll.
//
// DREI Ausgaenge (IO6 rot, IO2 gruen, IO8 blau), jeder hinter
// einem AO3400A, der DIM nach Masse zieht - also HIGH = aus. Der Strom steht
// fest (0,68 Ohm = 294 mA je Kanal). Dazu eine FARBE als vierte Groesse: die
// Helligkeit bleibt EIN Wert in Prozent, die Farbe verteilt ihn auf die drei
// Kanaele (kanal_anteile() in lib/zeiger, host-getestet), und eine
// Leistungsgrenze von 2,85 W = 95 % der 3 W liegt darueber (geraet.h,
// LED_MAX_W). Reine Farben und Zweiermischungen bleiben darunter und laufen
// voll; nur Weiss wird zurueckgenommen.
//
// Die Effekte kennen keine Farbe. Sie sagen "100 %" oder "0 %", und die
// Klinge blitzt in der eingestellten Farbe - der Alarm soll gesehen werden,
// nicht umgefaerbt.
//
// Helligkeit nach aussen in PROZENT, nie als Rohwert des Timers. Die Kennlinie
// steht in lib/zeiger (tastverhaeltnis) und ist host-getestet.
//
// ===========================================================================
// DIE EINE REGEL, DIE JEDER EFFEKT EINHALTEN MUSS
// ===========================================================================
// Nach dem Effekt steht das Licht wieder auf dem EINGESTELLTEN Wert - auch
// wenn der Effekt abgebrochen wurde, auch wenn waehrenddessen ein neuer
// Sollwert kam, auch wenn die Referenzfahrt die Klinge zwischendurch
// ausgeschaltet hat.
//
// Der eingestellte Wert und der momentan ausgegebene sind deshalb ZWEI
// Groessen, nicht eine. Ein Effekt veraendert nur die zweite. Wer beide zu
// einer macht, verliert den Sollwert beim ersten Abbruch.
//
// ===========================================================================
// WARUM NICHT BLOCKIEREND
// ===========================================================================
// Im Pruefstand sind die Effekte Skripte mit `delay:`. Hier laufen sie als
// Schrittkette: `tick()` schaut nur, ob der naechste Schritt faellig ist.
// Der Grund ist der Stundeneffekt - er dauert 60 Sekunden, und solange darf
// weder die Konsole noch der Uhrbetrieb stehen.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

#include <zeiger.h>

namespace starwatch {

/// Welcher Effekt gerade laeuft.
enum class Effekt {
  keiner,
  zuenden,        ///< Lichtschwert: Zuendversuche, Ausfahren, heisse Farbe
  loeschen,       ///< Lichtschwert faehrt ein
  blende,         ///< schlichte Ueberblendung, wenn der Zuendeffekt aus ist
  stundeneffekt,  ///< volle Stunde, eine Minute auf 100 %
  alarm,          ///< Timer abgelaufen: zwoelf harte Blitze
  rampe,          ///< Messhilfe: 0..20 % in halben Prozent
  treppe,         ///< Messhilfe: 0..100 % in Zehnerschritten
  gleiten,        ///< Uebergang vom gezeigten Wert, beliebige Dauer (0.20.0)
  puls,           ///< endloses weiches Auf und Ab (0.20.0)
  blitz,          ///< flash aus Home Assistant (0.20.0)
};

const char *effekt_text(Effekt e);

class Licht {
 public:
  /// Gibt false zurueck, wenn LEDC sich nicht einrichten liess - dann bleibt
  /// die Klinge dunkel, und der Aufrufer MUSS es melden. Ohne diese Meldung
  /// sucht man den Fehler am LED-Treiber oder an der Verdrahtung.
  bool beginn();

  /// Steht der Ausgang?
  bool bereit() const { return bereit_; }

  // -------------------------------------------------------------------------
  // Der eingestellte Wert
  // -------------------------------------------------------------------------

  /// Sollhelligkeit in Prozent. 0 heisst wirklich aus.
  /// Ein laufender Effekt wird dadurch NICHT abgebrochen - er kehrt am Ende
  /// auf den neuen Wert zurueck.
  void soll_prozent(float prozent);
  float soll_prozent() const { return soll_; }

  /// Klinge an oder aus, ohne den Reglerwert zu verlieren. `an(false)` ist
  /// nicht dasselbe wie `soll_prozent(0)`.
  void an(bool ein);
  bool ist_an() const { return an_; }

  /// Was gerade wirklich ausgegeben wird, in Prozent.
  float ausgabe_prozent() const { return ausgabe_; }

  // -------------------------------------------------------------------------
  // Die Farbe
  // -------------------------------------------------------------------------

  /// Farbe als 0xRRGGBB. Wirkt sofort, auch waehrend eines Effekts: der
  /// Effekt besitzt die HELLIGKEIT, nicht die Farbe.
  /// `zuenden` = true: laeuft die Klinge, zuendet sie neu und gleitet dabei
  /// von der alten zur neuen Farbe (28.09.2026) - fuer Farbknoepfe, `lf`
  /// und Home Assistant. Die Farbregler setzen ohne Zuendung, sonst finge
  /// sie beim Ziehen dauernd von vorn an.
  void farbe(uint32_t rgb, bool zuenden = false);
  uint32_t farbe() const { return farbe_; }

  /// Farbe MIT VORRANG vor der eingestellten, z. B. Orange waehrend des
  /// Timers (27.09.2026). Die Einstellung bleibt unberuehrt und gilt sofort
  /// wieder, wenn der Vorrang endet; die Helligkeit bleibt beim Regler bzw.
  /// beim Effekt. Signale (Zustand, Meldung) stehen weiter darueber.
  void farbe_vorrang(bool an, uint32_t rgb = 0);
  bool farbe_vorrang_an() const { return vorrang_an_; }

  /// Geschaetzte Leistung der LED in Watt, nach der Begrenzung.
  float leistung_w() const { return leistung_w_; }

  // -------------------------------------------------------------------------
  // SIGNALE: die Klinge sagt etwas, statt nur zu leuchten (25.09.2026)
  // -------------------------------------------------------------------------
  //
  // Ein Signal uebernimmt Farbe UND Helligkeit, solange es steht - der
  // Reglerwert, die Farbe und der Ein-Zustand bleiben dabei unberuehrt und
  // gelten sofort wieder, wenn es endet. Dieselbe Trennung wie bei den
  // Effekten: das Signal besitzt die Ausgabe, nicht die Einstellung.
  //
  // ZWEI EBENEN, und der Vorrang ist fest:
  //
  //   zustand   was die Uhr gerade TUT (Neustart, Referenzfahrt, Update ...)
  //   (Effekt)  laufende Effektkette - vor allem der Timeralarm
  //   meldung   Meldung von aussen, z. B. "Pflanzen giessen" ueber MQTT
  //   (Regler)  der normale Betrieb
  //
  // Die Meldung steht UNTER den Effekten: ein ablaufender Timer muss zu
  // sehen sein, auch wenn die Klinge seit Stunden gelb ist. Danach kommt die
  // Meldung von selbst zurueck.
  //
  // Ein Signal leuchtet AUCH BEI AUSGESCHALTETER KLINGE - eine Erinnerung,
  // die nur bei eingeschaltetem Licht erscheint, erinnert an nichts.
  enum class Ebene : uint8_t { meldung = 0, zustand = 1 };

  /// Signal setzen. `hell` in Prozent (< 0: der Reglerwert, mindestens 20),
  /// `dauer_ms` 0 = bis signal_aus(). Ersetzt ein laufendes Signal derselben
  /// Ebene - es gibt je Ebene EINS, das letzte gewinnt.
  void signal(Ebene e, uint32_t farbe, Muster m, float hell, uint32_t dauer_ms);
  void signal_aus(Ebene e);

  /// Die Meldung RUHT, solange der Zeiger nur Weg macht - Referenzfahrt und
  /// Anfahrt des Timers. Dieselbe Regel wie fuer die Klinge: eine leuchtende
  /// Klinge, die uebers Blatt wischt, sagt etwas, das nicht stimmt. Die
  /// Meldung bleibt dabei bestehen und ihre Dauer laeuft weiter.
  void meldung_pausieren(bool ruht) {
    meldung_ruht_ = ruht;
    ausgeben();
  }

  struct Signalzustand {
    bool aktiv = false;
    uint32_t farbe = 0;
    Muster muster = Muster::leuchten;
    float hell = 0.0f;
    uint32_t rest_ms = 0;  ///< 0 = unbegrenzt (wenn aktiv)
  };
  Signalzustand signal_zustand(Ebene e) const;

  /// Was die Klinge in diesem Augenblick WIRKLICH zeigt - mit Signal und
  /// Effekt. Fuer das Zifferblatt der Oberflaeche.
  uint32_t zeige_farbe() const { return zeige_farbe_; }
  float zeige_prozent() const { return zeige_prozent_; }

  // -------------------------------------------------------------------------
  // Der Faktor der Lichtautomatik (VEML7700 an J8)
  // -------------------------------------------------------------------------

  /// Faktor auf den eingestellten Wert, 0 .. 1. 1 heisst: keine Automatik.
  ///
  /// EIN DRITTER WERT, UND DAS MIT ABSICHT. Die Automatik koennte auch
  /// einfach soll_prozent() nachfuehren - dann waere aber der von Hand
  /// eingestellte Wert nach der ersten Daemmerung ueberschrieben, und beim
  /// Abschalten der Automatik gaebe es nichts, wohin zurueckzukehren waere.
  /// Der Regler bleibt also der Regler; die Automatik legt einen Faktor
  /// darueber.
  ///
  /// WIRKT NICHT AUF EFFEKTE. Ein Effekt besitzt die Ausgabe, solange er
  /// laeuft, und gibt absolute Prozentwerte aus: die Messrampe soll messen,
  /// was sie sagt, und der Alarm soll gesehen werden. Nach dem Effekt kehrt
  /// die Ausgabe ueber zurueck() auf den gedimmten Wert zurueck - die Regel
  /// "nach dem Effekt steht das Licht wieder auf dem eingestellten Wert"
  /// bleibt damit erhalten.
  void dimmfaktor(float faktor);
  float dimmfaktor() const { return dimm_; }

  /// Untergrenze, ab der die LED glimmt (AP 0.5). Startwert aus geraet.h.
  void min_prozent(float p) { min_ = p; }
  float min_prozent() const { return min_; }

  /// Tastverhaeltnis direkt setzen, unter Umgehung der Kennlinie. Zum Messen
  /// der Kennlinie selbst - sonst misst man die eigene Rechnung.
  void pwm_direkt(float prozent);

  // -------------------------------------------------------------------------
  // Effekte
  // -------------------------------------------------------------------------

  void zuenden();
  /// Zuenden mit einer anderen Ausgangsfarbe - fuer den Farbwechsel.
  void zuenden_von(uint32_t alt);

  /// ZUEND- UND EINFAHREFFEKT an oder aus (28.09.2026) - gilt fuer ALLES:
  /// Einschalten, Farbwechsel, Nachtabschaltung. Aus heisst nicht hartes
  /// An/Aus, sondern eine schlichte Ueberblendung (blende_bild, 0,5 s).
  void effekte(bool an) { effekte_ = an; }
  bool effekte() const { return effekte_; }
  void loeschen();
  void stundeneffekt();
  /// Timerende: DREI schnelle Blitze, nicht zwoelf. Zwoelf waren ein Alarm,
  /// der die halbe Wohnung weckt; drei sagen dasselbe und sind vorbei, bevor
  /// die Uhr auf die Zeit zurueckwechselt.
  void alarm();

  /// Ein Blitz je Sekunde, `n` Sekunden lang - der Countdown vor dem Timer.
  /// Der Zeiger steht dabei auf zwoelf und wartet.
  void countdown(int sekunden);
  void rampe();
  void treppe();

  /// Laufenden Effekt abbrechen und auf den eingestellten Wert zurueck.
  void effekt_abbrechen();

  // -------------------------------------------------------------------------
  // UEBERGANG, PULS, BLITZ (0.20.0, 30.09.2026)
  // -------------------------------------------------------------------------
  // Alle drei beginnen bei dem, was die Klinge GERADE ZEIGT - Helligkeit und
  // Farbe, auch mitten in einem anderen Uebergang oder Effekt. Nie bei der
  // gespeicherten Grundfarbe: genau das liess die Klinge bis 0.19.8 weiss
  // beginnen, wenn Home Assistant Rot schickte.

  /// Neue Werte weich setzen. Fehlende Werte (an < 0, hell < 0, !farbe_da)
  /// bleiben. Dauer 0 = sofort. Beendet einen Puls: ohne neue Werte kehrt
  /// die Klinge auf den Zustand vor dem Puls zurueck.
  struct Weich {
    int an = -1;          ///< -1 bleibt, 0 aus, 1 an
    float hell = -1.0f;   ///< Reglerwert in Prozent, < 0 bleibt
    bool farbe_da = false;
    uint32_t farbe = 0;
  };
  void weich_setzen(const Weich &w, uint32_t dauer_ms);

  /// Puls starten oder im Lauf aendern (Phase bleibt). Ohne Farbe: die
  /// aktuelle; ohne Helligkeit (< 0): 100 %. Die Einstellungen der Klinge
  /// (Regler, Farbe) bleiben unberuehrt, die Lichtautomatik wirkt nicht.
  void puls_starten(bool farbe_da, uint32_t farbe, float max_prozent);
  void puls_beenden(uint32_t dauer_ms) { weich_setzen(Weich{}, dauer_ms); }
  bool puls_aktiv() const { return effekt_ == Effekt::puls; }
  uint32_t puls_farbe() const { return puls_farbe_; }
  float puls_max() const { return puls_max_; }
  void puls_einstellen(uint32_t periode_ms, float min_anteil);
  uint32_t puls_periode() const { return puls_periode_; }
  float puls_min() const { return puls_min_; }

  /// An/aus, wie es gespeichert werden soll: waehrend eines Pulses der
  /// Zustand DAVOR - sonst kaeme eine vorher ausgeschaltete Klinge nach einem
  /// Neustart mitten im Puls eingeschaltet hoch.
  bool an_dauerhaft() const { return puls_aktiv() ? puls_vor_an_ : an_; }

  /// n Blitze in der aktuellen Farbe, danach zurueck (flash: short 1, long 3).
  void blitzen(int n);

  Effekt effekt() const { return effekt_; }
  bool effekt_laeuft() const { return effekt_ != Effekt::keiner; }

  /// Aus der Hauptschleife aufrufen. Ohne diesen Aufruf laeuft kein Effekt.
  void tick();

 private:
  struct Schritt {
    float prozent;      ///< Helligkeit in diesem Schritt
    uint32_t dauer_ms;  ///< wie lange, bevor der naechste kommt
  };

  /// Hoechstzahl der Schritte einer Kette. Die LAENGSTE ist die Messrampe mit
  /// 40 Stufen (0 bis 20 % in halben Prozent), danach der Alarm mit 24 und
  /// der Zuendablauf mit 11.
  ///
  /// Stand hier zuerst auf 26 - dann brach die Rampe bei 13 % ab, weil die
  /// Schleife gegen MAX_SCHRITTE prueft. Eine Messhilfe, die stillschweigend
  /// vor dem gesuchten Bereich endet, ist schlimmer als keine: man haette die
  /// Glimmschwelle nicht gefunden und dem Treiber die Schuld gegeben.
  static constexpr size_t MAX_SCHRITTE = 41;

  void kette_starten(Effekt e);
  void ausgabe_setzen(float prozent);
  /// Schreibt die Kanaele aus `ausgabe_`. `direkt` umgeht die Kennlinie -
  /// nur fuer pwm_direkt(). Die Leistungsgrenze umgeht NICHTS.
  void kanaele_schreiben(bool direkt);
  /// Entscheidet nach dem Vorrang oben, WAS ausgegeben wird, und schreibt.
  void ausgeben();
  void schreiben(float prozent, uint32_t farbe, bool direkt);
  void zurueck();
  /// Ein Bild der Zuendung ausgeben (zuend_bild in lib/zeiger).
  void zuend_schritt();

  struct Signal {
    bool aktiv = false;
    uint32_t farbe = 0;
    Muster muster = Muster::leuchten;
    float hell = 0.0f;
    uint32_t start_ms = 0;
    uint32_t dauer_ms = 0;
  };
  Signal sig_[2];
  bool meldung_ruht_ = false;
  uint32_t sig_zuletzt_ms_ = 0;
  uint32_t zeige_farbe_ = 0;
  float zeige_prozent_ = 0.0f;

  static constexpr int KANAELE = 3;

  Schritt kette_[MAX_SCHRITTE];
  size_t anzahl_ = 0;
  size_t index_ = 0;
  uint32_t schritt_ab_ = 0;
  Effekt effekt_ = Effekt::keiner;

  float soll_ = 60.0f;   ///< Reglerwert, wie im Pruefstand
  float dimm_ = 1.0f;    ///< Faktor der Lichtautomatik, 1 = keine
  float min_ = 1.0f;
  float ausgabe_ = 0.0f;
  bool direkt_ = false;  ///< letzte Ausgabe kam aus pwm_direkt()
  float leistung_w_ = 0.0f;
  uint32_t farbe_ = 0xFF0000;
  uint32_t vorrang_farbe_ = 0;
  bool vorrang_an_ = false;
  // Zuendung (zeitgesteuert, keine Schrittkette).
  uint32_t zuend_alt_ = 0;
  uint32_t zuend_ab_ = 0;
  uint32_t zuend_zufall_ = 1;
  uint32_t zuend_letzt_ = 0;
  uint32_t effekt_farbe_ = 0;  ///< Farbe, die die Zuendung gerade zeigt
  bool effekte_ = true;

  // Was die Klinge zuletzt OHNE Signal gezeigt hat - der Startpunkt jedes
  // Uebergangs.
  uint32_t basis_farbe_ = 0;
  uint32_t gl_von_farbe_ = 0;
  float gl_von_prozent_ = 0.0f;
  uint32_t gl_ab_ = 0;
  uint32_t gl_dauer_ = 0;
  void gleit_schritt();

  uint32_t puls_farbe_ = 0xFF0000;
  uint32_t puls_von_farbe_ = 0;
  uint32_t puls_farb_ab_ = 0;
  float puls_max_ = 100.0f;
  float puls_min_ = 0.08f;
  uint32_t puls_periode_ = 2000;
  float puls_phase_ = 0.0f;
  uint32_t puls_letzt_ = 0;
  bool puls_vor_an_ = false;
  void puls_schritt();

  /// Die gerade gezeigte Farbe des Effekts oder der Einstellung.
  uint32_t gezeigte_farbe() const { return basis_farbe_; }
  float blende_von_ = 0.0f;
  float blende_nach_ = 1.0f;
  /// Schlichte Ueberblendung starten (Zuendeffekt aus).
  void blende_starten(float von, float nach, uint32_t alt);
  bool an_ = false;
  bool bereit_ = false;
};

}  // namespace starwatch
