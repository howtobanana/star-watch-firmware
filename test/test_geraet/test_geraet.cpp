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

void test_aussteuerungsfenster_liegt_innerhalb_der_versorgung() {
  // Der SS49E laeuft an 3,3 V, seine Ruhelinie liegt in der Mitte. Eine
  // Auslenkung groesser als die halbe Versorgung ist physikalisch nicht
  // messbar - die Kurve waere dann abgeschnitten.
  TEST_ASSERT_TRUE(g::HALL_SPANNE_MAX_V < g::HALL_RUHE_V);
  TEST_ASSERT_TRUE(g::HALL_SPANNE_MIN_V > 0.0f);
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
  RUN_TEST(test_aussteuerungsfenster_liegt_innerhalb_der_versorgung);
  RUN_TEST(test_haltestrom_ist_nie_null);
  RUN_TEST(test_led_untergrenze_ist_ein_prozentwert);

  return UNITY_END();
}
