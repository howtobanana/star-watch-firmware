// ---------------------------------------------------------------------------
// Star Watch - oeffentliche Schluessel fuer die Pruefung von Update-Images.
// ERZEUGT von werkzeuge/signieren.py - nicht von Hand aendern.
//
// Diese Datei enthaelt KEIN Geheimnis: oeffentliche Schluessel duerfen im
// Repository liegen und in jedem ausgelieferten Geraet stehen.
//
// Der zweite Schluessel ist der Notausgang. Er wird nie zum Signieren
// benutzt und liegt offline - wird der erste kompromittiert, laesst sich
// damit ein Update ausliefern, das den ersten entfernt. Ohne ihn muesste
// jedes Geraet wieder ans Kabel.
// ---------------------------------------------------------------------------
#pragma once

// fw-signaturschluessel.pub.pem
static const char FW_PUBKEY_0[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAELWMiTmvEczwEAY93AW3u4zdc/oE/\n"
    "SrW0dC3OUIbWCR9/KbsC988SVDa2MW2ldwN2fXDFZchmGciRZ9/dQv+6eg==\n"
    "-----END PUBLIC KEY-----\n";
// fw-signaturschluessel-notfall.pub.pem
static const char FW_PUBKEY_1[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEltYenahAnZZhr2ybI4tLG0nQyLI8\n"
    "6JitDquI/frg3ef1vKFMrPelsybf6aVHcfNgH8t+MrSDbLdgKSf+QZHwZQ==\n"
    "-----END PUBLIC KEY-----\n";

static const char *FW_PUBKEYS[] = {FW_PUBKEY_0, FW_PUBKEY_1};
static const size_t FW_PUBKEY_ANZAHL = 2;
