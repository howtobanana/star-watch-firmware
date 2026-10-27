// ---------------------------------------------------------------------------
// Host-Tests der Geraetedaten. Prueft NICHT, ob die Zahlen richtig gemessen
// sind - das kann kein Test - sondern ob die ABLEITUNGEN zusammenpassen und
// ob geraet.h und zeiger.h dasselbe Bild von einer Umdrehung haben.
//
// Der Grund fuer diese Datei: seit AP 2.1 steht MIKROSCHRITTE an genau einer
// Stelle, und AP 1.1 wird sie voraussichtlich von 16 auf 64 aendern. Diese
// Tests sagen dann, ob das gereicht hat.
//
//   ~/.platformio/penv/bin/pio test -e native
// ---------------------------------------------------------------------------
#include <unity.h>

#include <cmath>

#include "geraet.h"
#include "zeiger.h"

namespace g = starwatch::geraet;

// --- Die Ableitungen ------------------------------------------------------

void test_rev_ist_gerechnet_nicht_getippt() {
  TEST_ASSERT_EQUAL_INT32(g::VOLLSCHRITTE * g::MIKROSCHRITTE, g::REV);
}

void test_zwoelf_stunden_ergeben_eine_umdrehung() {
  // Der Kern der ganzen Uhr: 12 h mal Schritte je Sekunde = eine Umdrehung.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 43200.0f,
                           g::SEK_JE_SCHRITT * static_cast<float>(g::REV));
}

void test_eine_umdrehung_sind_volle_360_grad() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 360.0f,
                           g::GRAD_JE_SCHRITT * static_cast<float>(g::REV));
}

void test_die_werte_von_1_16_stehen_noch() {
  // Faellt dieser Test, ist AP 1.1 entschieden worden. Dann sind hier die
  // neuen Zahlen einzutragen - und die 3200 im Briefing Abschnitt 4, in der
  // AI-Kurzanweisung Abschnitt 4 und im Aenderungsblatt.
  TEST_ASSERT_EQUAL_INT32(16, g::MIKROSCHRITTE);
  TEST_ASSERT_EQUAL_INT32(3200, g::REV);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.5f, g::SEK_JE_SCHRITT);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.1125f, g::GRAD_JE_SCHRITT);
}

// --- geraet.h und zeiger.h muessen dasselbe meinen ------------------------

void test_zwoelf_uhr_ist_schritt_null() {
  TEST_ASSERT_EQUAL_INT32(
      0, starwatch::soll_position(0, g::REV, g::SEK_JE_SCHRITT));
}

void test_sechs_uhr_ist_die_halbe_umdrehung() {
  TEST_ASSERT_EQUAL_INT32(
      g::REV / 2,
      starwatch::soll_position(6 * 3600, g::REV, g::SEK_JE_SCHRITT));
}

void test_ein_schritt_vor_zwoelf_ist_noch_nicht_zwoelf() {
  // Die letzte Position vor dem Umlauf. Faellt sie auf 0, laeuft die Uhr eine
  // Umdrehung zu weit.
  const int32_t pos = starwatch::soll_position(
      static_cast<int32_t>(43200.0f - g::SEK_JE_SCHRITT), g::REV,
      g::SEK_JE_SCHRITT);
  TEST_ASSERT_EQUAL_INT32(g::REV - 1, pos);
}

void test_der_tag_laeuft_zweimal_durch() {
  // 24 h auf einem Zwoelferblatt: um 15:00 steht der Zeiger wie um 03:00.
  TEST_ASSERT_EQUAL_INT32(
      starwatch::soll_position(3 * 3600, g::REV, g::SEK_JE_SCHRITT),
      starwatch::soll_position(15 * 3600, g::REV, g::SEK_JE_SCHRITT));
}

// --- Grenzwerte, die eine Messung spaeter fuellt --------------------------

void test_rueckzug_ist_gross_genug_fuer_ein_plausibles_fenster() {
  // AP 0.1 misst die Fensterbreite. Bis dahin nur die Untergrenze: ein
  // Rueckzug unter einem Grad Zeigerwinkel kann kein Fenster ueberstreichen.
  TEST_ASSERT_TRUE(g::HALL_RUECKZUG_SCHRITTE >
                   static_cast<int32_t>(1.0f / g::GRAD_JE_SCHRITT));
}

void test_aussteuerungsfenster_liegt_im_messbereich() {
  // Frueher: der SS49E lief an 3,3 V, und eine Auslenkung groesser als die
  // halbe Versorgung war physikalisch nicht messbar. Jetzt ist die
  // Grenze der Messbereich des TMAG5273 - darueber ist die Feldkurve
  // abgeschnitten, und ein abgeschnittener Gipfel hat keinen Schwerpunkt.
  TEST_ASSERT_TRUE(g::HALL_SPANNE_MAX_MT < g::TMAG_BEREICH_MT);
  TEST_ASSERT_TRUE(g::HALL_SPANNE_MIN_MT > 0.0f);
  TEST_ASSERT_TRUE(g::HALL_SPANNE_MIN_MT < g::HALL_SPANNE_MAX_MT);
}

void test_grobschwelle_liegt_ueber_dem_erdfeld() {
  // Rund 0,05 mT hat das Erdfeld. Eine Grobsuche, die darauf anspringt,
  // bleibt an einer beliebigen Stelle stehen - und der Rueckzug rechnet
  // dann von einem Ort, an dem kein Magnet ist.
  TEST_ASSERT_TRUE(g::HALL_GROB_SCHWELLE_MT > 0.2f);
  TEST_ASSERT_TRUE(g::HALL_GROB_SCHWELLE_MT < g::HALL_SPANNE_MIN_MT);
}

void test_der_messbereich_gehoert_zu_einer_wirklichen_variante() {
  // Der TMAG5273 kennt genau vier Bereiche. Steht hier etwas anderes, ist
  // eine Zahl getippt statt aus dem Datenblatt genommen.
  const bool bekannt = g::TMAG_BEREICH_MT == 40.0f || g::TMAG_BEREICH_MT == 80.0f ||
                       g::TMAG_BEREICH_MT == 133.0f || g::TMAG_BEREICH_MT == 266.0f;
  TEST_ASSERT_TRUE(bekannt);
}

void test_die_nachtschwellen_haben_hysterese() {
  // Ohne Abstand zwischen den beiden Schwellen schaltet die Klinge in der
  // Daemmerung im Minutentakt.
  TEST_ASSERT_TRUE(g::LUX_NACHT_AUS < g::LUX_NACHT_AN);
  TEST_ASSERT_TRUE(g::LUX_NACHT_AUS > 0.0f);
  TEST_ASSERT_TRUE(g::LUX_NACHT_AN <= g::LUX_HELL);
}

void test_die_lux_spanne_ist_logarithmierbar() {
  TEST_ASSERT_TRUE(g::LUX_DUNKEL > 0.0f);
  TEST_ASSERT_TRUE(g::LUX_HELL > g::LUX_DUNKEL);
  TEST_ASSERT_TRUE(g::LUX_ANTEIL_MIN > 0.0f && g::LUX_ANTEIL_MIN <= 1.0f);
}

void test_lux_aufloesung_trifft_die_groesste_messbare_helligkeit() {
  // Probe auf die eine Zahl, die aus dem Datenblatt kommt: bei x1/8 und
  // 25 ms ist die Aufloesung 32 * 16 mal so grob wie bei x2 und 800 ms.
  // Mal 65535 Zaehlschritten muss das die 140 000 lx ergeben, die Vishay
  // als groesste messbare Beleuchtungsstaerke nennt.
  const float grob = g::LUX_AUFLOESUNG_BASIS * 32.0f * 16.0f;
  TEST_ASSERT_FLOAT_WITHIN(2000.0f, 140000.0f, grob * 65535.0f);
}

void test_haltestrom_ist_nie_null() {
  // Ohne Haltestrom kippt die Klinge aus ihrer Lage. Der static_assert in
  // geraet.h faengt das beim Uebersetzen; dieser Test macht die Absicht
  // sichtbar, damit sie niemand "aufraeumt".
  TEST_ASSERT_GREATER_THAN_UINT16(0, g::STROM_HALT);
  TEST_ASSERT_LESS_OR_EQUAL_UINT16(g::STROM_LAUF, g::STROM_HALT);
}

void test_led_untergrenze_ist_ein_prozentwert() {
  TEST_ASSERT_TRUE(g::LED_MIN_PROZENT >= 0.0f && g::LED_MIN_PROZENT < 100.0f);
}

// --- Treiberstrom gegen R_SENSE (Arbeitsliste Firmware-Port 2.3 und 8) ----

/// Strom in mA RMS bei Stromstufe CS, nach der Formel, die TMCStepper selbst
/// benutzt: (CS+1)/32 * 0,325 V / (sqrt2 * (R_SENSE + 0,02 Ohm)).
static float strom_bei_cs(int cs) {
  return (cs + 1) / 32.0f * 0.325f / (std::sqrt(2.0f) * (g::R_SENSE + 0.02f)) *
         1000.0f;
}

/// Stromstufe fuer einen Wunschstrom, GENAU wie TMCStepper 0.7.3 sie
/// rechnet (TMCStepper.cpp, rms_current): abgeschnitten, nicht gerundet.
/// 400 mA an 0,47 Ohm ergeben so CS 26 = 396 mA, nicht CS 27.
static int cs_fuer(float ma) {
  return static_cast<int>(32.0f * 1.41421f * ma / 1000.0f * (g::R_SENSE + 0.02f) /
                              0.325f -
                          1.0f);
}

void test_laufstrom_liegt_mit_abstand_unter_dem_vollausschlag() {
  // Faellt dieser Test, gibt R_SENSE den Laufstrom nicht her: der Treiber
  // klemmt dann still auf CS 31, und der Motor bekommt weniger als verlangt.
  TEST_ASSERT_TRUE(static_cast<float>(g::STROM_LAUF) <= 0.9f * strom_bei_cs(31));
}

void test_strom_liegt_im_fenster_des_datenblatts() {
  // TMC2209 Datenblatt S. 25: "normal IRUN is 16 to 31 for best microstep
  // performance" - das Fenster gilt fuer den LAUFstrom. Der Haltestrom ist
  // seit 01.10.2026 150 mA (CS 9, an C4E8 gemessen, geraet.h); fuer IHOLD
  // nennt das Datenblatt kein Fenster.
  TEST_ASSERT_EQUAL_INT(26, cs_fuer(g::STROM_LAUF));
  TEST_ASSERT_EQUAL_INT(9, cs_fuer(g::STROM_HALT));
}

// --- Waermegrenze der RGB-LED (Vorgabe 25.09.2026, 27.10.2026 auf 95 %) -----------------------

void test_die_led_bekommt_nie_mehr_als_95_prozent_ihrer_drei_watt() {
  // Jede Farbe, die es gibt, bei 100 % am Regler: nie ueber 2,85 W. Geprueft
  // an allen Ecken des Farbwuerfels - dort liegt das Maximum.
  const uint32_t ecken[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
                            0xFF00FF, 0x00FFFF, 0xFFFFFF};
  for (uint32_t f : ecken) {
    float k[3];
    const float w = starwatch::kanal_anteile(
        starwatch::tastverhaeltnis(100.0f, g::LED_MIN_PROZENT), f,
        g::LED_WATT_VOLL, g::LED_MAX_W, k);
    TEST_ASSERT_TRUE(w <= 0.95f * 3.0f + 1e-4f);
    float echt = 0.0f;
    for (int i = 0; i < 3; i++)
      echt += k[i] * g::LED_WATT_VOLL[i];
    TEST_ASSERT_TRUE(echt <= 0.95f * 3.0f + 1e-4f);
  }
}

void test_rot_bei_hundert_prozent_ist_volle_leistung() {
  // "100 % Rot sind unter 1 W" - also volle 100 %, keine Drosselung.
  float k[3];
  const float w = starwatch::kanal_anteile(1.0f, 0xFF0000, g::LED_WATT_VOLL,
                                           g::LED_MAX_W, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[0]);
  TEST_ASSERT_TRUE(w < 1.0f);
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_rev_ist_gerechnet_nicht_getippt);
  RUN_TEST(test_zwoelf_stunden_ergeben_eine_umdrehung);
  RUN_TEST(test_eine_umdrehung_sind_volle_360_grad);
  RUN_TEST(test_die_werte_von_1_16_stehen_noch);

  RUN_TEST(test_zwoelf_uhr_ist_schritt_null);
  RUN_TEST(test_sechs_uhr_ist_die_halbe_umdrehung);
  RUN_TEST(test_ein_schritt_vor_zwoelf_ist_noch_nicht_zwoelf);
  RUN_TEST(test_der_tag_laeuft_zweimal_durch);

  RUN_TEST(test_rueckzug_ist_gross_genug_fuer_ein_plausibles_fenster);
  RUN_TEST(test_aussteuerungsfenster_liegt_im_messbereich);
  RUN_TEST(test_grobschwelle_liegt_ueber_dem_erdfeld);
  RUN_TEST(test_der_messbereich_gehoert_zu_einer_wirklichen_variante);
  RUN_TEST(test_die_nachtschwellen_haben_hysterese);
  RUN_TEST(test_die_lux_spanne_ist_logarithmierbar);
  RUN_TEST(test_lux_aufloesung_trifft_die_groesste_messbare_helligkeit);
  RUN_TEST(test_haltestrom_ist_nie_null);
  RUN_TEST(test_led_untergrenze_ist_ein_prozentwert);

  RUN_TEST(test_laufstrom_liegt_mit_abstand_unter_dem_vollausschlag);
  RUN_TEST(test_strom_liegt_im_fenster_des_datenblatts);
  RUN_TEST(test_die_led_bekommt_nie_mehr_als_95_prozent_ihrer_drei_watt);
  RUN_TEST(test_rot_bei_hundert_prozent_ist_volle_leistung);
  return UNITY_END();
}
