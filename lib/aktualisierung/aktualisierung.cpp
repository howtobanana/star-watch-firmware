#include "aktualisierung.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <Preferences.h>
#include <geraet.h>

#include "fw_pubkeys.h"

namespace starwatch {
namespace g = geraet;
namespace {

/// Eine Zeichenkette aus flachem JSON holen: "schluessel": "wert".
///
/// Wie bei Export/Import in lib/testweb bewusst ohne JSON-Bibliothek: das
/// Manifest ist flach und hat acht bekannte Felder. Die Abhaengigkeit gehoert
/// zu AP 3.4, wo die echte API entsteht.
bool json_text(const String &q, const char *schluessel, std::string *aus) {
  const int k = q.indexOf(String("\"") + schluessel + "\"");
  if (k < 0)
    return false;
  int i = q.indexOf(':', k);
  if (i < 0)
    return false;
  i = q.indexOf('"', i);
  if (i < 0)
    return false;
  const int e = q.indexOf('"', i + 1);
  if (e < 0)
    return false;
  *aus = std::string(q.substring(i + 1, e).c_str());
  return true;
}

bool json_zahl(const String &q, const char *schluessel, long *aus) {
  const int k = q.indexOf(String("\"") + schluessel + "\"");
  if (k < 0)
    return false;
  int i = q.indexOf(':', k);
  if (i < 0)
    return false;
  i++;
  while (i < (int)q.length() && (q[i] == ' ' || q[i] == '\t'))
    i++;
  const int anfang = i;
  while (i < (int)q.length() && q[i] >= '0' && q[i] <= '9')
    i++;
  if (i == anfang)
    return false;
  *aus = q.substring(anfang, i).toInt();
  return true;
}

/// TLS ohne Zertifikatspruefung - die BEGRUENDUNG steht im Kopf von
/// aktualisierung.h und ist keine Bequemlichkeit: die Signatur schuetzt den
/// Inhalt unabhaengig vom Weg, und ein fest eingebautes Wurzelzertifikat
/// laeuft ab, waehrend die Uhr an der Wand haengt.
void tls_vorbereiten(WiFiClientSecure &c) {
  c.setInsecure();
  c.setTimeout(15);
}

/// Eigene Sicht auf den BENUTZER-Bereich (AP 3.1). Eigene `Preferences`, weil
/// dieses Modul nicht an lib/betrieb haengen soll - derselbe Bereich, andere
/// Schluessel.
Preferences nvs;
const char *NVS_BENUTZER = "sw_nutzer";
const char *K_MODUS = "upd_modus";
const char *K_URL = "upd_url";

std::string hex(const uint8_t *b, size_t n) {
  static const char *z = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s.push_back(z[b[i] >> 4]);
    s.push_back(z[b[i] & 0x0F]);
  }
  return s;
}

}  // namespace

const char *updatekanal_text(Updatekanal k) {
  switch (k) {
    case Updatekanal::stabil: return "stabil";
    case Updatekanal::beta:   return "beta";
    case Updatekanal::eigen:  return "eigene URL";
  }
  return "stabil";
}

const char *updatemodus_text(Updatemodus m) {
  switch (m) {
    case Updatemodus::aus:         return "aus";
    case Updatemodus::manuell:     return "manuell";
    case Updatemodus::automatisch: return "automatisch";
  }
  return "aus";
}

void Aktualisierung::beginn(const char *version) {
  version_ = version;
  start_ms_ = millis();

  nvs.begin(NVS_BENUTZER, false);
  const uint8_t m = nvs.getUChar(K_MODUS, static_cast<uint8_t>(Updatemodus::aus));
  modus_ = (m <= 2) ? static_cast<Updatemodus>(m) : Updatemodus::aus;
  url_ = nvs.getString(K_URL, g::UPDATE_URL_VORGABE).c_str();
  if (url_.empty())
    url_ = g::UPDATE_URL_VORGABE;
  // EINMALIGE UMSTELLUNG: die alte Adresse zeigte ins private Repo und lieferte
  // dort immer 404. Wer sie gemerkt hat, faende nie wieder ein Update - der
  // gemerkte Wert gewinnt ja gegen die Vorgabe. Nur DIESE eine Adresse wird
  // ersetzt; eine selbst gesetzte URL bleibt unangetastet.
  if (url_ == g::UPDATE_URL_STILLGELEGT) {
    url_ = g::UPDATE_URL_VORGABE;
    nvs.putString(K_URL, url_.c_str());
  }

  // Laeuft dieses Image auf Bewaehrung? Dann darf es sich NICHT sofort
  // bestaetigen - siehe Kopf der Datei.
  const esp_partition_t *p = esp_ota_get_running_partition();
  esp_ota_img_states_t zustand;
  if (p != nullptr && esp_ota_get_state_partition(p, &zustand) == ESP_OK)
    auf_bewaehrung_ = (zustand == ESP_OTA_IMG_PENDING_VERIFY);
}

std::string Aktualisierung::partition_text() const {
  const esp_partition_t *p = esp_ota_get_running_partition();
  if (p == nullptr)
    return "unbekannt";
  std::string s = p->label;
  esp_ota_img_states_t z;
  if (esp_ota_get_state_partition(p, &z) != ESP_OK)
    return s + ", Zustand nicht lesbar";
  switch (z) {
    case ESP_OTA_IMG_NEW:
      return s + ", NEU (der Bootloader hat sie noch nicht auf Bewaehrung "
                 "gesetzt - Rollback NICHT scharf)";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return s + ", AUF BEWAEHRUNG (Rollback scharf)";
    case ESP_OTA_IMG_VALID:
      return s + ", bestaetigt";
    case ESP_OTA_IMG_INVALID:
      return s + ", als untauglich vermerkt";
    case ESP_OTA_IMG_ABORTED:
      return s + ", abgebrochen";
    default:
      return s + ", undefiniert (nie per OTA beschrieben)";
  }
}

void Aktualisierung::modus(Updatemodus m) {
  modus_ = m;
  nvs.putUChar(K_MODUS, static_cast<uint8_t>(m));
}

void Aktualisierung::url(const std::string &u) {
  url_ = u.empty() ? std::string(g::UPDATE_URL_VORGABE) : u;
  nvs.putString(K_URL, url_.c_str());
}

Updatekanal Aktualisierung::kanal() const {
  if (url_ == g::UPDATE_URL_VORGABE)
    return Updatekanal::stabil;
  if (url_ == g::UPDATE_URL_BETA)
    return Updatekanal::beta;
  return Updatekanal::eigen;
}

void Aktualisierung::kanal(Updatekanal k) {
  switch (k) {
    case Updatekanal::stabil: url(g::UPDATE_URL_VORGABE); break;
    case Updatekanal::beta:   url(g::UPDATE_URL_BETA);    break;
    case Updatekanal::eigen:  break;  // dafuer gibt es url()
  }
}

void Aktualisierung::bewaehrt_melden() {
  if (!auf_bewaehrung_)
    return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
    auf_bewaehrung_ = false;
}

void Aktualisierung::tick(int stunde, bool darf_stoeren) {
  if (modus_ != Updatemodus::automatisch)
    return;
  if (stunde < 0 || !darf_stoeren)
    return;
  // Ausserhalb der Stunde die Marke zuruecksetzen - sonst sperrt sie am
  // naechsten Tag weiter, und aus "taeglich" wird "einmal, nie wieder".
  // Derselbe Fehler steckte in der taeglichen Referenzfahrt; beide am
  // 24.08.2026 behoben, bevor sie je aufgefallen waeren: sie zeigen sich
  // erst am ZWEITEN Tag.
  if (stunde != g::UPDATE_STUNDE) {
    letzte_stunde_ = -1;
    return;
  }
  // Nur EINMAL in dieser Stunde, und nie oefter als der Prueftakt erlaubt.
  if (stunde == letzte_stunde_)
    return;
  if (letzte_pruefung_ms_ != 0 &&
      millis() - letzte_pruefung_ms_ < g::UPDATE_PRUEFTAKT_MS / 2)
    return;
  letzte_stunde_ = stunde;
  letzte_pruefung_ms_ = millis();
  if (pruefen())
    einspielen();
}

bool Aktualisierung::manifest_holen(std::string *roh) {
  WiFiClientSecure tls;
  tls_vorbereiten(tls);
  HTTPClient http;
  if (!http.begin(tls, url_.c_str())) {
    meldung_ = "URL nicht brauchbar: " + url_;
    return false;
  }
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    meldung_ = "Manifest nicht erreichbar (HTTP " + std::to_string(code) + ")";
    http.end();
    return false;
  }
  *roh = std::string(http.getString().c_str());
  http.end();
  return true;
}

bool Aktualisierung::signatur_pruefen(const std::string &block,
                                      const std::string &sig_b64) {
  if (FW_PUBKEY_ANZAHL == 0 || FW_PUBKEYS[0] == nullptr) {
    meldung_ =
        "KEIN SIGNATURSCHLUESSEL einkompiliert - Update abgelehnt. "
        "lib/aktualisierung/fw_pubkeys.h erzeugen, siehe werkzeuge/signieren.py";
    return false;
  }

  // Signatur aus base64
  uint8_t sig[160];
  size_t sig_n = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sig_n,
                            reinterpret_cast<const uint8_t *>(sig_b64.data()),
                            sig_b64.size()) != 0) {
    meldung_ = "Signatur ist kein gueltiges base64";
    return false;
  }

  // SHA-256 ueber den signierten Block
  uint8_t hash[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t *>(block.data()), block.size(),
                 hash, 0);

  // Gegen JEDEN hinterlegten Schluessel pruefen - der zweite ist der
  // Notausgang, siehe fw_pubkeys.h.
  for (size_t i = 0; i < FW_PUBKEY_ANZAHL; i++) {
    if (FW_PUBKEYS[i] == nullptr)
      continue;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    const int gelesen = mbedtls_pk_parse_public_key(
        &pk, reinterpret_cast<const uint8_t *>(FW_PUBKEYS[i]),
        strlen(FW_PUBKEYS[i]) + 1);
    bool ok = false;
    if (gelesen == 0)
      ok = (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig,
                              sig_n) == 0);
    mbedtls_pk_free(&pk);
    if (ok)
      return true;
  }
  meldung_ = "SIGNATUR PASST NICHT. Image wird nicht angefasst.";
  return false;
}

bool Aktualisierung::manifest_pruefen(const std::string &roh) {
  const String q(roh.c_str());

  std::string schema, version, sha, mindest, sig, bild;
  long groesse = 0;
  if (!json_text(q, "schema", &schema) || schema != "starwatch-fw-1") {
    meldung_ = "Fremdes oder fehlendes Schema - kein Star-Watch-Manifest.";
    return false;
  }
  if (!json_text(q, "version", &version) || !json_text(q, "sha256", &sha) ||
      !json_text(q, "mindest", &mindest) || !json_text(q, "signatur", &sig) ||
      !json_text(q, "url", &bild) || !json_zahl(q, "groesse", &groesse)) {
    meldung_ = "Manifest unvollstaendig.";
    return false;
  }

  // DEN BLOCK GENAU SO ZUSAMMENSETZEN wie werkzeuge/signieren.py -
  // Byte fuer Byte, kein JSON, keine Kanonisierung.
  const std::string block = std::string("starwatch-fw-1\n") + "version=" +
                            version + "\n" + "groesse=" +
                            std::to_string(groesse) + "\n" + "sha256=" + sha +
                            "\n" + "mindest=" + mindest + "\n";
  if (!signatur_pruefen(block, sig))
    return false;

  if (semver_vergleich(version.c_str(), version_) <= 0) {
    gefunden_ = version;
    meldung_ = "Schon aktuell (" + std::string(version_) + ").";
    return false;
  }
  if (semver_vergleich(version_, mindest.c_str()) < 0) {
    meldung_ = "Sprung zu weit: diese Fassung verlangt mindestens " + mindest +
               ", hier laeuft " + std::string(version_) + ".";
    return false;
  }

  gefunden_ = version;
  bild_url_ = bild;
  sha_soll_ = sha;
  groesse_ = static_cast<size_t>(groesse);
  return true;
}

bool Aktualisierung::pruefen() {
  meldung_.clear();
  stand_ = Updatestand::laeuft;
  std::string roh;
  if (!manifest_holen(&roh)) {
    stand_ = Updatestand::fehler;
    return false;
  }
  if (!manifest_pruefen(roh)) {
    stand_ = meldung_.rfind("Schon aktuell", 0) == 0 ? Updatestand::aktuell
                                                     : Updatestand::fehler;
    return false;
  }
  stand_ = Updatestand::verfuegbar;
  meldung_ = "Version " + gefunden_ + " steht bereit.";
  return true;
}

bool Aktualisierung::image_laden(const std::string &bild_url, size_t groesse,
                                 const std::string &sha_soll) {
  WiFiClientSecure tls;
  tls_vorbereiten(tls);
  HTTPClient http;
  if (!http.begin(tls, bild_url.c_str())) {
    meldung_ = "Image-URL nicht brauchbar.";
    return false;
  }
  http.setTimeout(20000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    meldung_ = "Image nicht erreichbar (HTTP " + std::to_string(code) + ")";
    http.end();
    return false;
  }
  if (http.getSize() > 0 && (size_t)http.getSize() != groesse) {
    meldung_ = "Groesse passt nicht zum Manifest.";
    http.end();
    return false;
  }
  if (!Update.begin(groesse)) {
    meldung_ = "Kein Platz fuer das Image.";
    http.end();
    return false;
  }

  // WAEHREND des Schreibens hashen: 1,2 MB passen nicht in den Speicher, und
  // erst hinterher aus dem Flash zurueckzulesen waere langsamer und keinen
  // Deut sicherer.
  mbedtls_sha256_context h;
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts(&h, 0);

  // Stream statt des konkreten Typs: der heisst im Core 3.x anders als
  // frueher, und gebraucht werden nur available() und readBytes().
  Stream *strom = http.getStreamPtr();
  uint8_t puffer[1024];
  size_t gesamt = 0;
  uint32_t still_seit = millis();
  while (gesamt < groesse) {
    const size_t da = strom->available();
    if (da == 0) {
      if (millis() - still_seit > 20000) {
        meldung_ = "Abbruch: keine Daten mehr.";
        Update.abort();
        mbedtls_sha256_free(&h);
        http.end();
        return false;
      }
      delay(1);
      continue;
    }
    still_seit = millis();
    const size_t n = strom->readBytes(puffer, da > sizeof(puffer) ? sizeof(puffer) : da);
    if (n == 0)
      continue;
    mbedtls_sha256_update(&h, puffer, n);
    if (Update.write(puffer, n) != n) {
      meldung_ = "Schreibfehler im Flash.";
      Update.abort();
      mbedtls_sha256_free(&h);
      http.end();
      return false;
    }
    gesamt += n;
  }
  http.end();

  uint8_t roh[32];
  mbedtls_sha256_finish(&h, roh);
  mbedtls_sha256_free(&h);
  const std::string ist = hex(roh, sizeof(roh));

  if (ist != sha_soll) {
    // Der Hash steht im SIGNIERTEN Block. Passt er nicht, war entweder der
    // Weg manipuliert oder die Uebertragung kaputt - in beiden Faellen wird
    // die Partition NICHT aktiviert.
    meldung_ = "SHA-256 passt nicht - Image verworfen.";
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    meldung_ = "Abschluss fehlgeschlagen.";
    return false;
  }
  return true;
}

bool Aktualisierung::einspielen() {
  if (stand_ != Updatestand::verfuegbar) {
    meldung_ = "Nichts zum Einspielen - erst pruefen.";
    return false;
  }
  stand_ = Updatestand::laeuft;
  if (!image_laden(bild_url_, groesse_, sha_soll_)) {
    stand_ = Updatestand::fehler;
    return false;
  }
  stand_ = Updatestand::eingespielt;
  meldung_ = "Version " + gefunden_ +
             " eingespielt. Neustart noetig; die neue Fassung laeuft dann auf "
             "Bewaehrung.";
  return true;
}

}  // namespace starwatch
