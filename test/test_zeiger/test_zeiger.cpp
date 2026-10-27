// ---------------------------------------------------------------------------
// Host-Tests der Rechenteile. Laufen ohne Hardware:  pio test -e native
//
// Die Faelle sind nicht ausgedacht. Jeder Block deckt eine Stelle ab, an der
// in der ESPHome-Fassung schon einmal etwas falsch war oder an der es sehr
// leicht falsch wird - Vorzeichen, Umlauf ueber zwoelf Uhr, Rueckwaertsfahrt.
// ---------------------------------------------------------------------------
#include <unity.h>

#include <cmath>
#include <cstring>

#include "zeiger.h"

using namespace starwatch;

namespace {
constexpr int32_t REV = 3200;      // Mikroschritte je 12 h, 1/16 und Riemen 1:1
constexpr float SEK_JE_SCHRITT = 13.5f;  // 43200 / 3200
}  // namespace

void setUp() {}
void tearDown() {}

// --- normiere --------------------------------------------------------------
// Der %-Operator liefert bei negativem Zaehler ein negatives Ergebnis. Genau
// daran scheitert jede handgeschriebene Modulo-Rechnung zuerst.
void test_normiere_faltet_auch_negative() {
  TEST_ASSERT_EQUAL_INT32(0, normiere(0, REV));
  TEST_ASSERT_EQUAL_INT32(1, normiere(1, REV));
  TEST_ASSERT_EQUAL_INT32(0, normiere(REV, REV));
  TEST_ASSERT_EQUAL_INT32(1, normiere(REV + 1, REV));
  TEST_ASSERT_EQUAL_INT32(REV - 1, normiere(-1, REV));
  TEST_ASSERT_EQUAL_INT32(REV - 1, normiere(-REV - 1, REV));
}

// --- soll_position ---------------------------------------------------------
void test_sollposition_volle_stunden() {
  TEST_ASSERT_EQUAL_INT32(0, soll_position(0, REV, SEK_JE_SCHRITT));          // 0:00
  TEST_ASSERT_EQUAL_INT32(800, soll_position(3 * 3600, REV, SEK_JE_SCHRITT));   // 3:00
  TEST_ASSERT_EQUAL_INT32(1600, soll_position(6 * 3600, REV, SEK_JE_SCHRITT));  // 6:00
  TEST_ASSERT_EQUAL_INT32(2400, soll_position(9 * 3600, REV, SEK_JE_SCHRITT));  // 9:00
}

void test_sollposition_nachmittag_ist_dieselbe_stelle() {
  // Das Blatt hat zwoelf Stunden: 15:32 steht dort, wo auch 3:32 steht.
  const int32_t nachmittag = soll_position(15 * 3600 + 32 * 60, REV, SEK_JE_SCHRITT);
  const int32_t vormittag = soll_position(3 * 3600 + 32 * 60, REV, SEK_JE_SCHRITT);
  TEST_ASSERT_EQUAL_INT32(vormittag, nachmittag);
  TEST_ASSERT_EQUAL_INT32(942, nachmittag);  // 12720 s / 13,5 = 942,2 -> 942
}

void test_sollposition_kurz_vor_zwoelf_laeuft_auf_null_um() {
  // 43199 s / 13,5 = 3199,93 -> gerundet 3200 -> und das ist wieder 0.
  // Ohne das Modulo stuende hier ein Ziel ausserhalb der Umdrehung.
  TEST_ASSERT_EQUAL_INT32(0, soll_position(43199, REV, SEK_JE_SCHRITT));
  TEST_ASSERT_EQUAL_INT32(0, soll_position(43200, REV, SEK_JE_SCHRITT));
}

// --- delta_vorwaerts -------------------------------------------------------
void test_delta_geht_nie_rueckwaerts() {
  TEST_ASSERT_EQUAL_INT32(800, delta_vorwaerts(0, 800, REV));
  TEST_ASSERT_EQUAL_INT32(0, delta_vorwaerts(800, 800, REV));
  // Von 9 Uhr auf 12 Uhr sind es 800 Schritte VORWAERTS, nicht -2400.
  TEST_ASSERT_EQUAL_INT32(800, delta_vorwaerts(2400, 0, REV));
  // Ein einziger Schritt ueber zwoelf hinweg.
  TEST_ASSERT_EQUAL_INT32(1, delta_vorwaerts(REV - 1, 0, REV));
}

void test_delta_vertraegt_positionen_ausserhalb_der_umdrehung() {
  // current_position waechst im Betrieb ueber rev hinaus - der Zaehler wird
  // nicht zurueckgesetzt. Das darf am Ergebnis nichts aendern.
  TEST_ASSERT_EQUAL_INT32(800, delta_vorwaerts(10 * REV + 2400, 0, REV));
}

// --- warten_statt_umrunden -------------------------------------------------
void test_vorlauf_wird_ausgesessen() {
  const int32_t TOL = 8;
  // Zeiger steht 3 Schritte vor der Zeit: delta waere 3197, fast eine Runde.
  TEST_ASSERT_TRUE(warten_statt_umrunden(REV - 3, REV, TOL));
  // Normale Stellfahrt bleibt eine Stellfahrt.
  TEST_ASSERT_FALSE(warten_statt_umrunden(1, REV, TOL));
  TEST_ASSERT_FALSE(warten_statt_umrunden(REV / 2, REV, TOL));
  // Genau auf der Toleranzgrenze wird noch gefahren, nicht gewartet.
  TEST_ASSERT_FALSE(warten_statt_umrunden(REV - TOL, REV, TOL));
}

// --- drift -----------------------------------------------------------------
void test_drift_hat_ein_vorzeichen() {
  TEST_ASSERT_EQUAL_INT32(4, drift(4, 0, REV));     // vier Schritte zu weit
  TEST_ASSERT_EQUAL_INT32(-4, drift(REV - 4, 0, REV));  // vier Schritte zu wenig
  TEST_ASSERT_EQUAL_INT32(0, drift(0, 0, REV));
  TEST_ASSERT_EQUAL_INT32(0, drift(REV, 0, REV));
}

// So rechnet Betrieb::steuere_auf: das Vorzeichen aus drift, angewandt auf den
// FORTLAUFENDEN Zaehler. Hier sitzt der Fehler, den man nicht sieht - ein
// Umlauf, der als ganze Umdrehung rueckwaerts herauskommt statt als ein
// Schritt vorwaerts. Am Gerät waere das ein Zeiger, der beim Steuern ueber
// zwoelf hinweg eine Runde dreht.
void test_steuern_nimmt_den_kuerzeren_weg() {
  auto schritt = [](int32_t ziel, int32_t wunsch) { return ziel + drift(wunsch, ziel, REV); };
  TEST_ASSERT_EQUAL_INT32(REV, schritt(REV - 1, 0));        // 3199 -> 0: einen vor
  TEST_ASSERT_EQUAL_INT32(REV - 1, schritt(REV, REV - 1));  // 0 -> 3199: einen zurueck
  TEST_ASSERT_EQUAL_INT32(800, schritt(0, 800));            // Viertel vor
  TEST_ASSERT_EQUAL_INT32(-800, schritt(0, REV - 800));     // Viertel ZURUECK, nicht 3/4 vor
  TEST_ASSERT_EQUAL_INT32(6480, schritt(6400, 80));         // zwei Umdrehungen weitergezaehlt
}

void test_drift_rechnet_gegen_einen_offset() {
  // Kalibrierte Uhr: die Referenzlage liegt nicht bei 0, sondern bei 120.
  TEST_ASSERT_EQUAL_INT32(0, drift(120, 120, REV));
  TEST_ASSERT_EQUAL_INT32(5, drift(125, 120, REV));
  TEST_ASSERT_EQUAL_INT32(-5, drift(115, 120, REV));
  // ... auch wenn der Durchgang ueber die Zwoelf faellt.
  TEST_ASSERT_EQUAL_INT32(-5, drift(REV - 5, 0, REV));
}

// --- schwerpunkt -----------------------------------------------------------
void test_schwerpunkt_findet_die_mitte_einer_symmetrischen_kurve() {
  const int32_t pos[] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
  const float v[] = {1.65f, 1.70f, 1.85f, 2.05f, 2.20f, 2.25f,
                     2.20f, 2.05f, 1.85f, 1.70f, 1.65f};
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt(pos, v, 11, 1.65f, 0.05f, &mitte));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, mitte);
}

void test_schwerpunkt_rechnet_mit_dem_betrag() {
  // Derselbe Magnet, falsch herum eingeklebt: die Kurve geht nach UNTEN.
  // Das Ergebnis muss dasselbe sein - die .scad laesst das ausdruecklich zu.
  const int32_t pos[] = {10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
  const float v[] = {1.65f, 1.60f, 1.45f, 1.25f, 1.10f, 1.05f,
                     1.10f, 1.25f, 1.45f, 1.60f, 1.65f};
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt(pos, v, 11, 1.65f, 0.05f, &mitte));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, mitte);
}

void test_schwerpunkt_meldet_fehlendes_feld() {
  // Nur Rauschen um die Ruhespannung: kein Feld gesehen, keine Referenz.
  const int32_t pos[] = {10, 11, 12, 13, 14};
  const float v[] = {1.65f, 1.66f, 1.64f, 1.67f, 1.63f};
  float mitte = -1.0f;
  TEST_ASSERT_FALSE(schwerpunkt(pos, v, 5, 1.65f, 0.05f, &mitte));
}

void test_schwerpunkt_ist_gegen_eine_schiefe_flanke_robust() {
  // Gipfel bei 15, aber die rechte Flanke traegt mehr Messpunkte. Der
  // Schwerpunkt darf sich davon nur wenig ziehen lassen.
  const int32_t pos[] = {13, 14, 15, 16, 17};
  const float v[] = {2.05f, 2.20f, 2.25f, 2.20f, 2.05f};
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt(pos, v, 5, 1.65f, 0.05f, &mitte));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, mitte);
}

// --- zeigerzeit ------------------------------------------------------------
void test_zeigerzeit_kennt_keine_null_uhr() {
  TEST_ASSERT_EQUAL_STRING("12:00", zeigerzeit(0, REV, SEK_JE_SCHRITT).c_str());
  TEST_ASSERT_EQUAL_STRING("12:00", zeigerzeit(REV, REV, SEK_JE_SCHRITT).c_str());
}

void test_zeigerzeit_volle_stunden() {
  TEST_ASSERT_EQUAL_STRING("3:00", zeigerzeit(800, REV, SEK_JE_SCHRITT).c_str());
  TEST_ASSERT_EQUAL_STRING("6:00", zeigerzeit(1600, REV, SEK_JE_SCHRITT).c_str());
  TEST_ASSERT_EQUAL_STRING("9:00", zeigerzeit(2400, REV, SEK_JE_SCHRITT).c_str());
}

void test_zeigerzeit_letzter_schritt_vor_zwoelf() {
  // 3199 * 13,5 = 43186,5 s = 11:59:46 -> "11:59"
  TEST_ASSERT_EQUAL_STRING("11:59", zeigerzeit(REV - 1, REV, SEK_JE_SCHRITT).c_str());
}

void test_zeigerzeit_vertraegt_grosse_zaehlerstaende() {
  TEST_ASSERT_EQUAL_STRING("3:00", zeigerzeit(200 * REV + 800, REV, SEK_JE_SCHRITT).c_str());
}

// --- Zusammenspiel ---------------------------------------------------------
// Ein ganzer Tag im Zeitraffer: die Uhr darf nie rueckwaerts laufen und muss
// nach 24 Stunden genau zwei Umdrehungen gemacht haben.
void test_ein_ganzer_tag_laeuft_nur_vorwaerts() {
  int32_t position = 0;
  int64_t gefahren = 0;

  for (int32_t sekunde = 0; sekunde < 86400; sekunde += 5) {
    const int32_t ziel = soll_position(sekunde, REV, SEK_JE_SCHRITT);
    const int32_t delta = delta_vorwaerts(position, ziel, REV);
    if (warten_statt_umrunden(delta, REV, 8))
      continue;
    TEST_ASSERT_TRUE_MESSAGE(delta >= 0, "Delta darf nie negativ werden");
    TEST_ASSERT_TRUE_MESSAGE(delta < 20, "Bei 5 s Takt sind hoechstens ein paar Schritte faellig");
    position += delta;
    gefahren += delta;
  }

  TEST_ASSERT_EQUAL_INT64(2 * REV, gefahren);
}

// ===========================================================================
// Schwerpunktsammler (AP 2.3) - dieselbe Rechnung, aber ohne Array
// ===========================================================================

void test_sammler_findet_die_mitte() {
  Schwerpunktsammler s(1.65f, 0.05f);
  // Symmetrische Kurve um Schritt 100
  s.nimm(98, 1.75f);
  s.nimm(99, 1.95f);
  s.nimm(100, 2.05f);
  s.nimm(101, 1.95f);
  s.nimm(102, 1.75f);
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(s.ergebnis(&mitte));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, mitte);
}

void test_sammler_rechnet_mit_dem_betrag() {
  // Falsch herum eingeklebter Magnet: die Kurve geht nach UNTEN. Der SS49E
  // ist bipolar, das Ergebnis muss dasselbe sein.
  Schwerpunktsammler s(1.65f, 0.05f);
  s.nimm(98, 1.55f);
  s.nimm(99, 1.35f);
  s.nimm(100, 1.25f);
  s.nimm(101, 1.35f);
  s.nimm(102, 1.55f);
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(s.ergebnis(&mitte));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, mitte);
}

void test_sammler_meldet_fehlendes_feld() {
  // Nur Rauschen um die Ruhelinie: kein Punkt ueber der Schwelle. Das MUSS
  // false sein und nicht 0 - eine 0 ist eine gueltige Position, und die Uhr
  // wuerde sich auf zwoelf referenzieren, wo gar kein Magnet ist.
  Schwerpunktsammler s(1.65f, 0.05f);
  for (int32_t p = 0; p < 50; p++)
    s.nimm(p, 1.65f + (p % 2 ? 0.01f : -0.01f));
  float mitte = -1.0f;
  TEST_ASSERT_FALSE(s.ergebnis(&mitte));
  TEST_ASSERT_EQUAL_UINT32(0, s.punkte());
}

void test_sammler_ist_gegen_eine_schiefe_flanke_robust() {
  // Die Kurve steigt links steiler als sie rechts faellt. Der Schwerpunkt
  // muss trotzdem zwischen den Aesten liegen und nicht am Rand.
  Schwerpunktsammler s(1.65f, 0.05f);
  s.nimm(10, 1.95f);
  s.nimm(11, 2.15f);
  s.nimm(12, 2.00f);
  s.nimm(13, 1.85f);
  s.nimm(14, 1.75f);
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(s.ergebnis(&mitte));
  TEST_ASSERT_TRUE(mitte > 10.5f && mitte < 12.5f);
}

void test_sammler_und_array_fassung_stimmen_ueberein() {
  // Beide Wege muessen dasselbe rechnen, sonst weicht die Referenzfahrt am
  // Geraet von dem ab, was der Host-Test prueft.
  const int32_t pos[] = {50, 51, 52, 53, 54, 55};
  const float mess[] = {1.70f, 1.85f, 2.00f, 1.98f, 1.80f, 1.68f};
  const float ruhe = 1.65f, schwelle = 0.05f;

  float aus_array = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt(pos, mess, 6, ruhe, schwelle, &aus_array));

  Schwerpunktsammler s(ruhe, schwelle);
  for (int i = 0; i < 6; i++)
    s.nimm(pos[i], mess[i]);
  float aus_sammler = 0.0f;
  TEST_ASSERT_TRUE(s.ergebnis(&aus_sammler));

  TEST_ASSERT_FLOAT_WITHIN(0.001f, aus_array, aus_sammler);
}

// ===========================================================================
// Schwerpunkt ueber ein symmetrisches Fenster
//
// DER FALL, DER AM AUFBAU AUFGEFALLEN IST: dieselbe Feldkurve, aber die Fahrt
// hat links mehr davon erfasst als rechts. Die alte Rechnung wandert dann
// nach links, die neue nicht.
// ===========================================================================

namespace {
/// Eine glockenfoermige Feldkurve um `gipfel`, wie sie der Magnet erzeugt.
/// `von`/`bis` schneiden sie ab - damit laesst sich eine Fahrt nachbilden,
/// die das Feld einseitig verpasst hat.
size_t kurve_bauen(int32_t *pos, float *wert, int32_t von, int32_t bis,
                   int32_t gipfel, float ruhe, float hoehe, int32_t halbwert) {
  size_t n = 0;
  for (int32_t p = von; p <= bis; p++) {
    const double d = static_cast<double>(p - gipfel) / halbwert;
    const float a = static_cast<float>(hoehe * std::exp(-d * d));
    pos[n] = p;
    wert[n] = ruhe - a;  // nach unten, wie am Aufbau gemessen
    n++;
  }
  return n;
}
}  // namespace

void test_symmetrisch_findet_die_mitte() {
  static int32_t pos[1200];
  static float wert[1200];
  const size_t n = kurve_bauen(pos, wert, 800, 1200, 1000, 1.60f, 0.25f, 100);
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt_symmetrisch(pos, wert, n, 1.60f, 0.05f, 150, &mitte));
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 1000.0f, mitte);
}

void test_symmetrisch_ist_gegen_einseitigen_schnitt_robust() {
  static int32_t pos_a[1200], pos_b[1200];
  static float wert_a[1200], wert_b[1200];

  // Die Kurve reicht bei Halbwertsbreite 100 und Schwelle 0,05 rund +-127
  // Schritte weit ueber die Schwelle. Damit der Schnitt der FAHRT wirkt, muss
  // er INNERHALB davon liegen - sonst schneidet die Schwelle zuerst, und
  // beide Rechnungen sehen dasselbe. (Genau daran ist dieser Test zuerst
  // gescheitert: er hat nichts geprueft.)
  //
  // A: 250 Schritte links vom Gipfel, 200 rechts - Kurve voll erfasst.
  const size_t na = kurve_bauen(pos_a, wert_a, 750, 1200, 1000, 1.60f, 0.25f, 100);
  // B: nur 50 Schritte links - die linke Flanke fehlt der Rechnung.
  const size_t nb = kurve_bauen(pos_b, wert_b, 950, 1200, 1000, 1.60f, 0.25f, 100);

  // Fenster +-40: bei BEIDEN Datensaetzen beidseitig abgedeckt.
  float a = 0.0f, b = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt_symmetrisch(pos_a, wert_a, na, 1.60f, 0.05f, 40, &a));
  TEST_ASSERT_TRUE(schwerpunkt_symmetrisch(pos_b, wert_b, nb, 1.60f, 0.05f, 40, &b));

  // A hat die Kurve vollstaendig - dort muss es genau stimmen.
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 1000.0f, a);

  // B fehlt die halbe linke Flanke. Dann bleibt ein SYSTEMATISCHER Restfehler:
  // die Iteration laeuft in einen Fixpunkt, bei dem Fenstermitte und Zentroid
  // uebereinstimmen, und der liegt bei einseitig fehlender Kurve nicht exakt
  // auf dem Gipfel. Gemessen rund 3 Schritte - gegenueber den 11 vor der
  // Iteration und den 23, die am Aufbau ohne Fenster auftraten.
  //
  // AM GERAET TRITT DIESER FALL NICHT AUF: der Rueckzug ist 500 Schritte, die
  // halbe Fensterbreite 199 - die Fahrt deckt die Kurve beidseitig ab. Der
  // Test beschreibt hier absichtlich einen schlimmeren Fall als die Realitaet.
  TEST_ASSERT_FLOAT_WITHIN(4.0f, 1000.0f, b);

  // Die ALTE Rechnung wandert dagegen weg: bei B fehlt die linke Flanke, also
  // zieht die rechte das Ergebnis nach rechts. Genau dieser Fehler hat am
  // Aufbau 23 Schritte Streuung erzeugt.
  float alt_a = 0.0f, alt_b = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt(pos_a, wert_a, na, 1.60f, 0.05f, &alt_a));
  TEST_ASSERT_TRUE(schwerpunkt(pos_b, wert_b, nb, 1.60f, 0.05f, &alt_b));
  TEST_ASSERT_TRUE(alt_b > alt_a + 5.0f);            // messbar verschoben
  TEST_ASSERT_TRUE(fabsf(alt_a - alt_b) > fabsf(a - b) * 5.0f);  // neue ist besser
}

void test_symmetrisch_uebersteht_einen_ausreisser_am_gipfel() {
  // DER FALL, DER AM AUFBAU DEN LETZTEN AUSREISSER ERZEUGT HAT: die Kurve ist
  // am Gipfel flach, und ein einzelner verrauschter Messpunkt wird dort zum
  // Maximum. Wird das Fenster um DIESEN Punkt gelegt, verschiebt es sich mit.
  static int32_t pos[600];
  static float wert[600];
  const size_t n = kurve_bauen(pos, wert, 800, 1200, 1000, 1.60f, 0.25f, 100);

  float ohne = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt_symmetrisch(pos, wert, n, 1.60f, 0.05f, 120, &ohne));

  // Jetzt einen Ausreisser 60 Schritte neben dem Gipfel einbauen, der HOEHER
  // ist als der echte Gipfel.
  for (size_t i = 0; i < n; i++)
    if (pos[i] == 1060)
      wert[i] = 1.60f - 0.30f;

  float mit = 0.0f;
  TEST_ASSERT_TRUE(schwerpunkt_symmetrisch(pos, wert, n, 1.60f, 0.05f, 120, &mit));

  // Das Ergebnis darf sich nur wenig bewegen - der eine Punkt hat unter
  // hunderten kaum Gewicht, solange das Zentrum gemittelt bestimmt wird.
  TEST_ASSERT_FLOAT_WITHIN(3.0f, ohne, mit);
}

void test_symmetrisch_lehnt_einseitig_unabgedecktes_fenster_ab() {
  // Die Fahrt endet 50 Schritte nach dem Gipfel, das Fenster will 200.
  // Dann ist das Ergebnis nicht vertrauenswuerdig - lieber false.
  static int32_t pos[600];
  static float wert[600];
  const size_t n = kurve_bauen(pos, wert, 700, 1050, 1000, 1.60f, 0.25f, 100);
  float mitte = -1.0f;
  TEST_ASSERT_FALSE(schwerpunkt_symmetrisch(pos, wert, n, 1.60f, 0.05f, 200, &mitte));
}

void test_symmetrisch_meldet_fehlendes_feld() {
  static int32_t pos[200];
  static float wert[200];
  for (int32_t i = 0; i < 200; i++) {
    pos[i] = i;
    wert[i] = 1.60f + (i % 2 ? 0.004f : -0.004f);
  }
  float mitte = -1.0f;
  TEST_ASSERT_FALSE(schwerpunkt_symmetrisch(pos, wert, 200, 1.60f, 0.05f, 50, &mitte));
}

// ===========================================================================
// Offset einmessen - "hier ist zwoelf Uhr"
//
// Diese Rechnung stand zuerst in lib/betrieb, mit falschem Vorzeichen und
// einem Nachlauf zu viel. Weil sie dort stand, konnte kein Host-Test sie
// fangen. Jetzt steht sie hier, und der Rundlauf wird geprueft.
// ===========================================================================

void test_einmessen_macht_die_iststellung_zur_null() {
  // Der Zeiger steht auf Schritt 200 und zeigt (von Hand gestellt) auf zwoelf.
  // Der Offset muss sich um genau diese 200 zurueckdrehen.
  TEST_ASSERT_EQUAL_INT32(normiere(-200, REV), offset_einmessen(0, 200, REV));
  TEST_ASSERT_EQUAL_INT32(normiere(300, REV), offset_einmessen(500, 200, REV));
}

void test_einmessen_faltet_negativ() {
  TEST_ASSERT_EQUAL_INT32(REV - 1, offset_einmessen(0, 1, REV));
  TEST_ASSERT_EQUAL_INT32(0, offset_einmessen(0, 0, REV));
  TEST_ASSERT_EQUAL_INT32(0, offset_einmessen(0, REV, REV));
}

void test_einmessen_schliesst_den_rundlauf() {
  // DER FALL, DER DEN FEHLER GEFUNDEN HAT. Vollstaendig durchgespielt:
  //
  //   Eine Referenzfahrt hat einen festen Nachlauf - sie faehrt immer
  //   dieselbe Strecke ueber den Schwerpunkt hinaus. Steht der Zeiger danach
  //   d Schritte VOR zwoelf, wird von Hand um d nachgestellt und eingemessen.
  //   Die NAECHSTE Fahrt muss dann bei derselben physischen Lage einen
  //   Zaehlerstand setzen, der genau diese Lage beschreibt - naemlich d
  //   Schritte vor zwoelf, also normiere(-d).
  const int32_t nachlauf = 50;
  const int32_t offset_alt = 0;
  const int32_t d = 30;  // so viel fehlt bis zwoelf

  // Fahrt 1 setzt den Zaehler:
  const int32_t zaehler_nach_fahrt1 = normiere(offset_alt + nachlauf, REV);
  // Von Hand um d nachstellen, bis der Zeiger auf zwoelf zeigt:
  const int32_t position = zaehler_nach_fahrt1 + d;
  // Einmessen:
  const int32_t offset_neu = offset_einmessen(offset_alt, position, REV);

  // Fahrt 2, dieselbe physische Lage, derselbe Nachlauf:
  const int32_t zaehler_nach_fahrt2 = normiere(offset_neu + nachlauf, REV);

  // Diese Lage liegt d Schritte VOR zwoelf.
  TEST_ASSERT_EQUAL_INT32(normiere(-d, REV), zaehler_nach_fahrt2);

  // Und von dort sind es genau d Schritte vorwaerts auf zwoelf - kein
  // Umlauf, keine Rueckwaertsfahrt.
  TEST_ASSERT_EQUAL_INT32(d, delta_vorwaerts(zaehler_nach_fahrt2, 0, REV));
}

void test_einmessen_ist_beim_zweiten_mal_stabil() {
  // Wurde einmal richtig eingemessen und steht der Zeiger wirklich auf
  // zwoelf (Position 0), darf sich der Offset NICHT mehr aendern.
  const int32_t offset = 1234;
  TEST_ASSERT_EQUAL_INT32(offset, offset_einmessen(offset, 0, REV));
}

// ===========================================================================
// Timer (AP 2.6)
// ===========================================================================

void test_timer_macht_genau_eine_umdrehung() {
  TEST_ASSERT_EQUAL_INT32(1000, timer_ziel(1000, 0, 300000, REV));
  TEST_ASSERT_EQUAL_INT32(1000 + REV / 2, timer_ziel(1000, 150000, 300000, REV));
  TEST_ASSERT_EQUAL_INT32(1000 + REV, timer_ziel(1000, 300000, 300000, REV));
}

void test_timer_laeuft_nie_weiter_als_einmal_herum() {
  // Auch wenn der Automat spaeter dran kommt als gedacht.
  TEST_ASSERT_EQUAL_INT32(1000 + REV, timer_ziel(1000, 999999, 300000, REV));
}

void test_timer_haelt_bei_zwoelf_stunden_die_aufloesung() {
  // 12 h = 43,2 Mio ms. In float hat der Quotient dort nur noch 24 Bit
  // Mantisse und springt in Stufen - deshalb rechnet timer_ziel in double.
  // Nach einer Sekunde muss sich das Ziel noch NICHT bewegt haben (ein
  // Schritt faellt erst alle 13,5 s an), nach 20 s aber schon.
  const uint32_t dauer = 43200u * 1000u;
  TEST_ASSERT_EQUAL_INT32(0, timer_ziel(0, 1000, dauer, REV));
  TEST_ASSERT_EQUAL_INT32(1, timer_ziel(0, 14000, dauer, REV));
  TEST_ASSERT_EQUAL_INT32(REV / 2, timer_ziel(0, dauer / 2, dauer, REV));
}

void test_timer_ohne_dauer_ist_sofort_um() {
  TEST_ASSERT_EQUAL_INT32(500 + REV, timer_ziel(500, 0, 0, REV));
}

// --- Laufart des Timers (27.09.2026) -------------------------------------
//
// Gleitend: EIN Fahrbefehl mit gerechnetem Tempo. Sekundentakt: ein Sprung je
// Sekunde. Vorher bekam der Zeiger alle 250 ms ein neues Ziel, und jedes war
// eine eigene Fahrt mit Rampe - das Ruckeln.

void test_sprungziel_steht_die_sekunde_und_springt_dann() {
  // 5 min: 3200 / 300 = 10,67 Schritte je Sekunde.
  TEST_ASSERT_EQUAL_INT32(1000, timer_sprungziel(1000, 0, 300000, REV));
  TEST_ASSERT_EQUAL_INT32(1000, timer_sprungziel(1000, 999, 300000, REV));
  TEST_ASSERT_EQUAL_INT32(1011, timer_sprungziel(1000, 1000, 300000, REV));
  TEST_ASSERT_EQUAL_INT32(1011, timer_sprungziel(1000, 1999, 300000, REV));
  TEST_ASSERT_EQUAL_INT32(1000 + REV, timer_sprungziel(1000, 300000, 300000, REV));
}

void test_sprungziel_springt_gleichmaessig_und_nur_vorwaerts() {
  // Jede Sekunde 10 oder 11 Schritte, nie 0, nie rueckwaerts - und die
  // Summe ist genau eine Umdrehung.
  int32_t vorher = timer_sprungziel(0, 0, 300000, REV);
  for (uint32_t k = 1; k <= 300; k++) {
    const int32_t jetzt = timer_sprungziel(0, k * 1000u, 300000, REV);
    const int32_t sprung = jetzt - vorher;
    TEST_ASSERT_TRUE_MESSAGE(sprung == 10 || sprung == 11, "Sprung je Sekunde");
    vorher = jetzt;
  }
  TEST_ASSERT_EQUAL_INT32(REV, vorher);
}

void test_gleittempo_ist_eine_umdrehung_in_der_dauer() {
  // 5 min: 3200 Schritte in 300 s = 10,667 Hz. 12 h: 74 mHz, also ein
  // Schritt alle 13,5 s - innerhalb dessen, was FastAccelStepper kann
  // (bis rund 15,6 s je Schritt).
  TEST_ASSERT_EQUAL_UINT32(10667, timer_gleittempo_mhz(REV, 300000, 0, 2000, 19200));
  TEST_ASSERT_EQUAL_UINT32(74, timer_gleittempo_mhz(REV, 43200u * 1000u, 0, 2000, 19200));
}

void test_gleittempo_uebergeht_einen_schritt_rundung() {
  const uint32_t nenn = timer_gleittempo_mhz(REV, 300000, 0, 2000, 19200);
  TEST_ASSERT_EQUAL_UINT32(nenn, timer_gleittempo_mhz(REV, 300000, 1, 2000, 19200));
  TEST_ASSERT_EQUAL_UINT32(nenn, timer_gleittempo_mhz(REV, 300000, -1, 2000, 19200));
}

void test_gleittempo_holt_rueckstand_auf_und_bleibt_in_grenzen() {
  // 10 Schritte hinten, ueber 2 s abgebaut: 5 Hz obendrauf.
  TEST_ASSERT_EQUAL_UINT32(15667, timer_gleittempo_mhz(REV, 300000, 10, 2000, 19200));
  // Weit voraus: nie unter den halben Nennwert - der Zeiger steht nie.
  TEST_ASSERT_EQUAL_UINT32(5333, timer_gleittempo_mhz(REV, 300000, -1000, 2000, 19200));
  // Weit hinten: nie ueber das Fahrtempo.
  TEST_ASSERT_EQUAL_UINT32(19200000u, timer_gleittempo_mhz(REV, 1000, 100000, 2000, 19200));
}

// --- Uhrname (27.09.2026) -------------------------------------------------

void test_alias_nimmt_umlaute_und_schneidet_raender() {
  std::string n = "alt";
  TEST_ASSERT_TRUE(alias_pruefen("  K\xC3\xBC" "che  ", &n));
  TEST_ASSERT_EQUAL_STRING("K\xC3\xBC" "che", n.c_str());
  TEST_ASSERT_TRUE(alias_pruefen("Wohnzimmer links", &n));
  TEST_ASSERT_EQUAL_STRING("Wohnzimmer links", n.c_str());
}

void test_alias_leer_heisst_kein_name() {
  std::string n = "alt";
  TEST_ASSERT_TRUE(alias_pruefen("   ", &n));
  TEST_ASSERT_EQUAL_STRING("", n.c_str());
}

void test_alias_weist_json_und_html_zeichen_ab() {
  std::string n = "alt";
  TEST_ASSERT_FALSE(alias_pruefen("a\"b", &n));
  TEST_ASSERT_FALSE(alias_pruefen("a\\b", &n));
  TEST_ASSERT_FALSE(alias_pruefen("<b>", &n));
  TEST_ASSERT_FALSE(alias_pruefen("a\tb", &n));
  TEST_ASSERT_FALSE(alias_pruefen("\xC3", &n));         // abgebrochenes UTF-8
  TEST_ASSERT_FALSE(alias_pruefen("\xBC" "abc", &n));   // Folgebyte ohne Anfang
  TEST_ASSERT_EQUAL_STRING("alt", n.c_str());           // bei Ablehnung unberuehrt
}

void test_alias_zaehlt_zeichen_nicht_bytes() {
  std::string n;
  // 24 Umlaute = 48 Bytes, aber 24 Zeichen: geht. 25 nicht.
  std::string ue;
  for (int i = 0; i < 24; i++)
    ue += "\xC3\xBC";
  TEST_ASSERT_TRUE(alias_pruefen(ue, &n));
  TEST_ASSERT_FALSE(alias_pruefen(ue + "x", &n));
}

// --- Weiche Lichtautomatik (27.09.2026) ------------------------------------

void test_dimm_nachfuehren_geht_weich_und_kommt_an() {
  // Von 1,0 auf 0,2 mit tau 1000 ms in 20-ms-Schritten: jeder Schritt klein
  // (hoechstens 2 % des Wegs), monoton fallend, nach 3 tau 95 % des Wegs,
  // und genau am Ziel, bevor 8 tau um sind.
  float f = 1.0f;
  float vorher = f;
  int schritte = 0;
  float nach_3tau = -1.0f;
  while (f != 0.2f && schritte < 1000) {
    f = dimm_nachfuehren(f, 0.2f, 20, 1000);
    TEST_ASSERT_TRUE(f <= vorher);
    TEST_ASSERT_TRUE(vorher - f <= 0.8f * 0.02f);
    vorher = f;
    schritte++;
    if (schritte == 150)
      nach_3tau = f;
  }
  TEST_ASSERT_EQUAL_FLOAT(0.2f, f);
  TEST_ASSERT_TRUE(schritte * 20 <= 8 * 1000);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f - 0.8f * 0.95f, nach_3tau);
}

void test_zwei_stufen_setzen_sanfter_ein_und_kommen_an() {
  // Zwei Stufen hintereinander (wie main.cpp): der erste Schritt ist viel
  // kleiner als mit einer, und am Ende steht der Wert genau auf dem Ziel.
  float stufe = 1.0f, f = 1.0f;
  stufe = dimm_nachfuehren(stufe, 0.2f, 20, 1000);
  f = dimm_nachfuehren(f, stufe, 20, 1000);
  const float einstufig = dimm_nachfuehren(1.0f, 0.2f, 20, 1000);
  TEST_ASSERT_TRUE((1.0f - f) < 0.1f * (1.0f - einstufig));
  int n = 0;
  while (f != 0.2f && n < 2000) {
    stufe = dimm_nachfuehren(stufe, 0.2f, 20, 1000);
    f = dimm_nachfuehren(f, stufe, 20, 1000);
    n++;
  }
  TEST_ASSERT_EQUAL_FLOAT(0.2f, f);
  // Nach 3 s noch nicht ganz da (langsamer als eine Stufe), nach 8 s schon.
  TEST_ASSERT_TRUE(n * 20 > 3000 && n * 20 < 12000);
}

void test_dimm_nachfuehren_sofort_und_geklemmt() {
  TEST_ASSERT_EQUAL_FLOAT(0.3f, dimm_nachfuehren(1.0f, 0.3f, 20, 0));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, dimm_nachfuehren(1.0f, 7.0f, 20, 1500));
  // NaN als Ziel heisst voll (1,0) - nie ein dunkler Ausgang ohne Meldung.
  TEST_ASSERT_EQUAL_FLOAT(1.0f, dimm_nachfuehren(0.5f, NAN, 1000000, 1500));
}

// --- Gemerkte Update-Adresse umstellen (28.09.2026) -----------------------

namespace {
const char *ORDNER = "https://h/o/updates/";
const char *STABIL = "https://h/o/updates/manifest.json";
const char *BETA = "https://h/o/updates/manifest-beta.json";
}  // namespace

void test_update_url_alte_namen_werden_umgestellt() {
  std::string u = "https://h/o/updates/manifest-alt.json";
  TEST_ASSERT_TRUE(update_url_umstellen(&u, ORDNER, STABIL, BETA));
  TEST_ASSERT_EQUAL_STRING(STABIL, u.c_str());
  u = "https://h/o/updates/manifest-alt-beta.json";  // beta bleibt beta
  TEST_ASSERT_TRUE(update_url_umstellen(&u, ORDNER, STABIL, BETA));
  TEST_ASSERT_EQUAL_STRING(BETA, u.c_str());
}

void test_update_url_heutige_und_fremde_bleiben() {
  std::string u = STABIL;
  TEST_ASSERT_FALSE(update_url_umstellen(&u, ORDNER, STABIL, BETA));
  u = BETA;
  TEST_ASSERT_FALSE(update_url_umstellen(&u, ORDNER, STABIL, BETA));
  u = "https://mein.server/uhr/manifest.json";  // selbst gesetzt
  TEST_ASSERT_FALSE(update_url_umstellen(&u, ORDNER, STABIL, BETA));
  TEST_ASSERT_EQUAL_STRING("https://mein.server/uhr/manifest.json", u.c_str());
}

// --- Weiche Fahrt und Zuendung (28.09.2026) --------------------------------

void test_weiche_beschleunigung_gibt_kurzen_wegen_zeit() {
  // Minutensprung, 4 Schritte, mindestens 180 ms: a = 4*4/0,18^2 = 493.
  TEST_ASSERT_EQUAL_UINT32(493, weiche_beschleunigung(4, 51200, 180));
  // Lange Wege laufen mit der eingestellten Rampe.
  TEST_ASSERT_EQUAL_UINT32(51200, weiche_beschleunigung(10000, 51200, 180));
  TEST_ASSERT_EQUAL_UINT32(51200, weiche_beschleunigung(0, 51200, 180));
  // Die Fahrzeit mit dieser Rampe ist wirklich mindestens 180 ms.
  TEST_ASSERT_TRUE(fahrzeit_ms(4, weiche_beschleunigung(4, 51200, 180), 12800) >= 179);
  TEST_ASSERT_EQUAL_UINT32(1, weiche_beschleunigung(4, 0, 180));
}

void test_weicher_einsatz_ist_ein_viertel_mit_deckel() {
  TEST_ASSERT_EQUAL_UINT32(1, weicher_einsatz(4, 800));
  TEST_ASSERT_EQUAL_UINT32(250, weicher_einsatz(1000, 800));
  TEST_ASSERT_EQUAL_UINT32(800, weicher_einsatz(100000, 800));
}

namespace {
uint8_t kanal(uint32_t f, int s) { return (f >> s) & 0xFF; }
}

void test_zuendung_beginnt_dunkel_und_endet_genau() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, zuend_bild(0, 0xFF0000, 0xFF0000, 7).anteil);
  const Zuendbild e = zuend_bild(ZUEND_DAUER_MS, 0x0000FF, 0xFF0000, 7);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, e.anteil);
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, e.farbe);
  // Kurz vor Schluss schon fast dort - kein Sprung am Ende.
  const Zuendbild f = zuend_bild(ZUEND_DAUER_MS - 5, 0x0000FF, 0xFF0000, 7);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, f.anteil);
  TEST_ASSERT_TRUE(kanal(f.farbe, 8) <= 3 && kanal(f.farbe, 0) <= 3);
}

void test_zuendung_bleibt_in_grenzen_und_ist_wiederholbar() {
  for (uint32_t z = 1; z < 6; z++) {
    for (uint32_t t = 0; t < ZUEND_DAUER_MS; t += 5) {
      const Zuendbild b = zuend_bild(t, 0x00FF00, 0xFF0000, z);
      TEST_ASSERT_TRUE(b.anteil >= 0.0f && b.anteil <= 1.35f);
      TEST_ASSERT_EQUAL_HEX32(b.farbe, zuend_bild(t, 0x00FF00, 0xFF0000, z).farbe);
    }
  }
}

void test_zuendung_laeuft_nie_ueber_weiss() {
  // 01.10.2026: "laeuft immer ueber weiss, soll eher die color temp changen".
  // Bei einer gesaettigten Zielfarbe bleibt der kleinste Kanal unten - kein
  // Moment, in dem alle drei LEDs hell sind.
  for (uint32_t ziel : {0xFF8000u, 0x00FF00u, 0x0000FFu, 0xFF0000u}) {
    for (uint32_t t = 0; t < ZUEND_DAUER_MS; t += 20) {
      const uint32_t f = zuend_bild(t, ziel, ziel, 3).farbe;
      const int mn = std::min({kanal(f, 16), kanal(f, 8), kanal(f, 0)});
      TEST_ASSERT_TRUE_MESSAGE(mn < 40, "Zuendung zieht Richtung Weiss");
    }
  }
}

void test_zuendung_beginnt_waermer_und_endet_in_der_farbe() {
  // Gruen: anfangs gelbgruen (Rot ueber null), am Ende reines Gruen.
  uint8_t rot_frueh = 0;
  for (uint32_t t = 200; t < 700; t += 10)
    rot_frueh = std::max<uint8_t>(rot_frueh, kanal(zuend_bild(t, 0x00FF00, 0x00FF00, 3).farbe, 16));
  TEST_ASSERT_TRUE(rot_frueh >= 60);
  TEST_ASSERT_EQUAL_HEX32(0x00FF00, zuend_bild(ZUEND_DAUER_MS, 0x00FF00, 0x00FF00, 3).farbe);
}

void test_waermer_verschiebt_den_farbton_richtung_rot() {
  // Gelb (255/128/0) wird orange: weniger Gruen, Rot bleibt voll.
  const uint32_t gelb = starwatch::waermer(0xFF8000, 1.0f);
  TEST_ASSERT_EQUAL_UINT8(255, kanal(gelb, 16));
  TEST_ASSERT_TRUE(kanal(gelb, 8) < 128);
  // Gruen wird gelbgruen, Blau wird violett, Rot bleibt Rot.
  TEST_ASSERT_TRUE(kanal(starwatch::waermer(0x00FF00, 1.0f), 16) > 60);
  TEST_ASSERT_TRUE(kanal(starwatch::waermer(0x0000FF, 1.0f), 16) > 60);
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, starwatch::waermer(0xFF0000, 1.0f));
  // Weiss wird warm: deutlich weniger Blau.
  TEST_ASSERT_TRUE(kanal(starwatch::waermer(0xFFD764, 1.0f), 0) < 80);
  // Staerke 0 aendert nichts, Schwarz bleibt schwarz.
  TEST_ASSERT_EQUAL_HEX32(0x12AB34, starwatch::waermer(0x12AB34, 0.0f));
  TEST_ASSERT_EQUAL_HEX32(0x000000, starwatch::waermer(0x000000, 1.0f));
}

void test_zuendung_zeigt_nur_die_neue_farbe() {
  // Von Blau nach Gelb: kein Moment mit Blau - die Zuendung zeigt das neue
  // Gelb, anfangs waermer (orange), am Ende genau.
  for (uint32_t t = 0; t < ZUEND_DAUER_MS; t += 20)
    TEST_ASSERT_TRUE(kanal(zuend_bild(t, 0x0000FF, 0xFF8000, 9).farbe, 0) == 0);
  TEST_ASSERT_TRUE(kanal(zuend_bild(400, 0x0000FF, 0xFF8000, 9).farbe, 8) < 128);
  TEST_ASSERT_EQUAL_HEX32(0xFF8000, zuend_bild(ZUEND_DAUER_MS, 0x0000FF, 0xFF8000, 9).farbe);
}

void test_einfahren_flackert_faehrt_ein_und_ist_dann_dunkel() {
  float spitze = 0.0f;
  for (uint32_t t = 0; t < 250; t += 5)
    spitze = std::max(spitze, loesch_bild(t, 0xFF0000, 4).anteil);
  TEST_ASSERT_TRUE(spitze > 1.04f && spitze <= 1.10f);        // wallt kurz auf
  TEST_ASSERT_TRUE(loesch_bild(900, 0xFF0000, 4).anteil < 0.8f);  // faehrt ein
  TEST_ASSERT_TRUE(loesch_bild(1600, 0xFF0000, 4).anteil < 0.1f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, loesch_bild(LOESCH_DAUER_MS, 0xFF0000, 4).anteil);
  for (uint32_t t = 0; t < LOESCH_DAUER_MS; t += 10) {
    const float a = loesch_bild(t, 0xFF0000, 4).anteil;
    TEST_ASSERT_TRUE(a >= 0.0f && a <= 1.16f);
  }
}

void test_kein_stroboskop_beim_zuenden_und_einfahren() {
  // AM GERAET BEURTEILT (28.09.2026): harte Blitze aus dem Dunkeln waren
  // unangenehm, ganz ohne Flackern war es leblos. Die Grenze dazwischen:
  // Flackerer duerfen deutlich sein, aber die Klinge faellt nie zurueck ins
  // Dunkle - einmal ueber 0,5, bleibt sie ueber 0,3 (bis zum Einfahren).
  // Und kein Bild springt um mehr als 0,6.
  for (uint32_t z = 1; z < 20; z++) {
    float vorher = 0.0f, hoechst = 0.0f;
    int einbrueche = 0;
    bool im_einbruch = false;
    for (uint32_t t = 0; t <= ZUEND_DAUER_MS; t += 20) {
      const float a = zuend_bild(t, 0xFF0000, 0x0014FF, z).anteil;
      TEST_ASSERT_TRUE_MESSAGE(std::fabs(a - vorher) < 0.6f, "Zuenden springt");
      if (hoechst > 0.5f)
        TEST_ASSERT_TRUE_MESSAGE(a > 0.3f, "Zuenden faellt ins Dunkle");
      if (a > hoechst)
        hoechst = a;
      // Flackerer zaehlen: deutlich unter dem bisherigen Hoechstwert.
      const bool tief = hoechst > 1.0f && a < 0.85f * hoechst;
      if (tief && !im_einbruch)
        einbrueche++;
      im_einbruch = tief;
      vorher = a;
    }
    TEST_ASSERT_TRUE_MESSAGE(einbrueche >= 2, "Flackern kaum sichtbar");
  }
  float vorher = loesch_bild(0, 0xFF0000, 1).anteil;
  for (uint32_t t = 20; t <= LOESCH_DAUER_MS; t += 20) {
    const float a = loesch_bild(t, 0xFF0000, 1).anteil;
    TEST_ASSERT_TRUE_MESSAGE(std::fabs(a - vorher) < 0.6f, "Einfahren springt");
    vorher = a;
  }
}

void test_blende_ohne_effekt() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, blende_bild(0, 0.0f, 1.0f, 0xFF0000, 0xFF0000).anteil);
  const Zuendbild m = blende_bild(BLENDE_DAUER_MS / 2, 1.0f, 1.0f, 0x0000FF, 0xFF0000);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, m.anteil);
  TEST_ASSERT_TRUE(kanal(m.farbe, 16) > 90 && kanal(m.farbe, 0) > 90);
  const Zuendbild e = blende_bild(BLENDE_DAUER_MS, 1.0f, 0.0f, 0x0000FF, 0xFF0000);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, e.anteil);
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, e.farbe);
}

void test_gleittempo_ohne_dauer_ist_null() {
  TEST_ASSERT_EQUAL_UINT32(0, timer_gleittempo_mhz(REV, 0, 0, 2000, 19200));
  TEST_ASSERT_EQUAL_UINT32(0, timer_gleittempo_mhz(0, 300000, 0, 2000, 19200));
}

// --- Mindestdauer einer Umdrehung (23.08.2026) ---------------------------
//
// 't 1' wurde vorher widerspruchslos angenommen und brauchte dann ein
// Vielfaches. Diese Rechnung sagt vorher, was ueberhaupt geht.

void test_mindestdauer_dreieck_wenn_das_tempo_nicht_reicht() {
  // Rampe 38400, Hoechsttempo sehr hoch: die Strecke ist zu kurz, um es zu
  // erreichen. Spitzentempo sqrt(rev*a) = sqrt(3200*38400) = 11085,
  // Dauer 2*sqrt(3200/38400) = 0,577 s.
  const uint32_t ms = timer_mindestdauer_ms(REV, 38400, 1000000);
  TEST_ASSERT_UINT32_WITHIN(10, 577, ms);
}

void test_mindestdauer_trapez_wenn_das_tempo_begrenzt() {
  // Kleines Hoechsttempo: 3200 Schritte/s, Rampe 38400. Die Rampen kosten
  // 2*v/a = 0,167 s und darin v^2/a = 266,7 Schritte; der Rest bei v:
  // (3200-266,7)/3200 = 0,917 s. Zusammen 1,083 s.
  const uint32_t ms = timer_mindestdauer_ms(REV, 38400, 3200);
  TEST_ASSERT_UINT32_WITHIN(10, 1083, ms);
}

void test_mindestdauer_faellt_mit_mehr_rampe() {
  // Mehr Rampe kann die Dauer nur verkuerzen, nie verlaengern.
  const uint32_t traege = timer_mindestdauer_ms(REV, 2000, 19200);
  const uint32_t flink = timer_mindestdauer_ms(REV, 38400, 19200);
  TEST_ASSERT_TRUE(flink < traege);
}

void test_mindestdauer_die_alte_rampe_konnte_keine_sekunde() {
  // Der Beweis fuer den Befund: mit den bis 23.08.2026 gesetzten 2000
  // Schritten/s^2 war eine Umdrehung in einer Sekunde unmoeglich.
  TEST_ASSERT_TRUE(timer_mindestdauer_ms(REV, 2000, 19200) > 1000);
}

// --- Fahrzeit allgemein, und damit der Schwungeffekt -------------------

void test_fahrzeit_ist_die_mindestdauer_fuer_eine_umdrehung() {
  // timer_mindestdauer_ms ist nur der Sonderfall weg = REV.
  TEST_ASSERT_EQUAL_UINT32(timer_mindestdauer_ms(REV, 51200, 19200),
                           fahrzeit_ms(REV, 51200, 19200));
}

void test_fahrzeit_zwei_schwungkreise_plus_viertel() {
  // Der Fall aus der Aufgabe: von 12:00 auf 15:00 mit zwei Schwungkreisen.
  // 2*3200 + 800 = 7200 Schritte bei a = 51200, v = 19200.
  // sqrt(7200*51200) = 19200 - das Dreieck erreicht genau das Hoechsttempo,
  // also Dauer 2*sqrt(7200/51200) = 0,750 s.
  TEST_ASSERT_UINT32_WITHIN(10, 750, fahrzeit_ms(2 * REV + 800, 51200, 19200));
}

void test_fahrzeit_waechst_mit_dem_weg() {
  TEST_ASSERT_TRUE(fahrzeit_ms(REV, 51200, 19200) <
                   fahrzeit_ms(2 * REV, 51200, 19200));
  TEST_ASSERT_TRUE(fahrzeit_ms(2 * REV, 51200, 19200) <
                   fahrzeit_ms(5 * REV, 51200, 19200));
}

void test_fahrzeit_langer_weg_ist_fast_reine_fahrt() {
  // Bei 20 Umdrehungen fallen die Rampen kaum noch auf: 64000 Schritte bei
  // 19200/s sind 3,33 s reine Fahrt, plus 2*v/a - v^2/a/v = v/a = 0,375 s.
  TEST_ASSERT_UINT32_WITHIN(30, 3708, fahrzeit_ms(20 * REV, 51200, 19200));
}

// --- Versionsvergleich (24.08.2026) --------------------------------------
//
// Entscheidet, ob ein Geraet ein Update annimmt. Ein Fehler hier heisst
// entweder "nimmt nichts mehr an" oder "nimmt ein Downgrade an".

void test_semver_gleich_ist_null() {
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich("1.2.3", "1.2.3"));
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich("0.9.0", "0.9.0"));
}

void test_semver_vergleicht_zahlen_nicht_text() {
  // Der Klassiker: alphabetisch kaeme "0.10.0" VOR "0.9.0".
  TEST_ASSERT_TRUE(semver_vergleich("0.10.0", "0.9.0") > 0);
  TEST_ASSERT_TRUE(semver_vergleich("0.9.0", "0.10.0") < 0);
  TEST_ASSERT_TRUE(semver_vergleich("1.0.0", "0.99.99") > 0);
  TEST_ASSERT_TRUE(semver_vergleich("2.0.0", "10.0.0") < 0);
}

void test_semver_fehlende_stellen_sind_null() {
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich("1.2", "1.2.0"));
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich("1", "1.0.0"));
  TEST_ASSERT_TRUE(semver_vergleich("1.2.1", "1.2") > 0);
}

void test_semver_anhaengsel_zaehlt_nicht() {
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich("1.2.3-test", "1.2.3"));
}

void test_semver_vertraegt_unsinn() {
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich(nullptr, "1.0.0"));
  TEST_ASSERT_EQUAL_INT(0, semver_vergleich("1.0.0", nullptr));
  TEST_ASSERT_TRUE(semver_vergleich("1.0.0", "") > 0);
}

void test_mindestdauer_vertraegt_unsinn() {
  TEST_ASSERT_EQUAL_UINT32(0, timer_mindestdauer_ms(0, 38400, 19200));
  TEST_ASSERT_EQUAL_UINT32(0, timer_mindestdauer_ms(REV, 0, 19200));
  TEST_ASSERT_EQUAL_UINT32(0, timer_mindestdauer_ms(REV, 38400, 0));
}

void test_timer_restzeit_rundet_auf() {
  // Solange eine angefangene Sekunde laeuft, darf nicht 0 dastehen - sonst
  // zeigt die Anzeige "abgelaufen", waehrend der Zeiger noch faehrt.
  TEST_ASSERT_EQUAL_STRING("0:05:00", timer_restzeit(0, 300000).c_str());
  TEST_ASSERT_EQUAL_STRING("0:02:30", timer_restzeit(150000, 300000).c_str());
  TEST_ASSERT_EQUAL_STRING("0:00:01", timer_restzeit(299500, 300000).c_str());
  TEST_ASSERT_EQUAL_STRING("0:00:00", timer_restzeit(300000, 300000).c_str());
  TEST_ASSERT_EQUAL_STRING("0:00:00", timer_restzeit(400000, 300000).c_str());
  TEST_ASSERT_EQUAL_STRING("12:00:00", timer_restzeit(0, 43200u * 1000u).c_str());
  TEST_ASSERT_EQUAL_STRING("--:--:--", timer_restzeit(0, 0).c_str());
}

// ===========================================================================
// Helligkeitskennlinie (AP 2.5)
// ===========================================================================

void test_null_prozent_ist_wirklich_aus() {
  // NICHT die Glimmschwelle. Wer "aus" stellt, will dunkel.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, tastverhaeltnis(0.0f, 5.0f));
}

void test_hundert_prozent_ist_voll() {
  TEST_ASSERT_EQUAL_FLOAT(1.0f, tastverhaeltnis(100.0f, 5.0f));
}

void test_kennlinie_spreizt_auf_die_glimmschwelle() {
  // Bei einer Untergrenze von 5 % soll 1 % am Regler knapp darueber liegen
  // und nicht darunter - sonst ist das erste Stueck des Reglers wirkungslos.
  const float bei_eins = tastverhaeltnis(1.0f, 5.0f);
  TEST_ASSERT_TRUE(bei_eins > 0.05f);
  TEST_ASSERT_TRUE(bei_eins < 0.06f);
  // Die Mitte des Reglers liegt in der Mitte zwischen Schwelle und voll.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.525f, tastverhaeltnis(50.0f, 5.0f));
}

void test_kennlinie_ohne_untergrenze_ist_linear() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, tastverhaeltnis(25.0f, 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.50f, tastverhaeltnis(50.0f, 0.0f));
}

void test_kennlinie_vertraegt_unsinn() {
  // Unsinnige Untergrenzen duerfen nicht aus dem Bereich 0..1 herausfuehren.
  TEST_ASSERT_TRUE(tastverhaeltnis(50.0f, -10.0f) >= 0.0f);
  TEST_ASSERT_TRUE(tastverhaeltnis(50.0f, 200.0f) <= 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, tastverhaeltnis(-5.0f, 5.0f));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, tastverhaeltnis(150.0f, 5.0f));
}

// ===========================================================================
// Farbe
// ===========================================================================

void test_farbe_liest_hex_mit_und_ohne_raute() {
  uint32_t f = 0;
  TEST_ASSERT_TRUE(farbe_lesen("#ff8800", &f));
  TEST_ASSERT_EQUAL_HEX32(0xFF8800, f);
  TEST_ASSERT_TRUE(farbe_lesen("00A0fF", &f));
  TEST_ASSERT_EQUAL_HEX32(0x00A0FF, f);
  TEST_ASSERT_TRUE(farbe_lesen("  #000000  ", &f));
  TEST_ASSERT_EQUAL_HEX32(0x000000, f);
}

void test_farbe_liest_drei_dezimalzahlen() {
  uint32_t f = 0;
  TEST_ASSERT_TRUE(farbe_lesen("255 136 0", &f));
  TEST_ASSERT_EQUAL_HEX32(0xFF8800, f);
  TEST_ASSERT_TRUE(farbe_lesen("0 0 255", &f));
  TEST_ASSERT_EQUAL_HEX32(0x0000FF, f);
}

void test_farbe_laesst_bei_unsinn_alles_stehen() {
  // Ein Vertipper darf die Farbe nicht halb setzen.
  uint32_t f = 0x123456;
  TEST_ASSERT_FALSE(farbe_lesen("", &f));
  TEST_ASSERT_FALSE(farbe_lesen("#ff88", &f));      // zu kurz
  TEST_ASSERT_FALSE(farbe_lesen("ff8800aa", &f));   // zu lang
  TEST_ASSERT_FALSE(farbe_lesen("gg0000", &f));     // keine Hexziffer
  TEST_ASSERT_FALSE(farbe_lesen("255", &f));        // eine Zahl ist keine Farbe
  TEST_ASSERT_FALSE(farbe_lesen("255 0", &f));      // zwei auch nicht
  TEST_ASSERT_FALSE(farbe_lesen("256 0 0", &f));    // ueber 255
  TEST_ASSERT_FALSE(farbe_lesen("1 2 3 4", &f));    // eine zu viel
  TEST_ASSERT_FALSE(farbe_lesen(nullptr, &f));
  TEST_ASSERT_EQUAL_HEX32(0x123456, f);
}

// Leistung je Kanal bei 100 %, gerundet wie am Geraet: R 0,77 W,
// G und B je 1,07 W. Grenze 2,4 W = 80 % von 3 W (die Vorgabe bis 27.10.2026).
static const float WATT_F[3] = {0.765f, 1.071f, 1.071f};
static const float GRENZE_F = 2.4f;

void test_kanaele_folgen_der_farbe_linear() {
  float k[3];
  kanal_anteile(1.0f, 0xFF8000, WATT_F, GRENZE_F, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 128.0f / 255.0f, k[1]);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, k[2]);
}

void test_reine_farbe_bei_hundert_prozent_bleibt_ungebremst() {
  // Rot allein ist unter 1 W - volle 100 %, keine Begrenzung.
  float k[3];
  const float w = kanal_anteile(1.0f, 0xFF0000, WATT_F, GRENZE_F, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.765f, w);
  kanal_anteile(1.0f, 0x0000FF, WATT_F, GRENZE_F, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[2]);
}

void test_zwei_farben_bei_hundert_prozent_bleiben_ungebremst() {
  float k[3];
  kanal_anteile(1.0f, 0x00FFFF, WATT_F, GRENZE_F, k);  // Gruen+Blau, 2,14 W
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[1]);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[2]);
  kanal_anteile(1.0f, 0xFFFF00, WATT_F, GRENZE_F, k);  // Rot+Gruen
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[1]);
}

void test_weiss_wird_auf_die_leistungsgrenze_genommen() {
  float k[3];
  const float w = kanal_anteile(1.0f, 0xFFFFFF, WATT_F, GRENZE_F, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, GRENZE_F, w);
  const float soll = GRENZE_F / (WATT_F[0] + WATT_F[1] + WATT_F[2]);
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, soll, k[i]);
}

void test_begrenzung_behaelt_den_farbton() {
  // Alle Kanaele um DENSELBEN Faktor - das Verhaeltnis bleibt.
  float k[3];
  kanal_anteile(1.0f, 0xFFFF80, WATT_F, 1.0f, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, k[1] / k[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 128.0f / 255.0f, k[2] / k[0]);
}

void test_halbe_helligkeit_liegt_unter_der_grenze() {
  // Weiss bei 50 %: 1,45 W - wird NICHT weiter gedrosselt.
  float k[3];
  kanal_anteile(0.5f, 0xFFFFFF, WATT_F, GRENZE_F, k);
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.5f, k[i]);
}

void test_kanaele_vertragen_unsinn() {
  float k[3];
  kanal_anteile(NAN, 0xFFFFFF, WATT_F, GRENZE_F, k);
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_EQUAL_FLOAT(0.0f, k[i]);
  kanal_anteile(3.0f, 0xFF0000, WATT_F, GRENZE_F, k);
  TEST_ASSERT_TRUE(k[0] <= 1.0f);
  // Eine unsinnige Grenze heisst dunkel, nicht unbegrenzt.
  kanal_anteile(1.0f, 0xFFFFFF, WATT_F, NAN, k);
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_EQUAL_FLOAT(0.0f, k[i]);
  kanal_anteile(1.0f, 0xFF000000u | 0x0000FF, WATT_F, GRENZE_F, k);  // Muell oben
  TEST_ASSERT_EQUAL_FLOAT(0.0f, k[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, k[2]);
}

void test_farbe_liest_namen() {
  uint32_t f = 0;
  TEST_ASSERT_TRUE(farbe_lesen("gelb", &f));
  TEST_ASSERT_EQUAL_HEX32(0xFF6E00, f);  // am Geraet abgestimmt, 28.09.2026
  TEST_ASSERT_TRUE(farbe_lesen("Weiss", &f));
  TEST_ASSERT_EQUAL_HEX32(0xFFD764, f);  // warmweiss, dito
  TEST_ASSERT_TRUE(farbe_lesen("Violett", &f));
  TEST_ASSERT_EQUAL_HEX32(0x9B00FF, f);
  TEST_ASSERT_TRUE(farbe_lesen("blau", &f));
  TEST_ASSERT_EQUAL_HEX32(0x0014FF, f);
  TEST_ASSERT_TRUE(farbe_lesen("gr\xc3\xbcn", &f));
  TEST_ASSERT_EQUAL_HEX32(0x00FF00, f);
  TEST_ASSERT_TRUE(farbe_lesen(" ROT ", &f));
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, f);
  f = 0x123456;
  TEST_ASSERT_FALSE(farbe_lesen("gelbgruen", &f));
  TEST_ASSERT_FALSE(farbe_lesen("gelb blau", &f));
  TEST_ASSERT_EQUAL_HEX32(0x123456, f);
}

// ===========================================================================
// Lichtsignale
// ===========================================================================

void test_leuchten_ist_immer_voll() {
  TEST_ASSERT_EQUAL_FLOAT(1.0f, muster_anteil(Muster::leuchten, 0));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, muster_anteil(Muster::leuchten, 123456));
}

void test_pulsieren_beginnt_oben_und_wird_nie_dunkel() {
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, muster_anteil(Muster::pulsieren, 0));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.15f, muster_anteil(Muster::pulsieren, 1200));
  for (uint32_t t = 0; t < 5000; t += 37) {
    const float a = muster_anteil(Muster::pulsieren, t);
    TEST_ASSERT_TRUE(a >= 0.149f && a <= 1.0001f);
  }
}

void test_blinken_halbe_sekunde_an_halbe_aus() {
  TEST_ASSERT_EQUAL_FLOAT(1.0f, muster_anteil(Muster::blinken, 0));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, muster_anteil(Muster::blinken, 499));
  TEST_ASSERT_EQUAL_FLOAT(0.0f, muster_anteil(Muster::blinken, 500));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, muster_anteil(Muster::blinken, 1000));
}

void test_meldung_nur_farbe_leuchtet_bis_quittiert() {
  Meldung m;
  TEST_ASSERT_TRUE(meldung_lesen("gelb", &m));
  TEST_ASSERT_EQUAL_HEX32(0xFF6E00, m.farbe);
  TEST_ASSERT_TRUE(m.muster == Muster::leuchten);
  TEST_ASSERT_EQUAL_UINT32(0, m.dauer_s);
  TEST_ASSERT_TRUE(m.hell < 0.0f);
}

void test_meldung_mit_allem() {
  Meldung m;
  TEST_ASSERT_TRUE(meldung_lesen("#00ff00 pulsieren 600 40", &m));
  TEST_ASSERT_EQUAL_HEX32(0x00FF00, m.farbe);
  TEST_ASSERT_TRUE(m.muster == Muster::pulsieren);
  TEST_ASSERT_EQUAL_UINT32(600, m.dauer_s);
  TEST_ASSERT_EQUAL_FLOAT(40.0f, m.hell);
  // Die optionalen Teile in anderer Reihenfolge.
  TEST_ASSERT_TRUE(meldung_lesen("violett 30 blinken", &m));
  TEST_ASSERT_TRUE(m.muster == Muster::blinken);
  TEST_ASSERT_EQUAL_UINT32(30, m.dauer_s);
}

void test_meldung_laesst_bei_unsinn_alles_stehen() {
  Meldung m;
  m.farbe = 0x010203;
  TEST_ASSERT_FALSE(meldung_lesen("", &m));
  TEST_ASSERT_FALSE(meldung_lesen("lila", &m));               // kein Name
  TEST_ASSERT_FALSE(meldung_lesen("gelb flackern", &m));      // kein Muster
  TEST_ASSERT_FALSE(meldung_lesen("gelb 90000", &m));         // ueber einen Tag
  TEST_ASSERT_FALSE(meldung_lesen("gelb 10 0", &m));          // Helligkeit 0
  TEST_ASSERT_FALSE(meldung_lesen("gelb 10 101", &m));        // ueber 100
  TEST_ASSERT_FALSE(meldung_lesen("gelb 10 50 7", &m));       // eine Zahl zu viel
  TEST_ASSERT_FALSE(meldung_lesen("gelb blinken pulsieren", &m));
  TEST_ASSERT_FALSE(meldung_lesen(nullptr, &m));
  TEST_ASSERT_EQUAL_HEX32(0x010203, m.farbe);
}

// ===========================================================================
// Kollisionsregel (AP 1.5)
// ===========================================================================

void test_referenzfahrt_hat_vorrang_vor_allem() {
  // Die eine Ausnahme von "letzte Aktion gewinnt". Wer der Suchfahrt in den
  // Zaehler schreibt, macht ihr Ergebnis wertlos.
  TEST_ASSERT_TRUE(Fuehrung::homing ==
                   fuehrung_bestimmen(true, true, true, true));
  TEST_ASSERT_TRUE(Fuehrung::homing ==
                   fuehrung_bestimmen(true, false, false, false));
}

void test_hand_schlaegt_timer_und_uhr() {
  TEST_ASSERT_TRUE(Fuehrung::hand ==
                   fuehrung_bestimmen(false, true, true, true));
}

void test_timer_schlaegt_die_uhr() {
  TEST_ASSERT_TRUE(Fuehrung::timer ==
                   fuehrung_bestimmen(false, false, true, true));
}

void test_uhr_fuehrt_wenn_sonst_nichts_laeuft() {
  TEST_ASSERT_TRUE(Fuehrung::uhr ==
                   fuehrung_bestimmen(false, false, false, true));
}

void test_niemand_fuehrt_im_ruhezustand() {
  TEST_ASSERT_TRUE(Fuehrung::niemand ==
                   fuehrung_bestimmen(false, false, false, false));
}


// ===========================================================================
// Umgebungslicht (VEML7700 an J8)
// ===========================================================================

void test_dunkler_als_die_untergrenze_gibt_den_kleinsten_anteil() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f, licht_anteil(0.2f, 1.0f, 300.0f, 0.15f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.15f, licht_anteil(1.0f, 1.0f, 300.0f, 0.15f));
}

void test_heller_als_die_obergrenze_gibt_voll() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, licht_anteil(300.0f, 1.0f, 300.0f, 0.15f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, licht_anteil(50000.0f, 1.0f, 300.0f, 0.15f));
}

void test_ueberblendung_ist_logarithmisch_nicht_linear() {
  // Das Wohnzimmer am Abend, rund 50 lx. Linear ueberblendet laege es bei
  // (50-1)/(300-1) = 16 % der Spanne, logarithmisch bei ln(50)/ln(300) =
  // 68,6 %. Genau dieser Unterschied ist der Grund fuer den Logarithmus:
  // linear waere der ganze Abend in den untersten Prozenten verschwunden.
  const float a = licht_anteil(50.0f, 1.0f, 300.0f, 0.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.686f, a);
  TEST_ASSERT_TRUE(a > 0.5f);  // linear waeren es 0,16
}

void test_anteil_waechst_mit_der_helligkeit() {
  float vorher = -1.0f;
  for (float lux = 0.5f; lux < 1000.0f; lux *= 1.5f) {
    const float a = licht_anteil(lux, 1.0f, 300.0f, 0.15f);
    TEST_ASSERT_TRUE(a >= vorher);
    TEST_ASSERT_TRUE(a >= 0.15f && a <= 1.0f);
    vorher = a;
  }
}

void test_licht_anteil_vertraegt_unsinn() {
  // Verdrehte oder unmoegliche Grenzen: voll aufdrehen, nicht abdunkeln.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, licht_anteil(10.0f, 300.0f, 1.0f, 0.15f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, licht_anteil(10.0f, 0.0f, 300.0f, 0.15f));
  // NaN darf nicht als NaN wieder herauskommen - im Tastverhaeltnis waere das
  // ein dunkler Ausgang ohne jede Meldung.
  const float n = licht_anteil(NAN, 1.0f, 300.0f, 0.15f);
  TEST_ASSERT_FALSE(std::isnan(n));
}

// --- Nachtschalter --------------------------------------------------------

void test_nachtschalter_faengt_an_an() {
  // Eine Uhr, die nach dem Einschalten erst einmal dunkel bleibt, sieht
  // kaputt aus.
  Nachtschalter n(0.5f, 3.0f, 60000);
  TEST_ASSERT_TRUE(n.an());
}

void test_nachtschalter_schaltet_erst_nach_der_frist_ab() {
  Nachtschalter n(0.5f, 3.0f, 60000);
  TEST_ASSERT_TRUE(n.aktualisiere(0.1f, 0));        // dunkel, Frist laeuft an
  TEST_ASSERT_TRUE(n.aktualisiere(0.1f, 59999));    // noch nicht
  TEST_ASSERT_FALSE(n.aktualisiere(0.1f, 60000));   // jetzt
}

void test_kurzer_schatten_schaltet_nicht() {
  // Der Grund fuer die Frist: etwas faellt ueber den Sensor und ist nach
  // zehn Sekunden wieder weg.
  Nachtschalter n(0.5f, 3.0f, 60000);
  n.aktualisiere(100.0f, 0);
  n.aktualisiere(0.1f, 1000);
  n.aktualisiere(0.1f, 10000);
  TEST_ASSERT_TRUE(n.aktualisiere(100.0f, 11000));
  // Und die angefangene Frist ist verfallen, faengt also neu an:
  n.aktualisiere(0.1f, 12000);
  TEST_ASSERT_TRUE(n.aktualisiere(0.1f, 71000));   // 59 s, noch nicht
  TEST_ASSERT_FALSE(n.aktualisiere(0.1f, 72000));  // 60 s
}

void test_hysterese_haelt_im_zwischenbereich_still() {
  // DER EIGENTLICHE ZWECK DER ZWEITEN SCHWELLE. Zwischen 0,5 und 3 lx
  // passiert nichts - in keiner Richtung.
  Nachtschalter n(0.5f, 3.0f, 1000);

  // Erst wirklich abschalten.
  n.aktualisiere(0.1f, 0);
  TEST_ASSERT_FALSE(n.aktualisiere(0.1f, 2000));

  // Jetzt in den Zwischenbereich: bleibt AUS, egal wie lange.
  TEST_ASSERT_FALSE(n.aktualisiere(2.0f, 3000));
  TEST_ASSERT_FALSE(n.aktualisiere(2.0f, 100000));

  // Erst ueber der oberen Schwelle geht sie wieder an.
  n.aktualisiere(5.0f, 101000);
  TEST_ASSERT_TRUE(n.aktualisiere(5.0f, 102000));

  // Und nun andersherum: 2 lx laesst sie AN.
  TEST_ASSERT_TRUE(n.aktualisiere(2.0f, 103000));
  TEST_ASSERT_TRUE(n.aktualisiere(2.0f, 200000));
}

void test_nachtschalter_ueberlebt_den_ueberlauf_von_millis() {
  // millis() laeuft nach 49,7 Tagen ueber. Eine Uhr laeuft laenger als das.
  const uint32_t kurz_vor_ueberlauf = 0xFFFFFFFFu - 30000u;
  Nachtschalter n(0.5f, 3.0f, 60000);
  TEST_ASSERT_TRUE(n.aktualisiere(0.1f, kurz_vor_ueberlauf));
  // 60 s spaeter, ueber den Ueberlauf hinweg: 30 s vor, 30 s nach null.
  TEST_ASSERT_FALSE(n.aktualisiere(0.1f, 30000u));
}

void test_nachtschalter_schaltet_sofort_ein() {
  // Aus-Frist 60 s, An-Frist 0: Licht an heisst Klinge an, mit dem naechsten
  // Messwert.
  Nachtschalter n(0.5f, 3.0f, 60000, 0);
  n.aktualisiere(0.1f, 0);
  TEST_ASSERT_FALSE(n.aktualisiere(0.1f, 60000));   // Nacht: aus
  TEST_ASSERT_TRUE(n.aktualisiere(50.0f, 61000));   // Licht an: sofort an
}

void test_countdown_zum_aus_ueberlebt_werte_knapp_ueber_der_schwelle() {
  // 0,49 / 0,50 / 0,49 ... - der Countdown laeuft weiter, bis 1,125 lx
  // (ein Viertel von 0,5 bis 3).
  Nachtschalter n(0.5f, 3.0f, 60000, 0);
  n.aktualisiere(0.49f, 0);
  n.aktualisiere(0.50f, 20000);
  n.aktualisiere(0.80f, 40000);
  TEST_ASSERT_FALSE(n.aktualisiere(0.49f, 60000));  // nach 60 s aus
  // Deutlich heller dagegen bricht ab.
  Nachtschalter m(0.5f, 3.0f, 60000, 0);
  m.aktualisiere(0.49f, 0);
  m.aktualisiere(1.5f, 20000);
  TEST_ASSERT_TRUE(m.aktualisiere(0.49f, 60000));   // neue Frist, noch an
}

void test_frist_wird_sichtbar_bevor_geschaltet_wird() {
  Nachtschalter n(0.5f, 3.0f, 60000);
  TEST_ASSERT_EQUAL_UINT32(0, n.frist_laeuft_ms(0));
  n.aktualisiere(0.1f, 1000);
  TEST_ASSERT_EQUAL_UINT32(9000, n.frist_laeuft_ms(10000));
  n.aktualisiere(100.0f, 11000);
  TEST_ASSERT_EQUAL_UINT32(0, n.frist_laeuft_ms(11000));
}

// ---------------------------------------------------------------------------
// Feinfahrtende (27.09.2026): die Feinfahrt endet an der Kurve, nicht an einer
// festen Strecke. Kurvenform: Gauss, an C4E8 gemessen 9,85 mT Gipfel, 842
// Schritte breit ueber 1,05 mT -> sigma ~ 199 Schritte.
// ---------------------------------------------------------------------------
static float gauss_mt(int32_t x, float mitte, float gipfel, float sigma, float ruhe) {
  const float d = (static_cast<float>(x) - mitte) / sigma;
  return ruhe + gipfel * std::exp(-0.5f * d * d);
}

// Faehrt die Kurve ab wie die Firmware und gibt die Halteposition zurueck,
// -1 wenn bis `bis` kein Halt kam.
static int32_t halt_bei(float mitte, float gipfel, float sigma, int32_t von, int32_t bis,
                        int32_t *pos, float *wert, size_t *n) {
  starwatch::Feinfahrtende ende(0.54f, 0.20f, 1.05f, 200);
  *n = 0;
  for (int32_t x = von; x <= bis; x++) {
    const float v = gauss_mt(x, mitte, gipfel, sigma, 0.54f);
    if (pos != nullptr) {
      pos[*n] = x;
      wert[*n] = v;
    }
    (*n)++;
    if (ende.fertig(x, v))
      return x;
  }
  return -1;
}

void test_feinfahrt_endet_hinter_der_breiten_kurve() {
  size_t n = 0;
  // Die C4E8-Kurve: 20 % des Gipfels liegen 357 Schritte neben der Mitte.
  const int32_t halt = halt_bei(1000.0f, 9.85f, 199.0f, 330, 3000, nullptr, nullptr, &n);
  TEST_ASSERT_TRUE(halt > 0);
  TEST_ASSERT_INT_WITHIN(3, 1000 + 357, halt);
}

void test_feinfahrt_haelt_beim_schmalen_magneten_den_mindestabstand() {
  size_t n = 0;
  // sigma 40: nach 72 Schritten ist das Feld unter 20 % - die Rechnung
  // braucht aber 150 Schritte je Seite. Der Halt kommt erst 200 hinter dem
  // Gipfel.
  const int32_t halt = halt_bei(1000.0f, 20.0f, 40.0f, 700, 3000, nullptr, nullptr, &n);
  TEST_ASSERT_INT_WITHIN(1, 1200, halt);
}

void test_feinfahrt_haelt_nicht_im_freien_feld() {
  starwatch::Feinfahrtende ende(0.54f, 0.20f, 1.05f, 200);
  bool gehalten = false;
  for (int32_t x = 0; x < 3000; x++)
    gehalten |= ende.fertig(x, 0.54f + ((x % 7) - 3) * 0.02f);
  TEST_ASSERT_FALSE(gehalten);
}

void test_feinfahrt_deckt_den_schwerpunkt_ab() {
  // Start wie die Firmware: 250 vor der Grobflanke (1,05 mT ueber Ruhe,
  // 421 vor der Mitte). Die Rechnung muss danach durchgehen und die Mitte
  // treffen.
  static int32_t pos[4000];
  static float wert[4000];
  size_t n = 0;
  const int32_t halt = halt_bei(1234.0f, 9.85f, 199.0f, 1234 - 421 - 250, 5000, pos, wert, &n);
  TEST_ASSERT_TRUE(halt > 0);
  float mitte = 0.0f;
  TEST_ASSERT_TRUE(starwatch::schwerpunkt_symmetrisch(pos, wert, n, 0.54f, 9.85f * 0.20f,
                                                      150, &mitte));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 1234.0f, mitte);
}

void test_feldbreite_aus_den_punkten() {
  static int32_t pos[2000];
  static float wert[2000];
  size_t n = 0;
  for (int32_t x = 0; x < 2000; x++, n++) {
    pos[n] = x;
    wert[n] = gauss_mt(x, 1000.0f, 9.85f, 199.0f, 0.54f);
  }
  // 1,05 mT ueber Ruhe liegt 421 Schritte neben der Mitte.
  TEST_ASSERT_INT_WITHIN(3, 842,
                         starwatch::feldbreite(pos, wert, n, 0.54f, 1.05f));
  TEST_ASSERT_EQUAL_INT32(0, starwatch::feldbreite(pos, wert, n, 0.54f, 50.0f));
}

// ---------------------------------------------------------------------------
// Lichtautomatik-Einstellungen (27.09.2026)
// ---------------------------------------------------------------------------
void test_weiss_ist_das_weiss_dieser_uhr() {
  // "weiss" folgt der Uhr ('lw', 27.10.2026), ohne Angabe das von C4E8.
  uint32_t f = 0;
  TEST_ASSERT_TRUE(starwatch::farbe_lesen("weiss", &f));
  TEST_ASSERT_EQUAL_HEX32(0xFFD764, f);
  TEST_ASSERT_TRUE(starwatch::farbe_lesen("Wei\xc3\x9f", &f, 0x64FF96));
  TEST_ASSERT_EQUAL_HEX32(0x64FF96, f);
  TEST_ASSERT_TRUE(starwatch::farbe_lesen("rot", &f, 0x64FF96));  // andere bleiben
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, f);
}

void test_lichtautomatik_vorgaben_bleiben_unveraendert() {
  const starwatch::Lichtautomatik v;
  const auto b = starwatch::lichtautomatik_begrenzt(v, 1.0f);
  // Vorgaben seit 27.10.2026: kleinste 30 %, voll ab 20 lx, aus unter 0,2 lx.
  TEST_ASSERT_EQUAL_FLOAT(0.30f, b.anteil_min);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, b.lux_hell);
  TEST_ASSERT_EQUAL_FLOAT(0.2f, b.nacht_aus);
  // Aus unter 0,2, an ueber 0,27 (Faktor 1,35 seit 28.09.2026, vorher 3,0).
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.27f, starwatch::nacht_an_lux(b));
}

void test_einschaltschwelle_liegt_knapp_ueber_der_ausschaltschwelle() {
  // Vorgabe: aus unter 0,3 lx -> an schon bei 0,4 lx, nicht erst bei 1,8.
  starwatch::Lichtautomatik a;
  a.nacht_aus = 0.3f;
  const auto b = starwatch::lichtautomatik_begrenzt(a, 1.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.4f, starwatch::nacht_an_lux(b));
  // Countdown zum Aus bricht erst ab 0,36 lx ab (20 % ueber 0,3).
  starwatch::Nachtschalter n(0.3f, starwatch::nacht_an_lux(b), 30000, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.36f, n.abbruch_lux());
  n.aktualisiere(0.25f, 0);
  n.aktualisiere(0.33f, 10000);                    // Rauschen: zaehlt weiter
  TEST_ASSERT_FALSE(n.aktualisiere(0.25f, 30000)); // nach 30 s aus
  TEST_ASSERT_TRUE(n.aktualisiere(0.42f, 32000));  // Stimmungslicht: sofort an
}

void test_lichtautomatik_klemmt_unsinn() {
  starwatch::Lichtautomatik a;
  a.anteil_min = 0.0f;
  a.lux_hell = 1.0f;
  a.nacht_aus = 500.0f;
  const auto b = starwatch::lichtautomatik_begrenzt(a, 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.05f, b.anteil_min);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, b.lux_hell);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, b.nacht_aus);  // hoechstens halb so hell wie "voll"
  TEST_ASSERT_TRUE(starwatch::nacht_an_lux(b) <= b.lux_hell / 2.0f);
  TEST_ASSERT_TRUE(starwatch::nacht_an_lux(b) >= b.nacht_aus);
}

void test_lichtautomatik_nan_faellt_auf_vorgabe() {
  starwatch::Lichtautomatik a;
  a.anteil_min = NAN;
  a.lux_hell = NAN;
  a.nacht_aus = NAN;
  const auto b = starwatch::lichtautomatik_begrenzt(a, 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.30f, b.anteil_min);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, b.lux_hell);
  TEST_ASSERT_EQUAL_FLOAT(0.2f, b.nacht_aus);
}

void test_nachtschalter_neue_schwellen_wirken() {
  starwatch::Nachtschalter n(0.5f, 3.0f, 1000);
  n.schwellen(5.0f, 30.0f);
  n.aktualisiere(2.0f, 0);
  TEST_ASSERT_FALSE(n.aktualisiere(2.0f, 1500));  // 2 lx liegt jetzt unter "aus"
}

// ---------------------------------------------------------------------------
// Uebergang, Puls, Lichtbefehl (0.20.0)
// ---------------------------------------------------------------------------
void test_uebergang_laeuft_linear_in_beide_richtungen() {
  TEST_ASSERT_EQUAL_FLOAT(80.0f, starwatch::uebergang_wert(80.0f, 20.0f, 0, 2000));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, starwatch::uebergang_wert(80.0f, 20.0f, 1000, 2000));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, starwatch::uebergang_wert(80.0f, 20.0f, 2000, 2000));
  TEST_ASSERT_EQUAL_FLOAT(35.0f, starwatch::uebergang_wert(20.0f, 80.0f, 500, 2000));
  TEST_ASSERT_EQUAL_FLOAT(80.0f, starwatch::uebergang_wert(20.0f, 80.0f, 0, 0));
}

void test_farbe_mischen_ist_je_kanal_linear() {
  TEST_ASSERT_EQUAL_HEX32(0xFFD764, starwatch::farbe_mischen(0xFFD764, 0xFF0000, 0.0f));
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, starwatch::farbe_mischen(0xFFD764, 0xFF0000, 1.0f));
  TEST_ASSERT_EQUAL_HEX32(0x806C32, starwatch::farbe_mischen(0x000000, 0xFFD764, 0.5f));
}

void test_puls_bleibt_zwischen_minimum_und_voll() {
  for (int i = 0; i <= 100; i++) {
    const float a = starwatch::puls_anteil(i / 100.0f, 0.08f);
    TEST_ASSERT_TRUE(a >= 0.08f - 1e-5f && a <= 1.0f + 1e-5f);
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.08f, starwatch::puls_anteil(0.0f, 0.08f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, starwatch::puls_anteil(0.5f, 0.08f));
  // Nie 0, auch wenn 0 verlangt wird.
  TEST_ASSERT_TRUE(starwatch::puls_anteil(0.0f, 0.0f) > 0.0f);
}

void test_puls_phase_trifft_den_gezeigten_wert() {
  for (float a : {0.08f, 0.3f, 0.77f, 1.0f}) {
    const float p = starwatch::puls_phase_fuer(a, 0.08f);
    TEST_ASSERT_TRUE(p >= 0.0f && p <= 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, a, starwatch::puls_anteil(p, 0.08f));
  }
}

void test_puls_periodenwechsel_springt_nicht() {
  float p = 0.2f;
  const float vorher = starwatch::puls_anteil(p, 0.08f);
  p = starwatch::puls_phase_weiter(p, 0, 400);  // neue Periode, keine Zeit vergangen
  TEST_ASSERT_EQUAL_FLOAT(vorher, starwatch::puls_anteil(p, 0.08f));
  p = starwatch::puls_phase_weiter(0.9f, 400, 2000);  // 0,9 + 0,2 = 1,1 -> 0,1
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.1f, p);
  TEST_ASSERT_EQUAL_UINT32(400, starwatch::puls_periode_begrenzt(10));
  TEST_ASSERT_EQUAL_UINT32(4000, starwatch::puls_periode_begrenzt(99999));
}

void test_lichtbefehl_wie_home_assistant() {
  starwatch::Lichtbefehl b;
  TEST_ASSERT_TRUE(starwatch::lichtbefehl_lesen(
      "{\"state\": \"ON\", \"brightness\": 40, \"color\": {\"r\": 255, \"g\": 0, \"b\": 0}, "
      "\"transition\": 2.5}", &b));
  TEST_ASSERT_EQUAL_INT(1, b.state);
  TEST_ASSERT_EQUAL_INT(40, b.hell);
  TEST_ASSERT_TRUE(b.farbe_da);
  TEST_ASSERT_EQUAL_HEX32(0xFF0000, b.farbe);
  TEST_ASSERT_EQUAL_INT32(2500, b.uebergang_ms);
  TEST_ASSERT_EQUAL_INT(-1, b.effekt);
  TEST_ASSERT_EQUAL_INT(0, b.blitze);
}

void test_lichtbefehl_effekt_und_flash() {
  starwatch::Lichtbefehl b;
  starwatch::lichtbefehl_lesen(
      "{\"state\":\"ON\",\"effect\":\"Puls\",\"color\":{\"r\":255,\"g\":0,\"b\":0},\"brightness\":255}", &b);
  TEST_ASSERT_EQUAL_INT(1, b.effekt);
  TEST_ASSERT_EQUAL_INT(100, b.hell);  // brightness_scale 100: mehr gibt es nicht
  starwatch::lichtbefehl_lesen("{\"state\":\"ON\",\"effect\":\"none\"}", &b);
  TEST_ASSERT_EQUAL_INT(0, b.effekt);
  TEST_ASSERT_EQUAL_INT(-1, b.hell);
  TEST_ASSERT_FALSE(b.farbe_da);
  starwatch::lichtbefehl_lesen("{\"state\":\"ON\",\"flash\":\"long\"}", &b);
  TEST_ASSERT_EQUAL_INT(3, b.blitze);
  starwatch::lichtbefehl_lesen("{\"state\":\"OFF\",\"transition\":1}", &b);
  TEST_ASSERT_EQUAL_INT(0, b.state);
  TEST_ASSERT_EQUAL_INT32(1000, b.uebergang_ms);
  // Eine kaputte Farbe wird nicht halb uebernommen.
  starwatch::lichtbefehl_lesen("{\"color\":{\"r\":300,\"g\":0,\"b\":0}}", &b);
  TEST_ASSERT_FALSE(b.farbe_da);
}

// --- Homing-Verlauf (05.10.2026) --------------------------------------------
// Ein Schrittmotor verliert ganze elektrische Perioden (64 Zaehlschritte bei
// 1/16). Die Streuung der Referenzfahrt liegt bei +-3, die Toleranz bei 8.
void test_schrittverlust_streuung_ist_kein_verlust() {
  for (int32_t a : {0, 1, -1, 3, -3, 8, -8}) {
    const Verlustbefund b = schrittverlust(a, 64, 8);
    TEST_ASSERT_FALSE(b.verlust);
    TEST_ASSERT_FALSE(b.periodisch);
    TEST_ASSERT_EQUAL_INT32(0, b.perioden);
  }
}

void test_schrittverlust_ganze_perioden_beide_richtungen() {
  // Der Fall C4E8: rund 380 = 6 Perioden (384) mit 4 Schritten Rest.
  Verlustbefund b = schrittverlust(380, 64, 8);
  TEST_ASSERT_TRUE(b.verlust);
  TEST_ASSERT_TRUE(b.periodisch);
  TEST_ASSERT_EQUAL_INT32(6, b.perioden);
  TEST_ASSERT_EQUAL_INT32(-4, b.rest);

  b = schrittverlust(-130, 64, 8);
  TEST_ASSERT_TRUE(b.periodisch);
  TEST_ASSERT_EQUAL_INT32(-2, b.perioden);
  TEST_ASSERT_EQUAL_INT32(-2, b.rest);
}

void test_schrittverlust_krumme_abweichung_ist_nicht_periodisch() {
  // Halbe Periode: Verlust ja, aber nicht die Handschrift des Motors.
  const Verlustbefund b = schrittverlust(96, 64, 8);
  TEST_ASSERT_TRUE(b.verlust);
  TEST_ASSERT_FALSE(b.periodisch);
  TEST_ASSERT_EQUAL_INT32(32, b.rest < 0 ? -b.rest : b.rest);
}

namespace {
Homingeintrag eintrag(int16_t abw) {
  Homingeintrag e;
  e.abweichung = abw;
  return e;
}
}  // namespace

void test_homingverlauf_neuester_zuerst_und_umlauf() {
  Homingverlauf v;
  TEST_ASSERT_EQUAL_UINT32(0, v.anzahl());
  TEST_ASSERT_EQUAL_INT16(0, v.neuer(0).abweichung);  // leer, kein Absturz

  for (int16_t i = 1; i <= 3; i++)
    v.hinzu(eintrag(i));
  TEST_ASSERT_EQUAL_UINT32(3, v.anzahl());
  TEST_ASSERT_EQUAL_INT16(3, v.neuer(0).abweichung);
  TEST_ASSERT_EQUAL_INT16(1, v.neuer(2).abweichung);
  TEST_ASSERT_EQUAL_INT16(0, v.neuer(3).abweichung);

  // Ueber den Rand: die aeltesten fallen heraus, die Reihenfolge bleibt.
  for (int16_t i = 4; i <= static_cast<int16_t>(Homingverlauf::N) + 5; i++)
    v.hinzu(eintrag(i));
  TEST_ASSERT_EQUAL_UINT32(Homingverlauf::N, v.anzahl());
  TEST_ASSERT_EQUAL_INT16(Homingverlauf::N + 5, v.neuer(0).abweichung);
  TEST_ASSERT_EQUAL_INT16(6, v.neuer(Homingverlauf::N - 1).abweichung);
}

void test_homingverlauf_abbild_hin_und_zurueck() {
  Homingverlauf a;
  for (int16_t i = 1; i <= 30; i++)
    a.hinzu(eintrag(i));
  Homingverlauf b;
  TEST_ASSERT_TRUE(b.laden(a.abbild(), Homingverlauf::abbild_groesse()));
  TEST_ASSERT_EQUAL_UINT32(a.anzahl(), b.anzahl());
  for (size_t i = 0; i < a.anzahl(); i++)
    TEST_ASSERT_EQUAL_INT16(a.neuer(i).abweichung, b.neuer(i).abweichung);
}

void test_homingverlauf_kaputtes_abbild_bleibt_leer() {
  Homingverlauf a;
  a.hinzu(eintrag(7));
  uint8_t roh[Homingverlauf::abbild_groesse()];
  memcpy(roh, a.abbild(), sizeof(roh));

  Homingverlauf b;
  TEST_ASSERT_FALSE(b.laden(roh, sizeof(roh) - 1));  // andere Groesse
  TEST_ASSERT_EQUAL_UINT32(0, b.anzahl());

  uint8_t falsch[sizeof(roh)];
  memcpy(falsch, roh, sizeof(roh));
  falsch[0] = Homingverlauf::VERSION + 1;  // andere Version
  TEST_ASSERT_FALSE(b.laden(falsch, sizeof(falsch)));

  memcpy(falsch, roh, sizeof(roh));
  falsch[2] = 5;  // naechster passt nicht zu anzahl 1
  TEST_ASSERT_FALSE(b.laden(falsch, sizeof(falsch)));
  TEST_ASSERT_EQUAL_UINT32(0, b.anzahl());

  TEST_ASSERT_TRUE(b.laden(roh, sizeof(roh)));
  TEST_ASSERT_EQUAL_INT16(7, b.neuer(0).abweichung);
}

// --- Einrichtung per Bluetooth ---------------------------------------------

void test_netze_json_staerkstes_zuerst_ohne_doppelte() {
  std::vector<Funknetz> n = {{"Gast", -70, true}, {"Voldemodem", -40, false},
                             {"", -30, false}, {"Voldemodem", -60, false},
                             {"A\"b", -50, false}};
  TEST_ASSERT_EQUAL_STRING(
      "[{\"ssid\":\"Voldemodem\",\"rssi\":-40,\"offen\":false},"
      "{\"ssid\":\"A\\\"b\",\"rssi\":-50,\"offen\":false},"
      "{\"ssid\":\"Gast\",\"rssi\":-70,\"offen\":true}]",
      netze_json(n, 512).c_str());
}

void test_netze_json_bleibt_unter_der_grenze() {
  std::vector<Funknetz> n;
  for (int i = 0; i < 40; i++)
    n.push_back({"Netz-mit-langem-Namen-" + std::to_string(i), -40 - i, false});
  const std::string j = netze_json(n, 512);
  TEST_ASSERT_TRUE(j.size() <= 512);
  TEST_ASSERT_EQUAL_CHAR('[', j.front());
  TEST_ASSERT_EQUAL_CHAR(']', j.back());
  TEST_ASSERT_TRUE(j.find("Netz-mit-langem-Namen-0\"") != std::string::npos);  // staerkstes bleibt
  TEST_ASSERT_EQUAL_STRING("[]", netze_json({}, 512).c_str());
}

void test_wlan_auftrag_lesen() {
  std::string s, p;
  TEST_ASSERT_TRUE(wlan_auftrag_lesen("Voldemodem\ngeheim123", &s, &p));
  TEST_ASSERT_EQUAL_STRING("Voldemodem", s.c_str());
  TEST_ASSERT_EQUAL_STRING("geheim123", p.c_str());
  TEST_ASSERT_TRUE(wlan_auftrag_lesen("Gast", &s, &p));  // offenes Netz
  TEST_ASSERT_EQUAL_STRING("", p.c_str());
  TEST_ASSERT_FALSE(wlan_auftrag_lesen("\ngeheim123", &s, &p));
  TEST_ASSERT_FALSE(wlan_auftrag_lesen("Netz\nkurz", &s, &p));
  TEST_ASSERT_FALSE(wlan_auftrag_lesen(std::string(33, 'x') + "\ngeheim123", &s, &p));
  TEST_ASSERT_FALSE(wlan_auftrag_lesen("Netz\n" + std::string(65, 'x'), &s, &p));
}

int main(int, char **) {
  UNITY_BEGIN();

  RUN_TEST(test_normiere_faltet_auch_negative);

  RUN_TEST(test_sollposition_volle_stunden);
  RUN_TEST(test_sollposition_nachmittag_ist_dieselbe_stelle);
  RUN_TEST(test_sollposition_kurz_vor_zwoelf_laeuft_auf_null_um);

  RUN_TEST(test_delta_geht_nie_rueckwaerts);
  RUN_TEST(test_delta_vertraegt_positionen_ausserhalb_der_umdrehung);

  RUN_TEST(test_vorlauf_wird_ausgesessen);

  RUN_TEST(test_drift_hat_ein_vorzeichen);
  RUN_TEST(test_drift_rechnet_gegen_einen_offset);
  RUN_TEST(test_steuern_nimmt_den_kuerzeren_weg);

  RUN_TEST(test_schwerpunkt_findet_die_mitte_einer_symmetrischen_kurve);
  RUN_TEST(test_schwerpunkt_rechnet_mit_dem_betrag);
  RUN_TEST(test_schwerpunkt_meldet_fehlendes_feld);
  RUN_TEST(test_schwerpunkt_ist_gegen_eine_schiefe_flanke_robust);

  RUN_TEST(test_zeigerzeit_kennt_keine_null_uhr);
  RUN_TEST(test_zeigerzeit_volle_stunden);
  RUN_TEST(test_zeigerzeit_letzter_schritt_vor_zwoelf);
  RUN_TEST(test_zeigerzeit_vertraegt_grosse_zaehlerstaende);

  RUN_TEST(test_ein_ganzer_tag_laeuft_nur_vorwaerts);

  // --- AP 2.3: Schwerpunkt waehrend der Fahrt ---
  RUN_TEST(test_sammler_findet_die_mitte);
  RUN_TEST(test_sammler_rechnet_mit_dem_betrag);
  RUN_TEST(test_sammler_meldet_fehlendes_feld);
  RUN_TEST(test_sammler_ist_gegen_eine_schiefe_flanke_robust);
  RUN_TEST(test_sammler_und_array_fassung_stimmen_ueberein);

  // --- Schwerpunkt, symmetrisches Fenster ---
  RUN_TEST(test_symmetrisch_findet_die_mitte);
  RUN_TEST(test_symmetrisch_ist_gegen_einseitigen_schnitt_robust);
  RUN_TEST(test_symmetrisch_uebersteht_einen_ausreisser_am_gipfel);
  RUN_TEST(test_symmetrisch_lehnt_einseitig_unabgedecktes_fenster_ab);
  RUN_TEST(test_symmetrisch_meldet_fehlendes_feld);

  // --- Offset einmessen ---
  RUN_TEST(test_einmessen_macht_die_iststellung_zur_null);
  RUN_TEST(test_einmessen_faltet_negativ);
  RUN_TEST(test_einmessen_schliesst_den_rundlauf);
  RUN_TEST(test_einmessen_ist_beim_zweiten_mal_stabil);

  // --- AP 2.6: Timer ---
  RUN_TEST(test_timer_macht_genau_eine_umdrehung);
  RUN_TEST(test_timer_laeuft_nie_weiter_als_einmal_herum);
  RUN_TEST(test_timer_haelt_bei_zwoelf_stunden_die_aufloesung);
  RUN_TEST(test_timer_ohne_dauer_ist_sofort_um);
  RUN_TEST(test_sprungziel_steht_die_sekunde_und_springt_dann);
  RUN_TEST(test_sprungziel_springt_gleichmaessig_und_nur_vorwaerts);
  RUN_TEST(test_gleittempo_ist_eine_umdrehung_in_der_dauer);
  RUN_TEST(test_gleittempo_uebergeht_einen_schritt_rundung);
  RUN_TEST(test_gleittempo_holt_rueckstand_auf_und_bleibt_in_grenzen);
  RUN_TEST(test_gleittempo_ohne_dauer_ist_null);
  RUN_TEST(test_kein_stroboskop_beim_zuenden_und_einfahren);
  RUN_TEST(test_blende_ohne_effekt);
  RUN_TEST(test_einfahren_flackert_faehrt_ein_und_ist_dann_dunkel);
  RUN_TEST(test_weiche_beschleunigung_gibt_kurzen_wegen_zeit);
  RUN_TEST(test_weicher_einsatz_ist_ein_viertel_mit_deckel);
  RUN_TEST(test_zuendung_beginnt_dunkel_und_endet_genau);
  RUN_TEST(test_zuendung_bleibt_in_grenzen_und_ist_wiederholbar);
  RUN_TEST(test_zuendung_laeuft_nie_ueber_weiss);
  RUN_TEST(test_zuendung_beginnt_waermer_und_endet_in_der_farbe);
  RUN_TEST(test_waermer_verschiebt_den_farbton_richtung_rot);
  RUN_TEST(test_zuendung_zeigt_nur_die_neue_farbe);
  RUN_TEST(test_update_url_alte_namen_werden_umgestellt);
  RUN_TEST(test_update_url_heutige_und_fremde_bleiben);
  RUN_TEST(test_dimm_nachfuehren_geht_weich_und_kommt_an);
  RUN_TEST(test_dimm_nachfuehren_sofort_und_geklemmt);
  RUN_TEST(test_zwei_stufen_setzen_sanfter_ein_und_kommen_an);
  RUN_TEST(test_alias_nimmt_umlaute_und_schneidet_raender);
  RUN_TEST(test_alias_leer_heisst_kein_name);
  RUN_TEST(test_alias_weist_json_und_html_zeichen_ab);
  RUN_TEST(test_alias_zaehlt_zeichen_nicht_bytes);
  RUN_TEST(test_timer_restzeit_rundet_auf);
  RUN_TEST(test_mindestdauer_dreieck_wenn_das_tempo_nicht_reicht);
  RUN_TEST(test_mindestdauer_trapez_wenn_das_tempo_begrenzt);
  RUN_TEST(test_mindestdauer_faellt_mit_mehr_rampe);
  RUN_TEST(test_mindestdauer_die_alte_rampe_konnte_keine_sekunde);
  RUN_TEST(test_mindestdauer_vertraegt_unsinn);
  RUN_TEST(test_fahrzeit_ist_die_mindestdauer_fuer_eine_umdrehung);
  RUN_TEST(test_fahrzeit_zwei_schwungkreise_plus_viertel);
  RUN_TEST(test_fahrzeit_waechst_mit_dem_weg);
  RUN_TEST(test_fahrzeit_langer_weg_ist_fast_reine_fahrt);
  RUN_TEST(test_semver_gleich_ist_null);
  RUN_TEST(test_semver_vergleicht_zahlen_nicht_text);
  RUN_TEST(test_semver_fehlende_stellen_sind_null);
  RUN_TEST(test_semver_anhaengsel_zaehlt_nicht);
  RUN_TEST(test_semver_vertraegt_unsinn);

  // --- AP 2.5: Helligkeit ---
  RUN_TEST(test_null_prozent_ist_wirklich_aus);
  RUN_TEST(test_hundert_prozent_ist_voll);
  RUN_TEST(test_kennlinie_spreizt_auf_die_glimmschwelle);
  RUN_TEST(test_kennlinie_ohne_untergrenze_ist_linear);
  RUN_TEST(test_kennlinie_vertraegt_unsinn);

  // --- Farbe ---
  RUN_TEST(test_farbe_liest_hex_mit_und_ohne_raute);
  RUN_TEST(test_farbe_liest_drei_dezimalzahlen);
  RUN_TEST(test_farbe_laesst_bei_unsinn_alles_stehen);
  RUN_TEST(test_kanaele_folgen_der_farbe_linear);
  RUN_TEST(test_reine_farbe_bei_hundert_prozent_bleibt_ungebremst);
  RUN_TEST(test_zwei_farben_bei_hundert_prozent_bleiben_ungebremst);
  RUN_TEST(test_weiss_wird_auf_die_leistungsgrenze_genommen);
  RUN_TEST(test_begrenzung_behaelt_den_farbton);
  RUN_TEST(test_halbe_helligkeit_liegt_unter_der_grenze);
  RUN_TEST(test_kanaele_vertragen_unsinn);
  RUN_TEST(test_farbe_liest_namen);
  RUN_TEST(test_leuchten_ist_immer_voll);
  RUN_TEST(test_pulsieren_beginnt_oben_und_wird_nie_dunkel);
  RUN_TEST(test_blinken_halbe_sekunde_an_halbe_aus);
  RUN_TEST(test_meldung_nur_farbe_leuchtet_bis_quittiert);
  RUN_TEST(test_meldung_mit_allem);
  RUN_TEST(test_meldung_laesst_bei_unsinn_alles_stehen);

  // --- Umgebungslicht ---
  RUN_TEST(test_dunkler_als_die_untergrenze_gibt_den_kleinsten_anteil);
  RUN_TEST(test_heller_als_die_obergrenze_gibt_voll);
  RUN_TEST(test_ueberblendung_ist_logarithmisch_nicht_linear);
  RUN_TEST(test_anteil_waechst_mit_der_helligkeit);
  RUN_TEST(test_licht_anteil_vertraegt_unsinn);
  RUN_TEST(test_nachtschalter_faengt_an_an);
  RUN_TEST(test_nachtschalter_schaltet_erst_nach_der_frist_ab);
  RUN_TEST(test_kurzer_schatten_schaltet_nicht);
  RUN_TEST(test_hysterese_haelt_im_zwischenbereich_still);
  RUN_TEST(test_nachtschalter_ueberlebt_den_ueberlauf_von_millis);
  RUN_TEST(test_frist_wird_sichtbar_bevor_geschaltet_wird);
  RUN_TEST(test_nachtschalter_schaltet_sofort_ein);
  RUN_TEST(test_countdown_zum_aus_ueberlebt_werte_knapp_ueber_der_schwelle);

  // --- AP 1.5: Kollisionsregel ---
  RUN_TEST(test_referenzfahrt_hat_vorrang_vor_allem);
  RUN_TEST(test_hand_schlaegt_timer_und_uhr);
  RUN_TEST(test_timer_schlaegt_die_uhr);
  RUN_TEST(test_uhr_fuehrt_wenn_sonst_nichts_laeuft);
  RUN_TEST(test_niemand_fuehrt_im_ruhezustand);

  // --- Feinfahrtende (27.09.2026) ---
  RUN_TEST(test_feinfahrt_endet_hinter_der_breiten_kurve);
  RUN_TEST(test_feinfahrt_haelt_beim_schmalen_magneten_den_mindestabstand);
  RUN_TEST(test_feinfahrt_haelt_nicht_im_freien_feld);
  RUN_TEST(test_feinfahrt_deckt_den_schwerpunkt_ab);
  RUN_TEST(test_feldbreite_aus_den_punkten);

  // --- Lichtautomatik-Einstellungen (27.09.2026) ---
  RUN_TEST(test_weiss_ist_das_weiss_dieser_uhr);
  RUN_TEST(test_lichtautomatik_vorgaben_bleiben_unveraendert);
  RUN_TEST(test_lichtautomatik_klemmt_unsinn);
  RUN_TEST(test_einschaltschwelle_liegt_knapp_ueber_der_ausschaltschwelle);
  RUN_TEST(test_lichtautomatik_nan_faellt_auf_vorgabe);
  RUN_TEST(test_nachtschalter_neue_schwellen_wirken);

  // --- Uebergang, Puls, Lichtbefehl (0.20.0) ---
  RUN_TEST(test_uebergang_laeuft_linear_in_beide_richtungen);
  RUN_TEST(test_farbe_mischen_ist_je_kanal_linear);
  RUN_TEST(test_puls_bleibt_zwischen_minimum_und_voll);
  RUN_TEST(test_puls_phase_trifft_den_gezeigten_wert);
  RUN_TEST(test_puls_periodenwechsel_springt_nicht);
  RUN_TEST(test_lichtbefehl_wie_home_assistant);
  RUN_TEST(test_lichtbefehl_effekt_und_flash);
  RUN_TEST(test_schrittverlust_streuung_ist_kein_verlust);
  RUN_TEST(test_schrittverlust_ganze_perioden_beide_richtungen);
  RUN_TEST(test_schrittverlust_krumme_abweichung_ist_nicht_periodisch);
  RUN_TEST(test_homingverlauf_neuester_zuerst_und_umlauf);
  RUN_TEST(test_homingverlauf_abbild_hin_und_zurueck);
  RUN_TEST(test_homingverlauf_kaputtes_abbild_bleibt_leer);

  // --- Einrichtung per Bluetooth (23.10.2026) ---
  RUN_TEST(test_netze_json_staerkstes_zuerst_ohne_doppelte);
  RUN_TEST(test_netze_json_bleibt_unter_der_grenze);
  RUN_TEST(test_wlan_auftrag_lesen);

  return UNITY_END();
}
