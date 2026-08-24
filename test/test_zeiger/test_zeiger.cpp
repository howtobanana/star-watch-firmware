// ---------------------------------------------------------------------------
// Host-Tests der Rechenteile. Laufen ohne Hardware:  pio test -e native
//
// Die Faelle sind nicht ausgedacht. Jeder Block deckt eine Stelle ab, an der
// in der ESPHome-Fassung schon einmal etwas falsch war oder an der es sehr
// leicht falsch wird - Vorzeichen, Umlauf ueber zwoelf Uhr, Rueckwaertsfahrt.
// ---------------------------------------------------------------------------
#include <unity.h>

#include <cmath>

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

  // --- AP 1.5: Kollisionsregel ---
  RUN_TEST(test_referenzfahrt_hat_vorrang_vor_allem);
  RUN_TEST(test_hand_schlaegt_timer_und_uhr);
  RUN_TEST(test_timer_schlaegt_die_uhr);
  RUN_TEST(test_uhr_fuehrt_wenn_sonst_nichts_laeuft);
  RUN_TEST(test_niemand_fuehrt_im_ruhezustand);

  return UNITY_END();
}
