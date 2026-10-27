#include "einrichtung.h"

#include <BLEDevice.h>
#include <BLEServer.h>

#include <mutex>

namespace starwatch {

namespace {

BLECharacteristic *m_netze = nullptr;
BLECharacteristic *m_status = nullptr;

// Der Schreibauftrag kommt im Bluetooth-Task an und wird in loop()
// abgeholt - dazwischen liegt nur dieser Puffer. Ein Mutex, keine
// Spinlock-Sperre: String kopieren heisst allozieren, und das darf nicht in
// einem kritischen Abschnitt passieren.
std::mutex sperre;
String auftrag;
bool auftrag_da = false;

class Wlan : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const String w = c->getValue();
    std::lock_guard<std::mutex> g(sperre);
    auftrag = w;
    auftrag_da = true;
  }
};

class Verbindung : public BLEServerCallbacks {
  // Nach dem Trennen wieder werben - sonst findet die App die Uhr nach
  // einem Abbruch erst nach einem Neustart wieder.
  void onDisconnect(BLEServer *) override { BLEDevice::startAdvertising(); }
};

}  // namespace

void BleEinrichtung::starten(const String &name, const String &netze_json) {
  if (laeuft_ || beendet_)
    return;
  BLEDevice::init(name);
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new Verbindung());
  BLEService *d = srv->createService(DIENST);
  m_netze = d->createCharacteristic(NETZE, BLECharacteristic::PROPERTY_READ);
  m_netze->setValue(netze_json);
  BLECharacteristic *w = d->createCharacteristic(WLAN, BLECharacteristic::PROPERTY_WRITE);
  w->setCallbacks(new Wlan());
  m_status = d->createCharacteristic(
      STATUS, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  m_status->setValue("{\"stand\":\"bereit\"}");
  d->start();
  BLEDevice::getAdvertising()->addServiceUUID(DIENST);
  BLEDevice::startAdvertising();
  laeuft_ = true;
}

void BleEinrichtung::beenden() {
  if (!laeuft_)
    return;
  // true = auch den Speicher des Controllers freigeben. Danach geht kein
  // init() mehr bis zum Neustart - gewollt: die Einrichtung ist vorbei.
  BLEDevice::deinit(true);
  m_netze = m_status = nullptr;
  laeuft_ = false;
  beendet_ = true;
}

void BleEinrichtung::netze(const String &json) {
  if (m_netze != nullptr)
    m_netze->setValue(json);
}

void BleEinrichtung::melden(const String &json) {
  if (m_status == nullptr)
    return;
  m_status->setValue(json);
  m_status->notify();
}

bool BleEinrichtung::auftrag_abholen(String *daten, bool *scan) {
  if (!laeuft_)
    return false;
  String a;
  {
    std::lock_guard<std::mutex> g(sperre);
    if (!auftrag_da)
      return false;
    a = auftrag;
    auftrag_da = false;
  }
  *scan = (a == "?");
  *daten = a;
  return true;
}

}  // namespace starwatch
