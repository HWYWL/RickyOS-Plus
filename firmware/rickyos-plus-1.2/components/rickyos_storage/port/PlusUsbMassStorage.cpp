// Metalio E-Ink4-Plus: S31 HS UTMI, raw SdFat sectors, factory USB route.
// Sector callback logic derived from the pinned FreeInk SDK UsbMassStorage (MIT).
#include "UsbMassStorage.h"
#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <epd_board_metalio_eink4_plus.h>
#include <tinyusb.h>
#include <plus_i2c_wire.h>
#include <metalio_i2c1.h>
#include <tinyusb_default_config.h>

namespace freeink {
namespace {
std::atomic<UsbMassStorage*> gOwner{nullptr};
std::atomic<FsBlockDeviceInterface*> gDev{nullptr};
constexpr uint16_t kBlockSize = 512;
alignas(16) uint8_t gSectorScratch[kBlockSize];
StaticSemaphore_t callbackMutexStorage;
SemaphoreHandle_t callbackMutex = nullptr;
// TinyUSB callbacks are serialized with teardown. Static mutex storage avoids
// new heap allocation; the task may only be stopped after sector I/O has drained.
struct CallbackLock {
 bool held = callbackMutex && xSemaphoreTake(callbackMutex, portMAX_DELAY) == pdTRUE;
 ~CallbackLock() { if (held) xSemaphoreGive(callbackMutex); }
};

bool mediaReady() {
  const auto* owner = gOwner.load();
  if (!owner || !owner->active()) return false;
  const auto state = owner->state();
  return state != UsbMassStorageState::Ejected && state != UsbMassStorageState::IoError;
}
bool isRangeValid(FsBlockDeviceInterface* dev, const uint32_t lba, const uint32_t offset, const uint32_t bufsize) {
  if (!dev || bufsize == 0) return false;
  const uint64_t totalBytes = static_cast<uint64_t>(dev->sectorCount()) * kBlockSize;
  const uint64_t start = static_cast<uint64_t>(lba) * kBlockSize + offset;
  const uint64_t end = start + bufsize;
  return end >= start && end <= totalBytes;
}

int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  auto* const dev = gDev.load();
  auto* const owner = gOwner.load();
  if (!buffer || !isRangeValid(dev, lba, offset, bufsize)) {
    if (owner) owner->markIoError();
    return -1;
  }

  const uint64_t start = static_cast<uint64_t>(lba) * kBlockSize + offset;
  auto* output = static_cast<uint8_t*>(buffer);
  uint32_t remaining = bufsize;
  uint64_t cursor = start;

  if ((offset % kBlockSize) == 0 && (bufsize % kBlockSize) == 0) {
    const size_t sectors = bufsize / kBlockSize;
    if (!dev->readSectors(static_cast<Sector_t>(start / kBlockSize), output, sectors)) {
      if (owner) owner->markIoError();
      return -1;
    }
  } else {
    while (remaining > 0) {
      const auto sector = static_cast<Sector_t>(cursor / kBlockSize);
      const size_t within = static_cast<size_t>(cursor % kBlockSize);
      const size_t count = std::min<size_t>(kBlockSize - within, remaining);
      if (!dev->readSector(sector, gSectorScratch)) {
        if (owner) owner->markIoError();
        return -1;
      }
      memcpy(output, gSectorScratch + within, count);
      output += count;
      cursor += count;
      remaining -= count;
    }
  }

  if (owner) owner->markAccessed();
  return static_cast<int32_t>(bufsize);
}

int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  auto* const dev = gDev.load();
  auto* const owner = gOwner.load();
  if (!buffer || !isRangeValid(dev, lba, offset, bufsize)) {
    if (owner) owner->markIoError();
    return -1;
  }

  const uint64_t start = static_cast<uint64_t>(lba) * kBlockSize + offset;
  auto* input = buffer;
  uint32_t remaining = bufsize;
  uint64_t cursor = start;

  if ((offset % kBlockSize) == 0 && (bufsize % kBlockSize) == 0) {
    const size_t sectors = bufsize / kBlockSize;
    if (!dev->writeSectors(static_cast<Sector_t>(start / kBlockSize), input, sectors)) {
      if (owner) owner->markIoError();
      return -1;
    }
  } else {
    while (remaining > 0) {
      const auto sector = static_cast<Sector_t>(cursor / kBlockSize);
      const size_t within = static_cast<size_t>(cursor % kBlockSize);
      const size_t count = std::min<size_t>(kBlockSize - within, remaining);
      if (!dev->readSector(sector, gSectorScratch)) {
        if (owner) owner->markIoError();
        return -1;
      }
      memcpy(gSectorScratch + within, input, count);
      if (!dev->writeSector(sector, gSectorScratch)) {
        if (owner) owner->markIoError();
        return -1;
      }
      input += count;
      cursor += count;
      remaining -= count;
    }
  }

  if (owner) owner->markAccessed();
  return static_cast<int32_t>(bufsize);
}


bool syncMedia() {
  auto* dev = gDev.load();
  if (dev && dev->syncDevice()) return true;
  if (auto* owner = gOwner.load()) owner->markIoError();
  return false;
}
constexpr uint8_t fsConfig[] = {
 TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_MSC_DESC_LEN,0,100),
 TUD_MSC_DESCRIPTOR(0,0,0x01,0x81,64),
};
constexpr uint8_t hsConfig[] = {
 TUD_CONFIG_DESCRIPTOR(1,1,0,TUD_CONFIG_DESC_LEN+TUD_MSC_DESC_LEN,0,100),
 TUD_MSC_DESCRIPTOR(0,0,0x01,0x81,512),
};
const tusb_desc_device_t deviceDescriptor = [] {
 tusb_desc_device_t d{};
 d.bLength=sizeof(d); d.bDescriptorType=TUSB_DESC_DEVICE; d.bcdUSB=0x0200;
 d.bMaxPacketSize0=CFG_TUD_ENDPOINT0_SIZE;d.idVendor=0x303a;d.idProduct=0x4002;
 d.bcdDevice=0x0100;d.iManufacturer=1;d.iProduct=2;d.iSerialNumber=3;d.bNumConfigurations=1;
 return d;
}();
char serialNumber[13];
const char languageId[]={0x09,0x04};
const char* strings[]={languageId,"Ricky AI Studio","RickyOS Metalio Plus",serialNumber};
}

bool UsbMassStorage::begin(FsBlockDeviceInterface* dev) {
 if (_active || !dev || !dev->sectorCount()) return false;
 if (!callbackMutex) callbackMutex = xSemaphoreCreateMutexStatic(&callbackMutexStorage);
 if (!callbackMutex) return false;
 uint8_t mac[6];
 if (esp_read_mac(mac, ESP_MAC_WIFI_STA)!=ESP_OK) return false;
 snprintf(serialNumber,sizeof(serialNumber),"%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
 gDev.store(dev);gOwner.store(this);
 _state.store(UsbMassStorageState::WaitingForHost);_hostSeen.store(false);_active=true;
 auto config=TINYUSB_DEFAULT_CONFIG(); // S31 HS UTMI, managed PHY and bounded task.
 config.descriptor.device=&deviceDescriptor;
 config.descriptor.string=strings;config.descriptor.string_count=4;
 config.descriptor.full_speed_config=fsConfig;config.descriptor.high_speed_config=hsConfig;
 const esp_err_t error=tinyusb_driver_install(&config);
 if(error!=ESP_OK) {
   _active=false;gOwner.store(nullptr);gDev.store(nullptr);_state.store(UsbMassStorageState::Idle);
   epd_board_metalio_eink4_plus_ana_sw_set(true);
   ESP_LOGE("PlusUSB","install failed: %s",esp_err_to_name(error));return false;
 }
 ESP_LOGI("PlusUSB","S31 HS MSC ready; handing raw SD to host");
 // Switch physical routing while the OTG pull-up is down. Reconnect only
 // after the host has had a chance to release the prior debug device.
 tud_disconnect();
 epd_board_metalio_eink4_plus_ana_sw_set(false);
 delay(150);
 tud_connect();
 return true;
}

void UsbMassStorage::end() {
 if(!_active) return;
 tud_disconnect();
 // Espressif stops its task by deletion. Drain synchronous sector callbacks
 // first and hold the gate through deletion, never interrupt a card transaction.
 if (xSemaphoreTake(callbackMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
   epd_board_metalio_eink4_plus_ana_sw_set(true);
   ESP_LOGE("PlusUSB","sector callback did not drain; reboot before SD remount");
   esp_restart();
   return;
 }
 _active = false;
 const esp_err_t error=tinyusb_driver_uninstall();
 epd_board_metalio_eink4_plus_ana_sw_set(true);
 if(error!=ESP_OK) {
   ESP_LOGE("PlusUSB","teardown failed: %s; rebooting before filesystem access",esp_err_to_name(error));
   xSemaphoreGive(callbackMutex);
   esp_restart(); // Never remount or destroy storage underneath an undrained callback.
   return;
 }
 syncMedia();
 gOwner.store(nullptr);gDev.store(nullptr);
 xSemaphoreGive(callbackMutex);
 _state.store(UsbMassStorageState::Idle);_hostSeen.store(false);
 ESP_LOGI("PlusUSB","raw SD released; USB debug route restored");
}

UsbMassStorageState UsbMassStorage::state() const {
 if(!_active) return UsbMassStorageState::Idle;
 const auto current=_state.load();
 if(current==UsbMassStorageState::Ejected) return current;
 if(!tud_mounted()) return _hostSeen.load()?UsbMassStorageState::Disconnected:UsbMassStorageState::WaitingForHost;
 _hostSeen.store(true);
 if(current==UsbMassStorageState::IoError) return current;
 auto expected=UsbMassStorageState::WaitingForHost;
 _state.compare_exchange_strong(expected,UsbMassStorageState::Connected);
 return _state.load();
}
bool UsbMassStorage::hostConnected() const {auto s=state();return s==UsbMassStorageState::Connected||s==UsbMassStorageState::Accessed;}
bool UsbMassStorage::disconnectHost() const {return _active&&tud_disconnect();}
bool UsbMassStorage::hostSuspended() const {
 if (!_active || !tud_mounted() || !tud_suspended()) return false;
 // A suspended host is not a cable removal. The vendor CX25601N VBUS field
 // is read-only: REG_STATUS1 (0x1e), bits 2:0; 0 means no adapter.
 // Failed/unknown reads keep the session intact, never guess from charge state.
 static uint32_t checked = 0;
 static bool missingVbus = false;
 const uint32_t now = millis();
 if (checked && now - checked < 250) return missingVbus;
 checked = now;
 uint8_t status = 0;
 missingVbus = false;
 if (!metalio_i2c1_lock(20)) return false;
 const auto result = plus_i2c_read(0x6b, 0x1e, &status, 1);
 metalio_i2c1_unlock();
 if (result == ESP_OK) missingVbus = (status & 7) == 0;
 return missingVbus;
}
void UsbMassStorage::markAccessed() const {
 auto current=_state.load();
 while(current!=UsbMassStorageState::Ejected&&current!=UsbMassStorageState::IoError) {
   if(_state.compare_exchange_weak(current,UsbMassStorageState::Accessed))return;
 }
}
void UsbMassStorage::markEjected() const {_state.store(UsbMassStorageState::Ejected);}
void UsbMassStorage::markIoError() const {_state.store(UsbMassStorageState::IoError);}
}

extern "C" {
void tud_msc_inquiry_cb(uint8_t, uint8_t vendor[8], uint8_t product[16], uint8_t revision[4]) {
 memcpy(vendor,"RickyOS ",8);memcpy(product,"Metalio Plus SD  ",16);memcpy(revision,"1.2 ",4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
 freeink::CallbackLock lock;
 if(lun==0 && freeink::mediaReady())return true;
 tud_msc_set_sense(lun,SCSI_SENSE_NOT_READY,0x3a,0);return false;
}
void tud_msc_capacity_cb(uint8_t, uint32_t* count, uint16_t* size) {
 freeink::CallbackLock lock;
 auto* dev=freeink::gDev.load();*count=dev?dev->sectorCount():0;*size=512;
}
bool tud_msc_is_writable_cb(uint8_t lun) {freeink::CallbackLock lock;return lun==0&&freeink::mediaReady();}
int32_t tud_msc_read10_cb(uint8_t lun,uint32_t lba,uint32_t offset,void* buffer,uint32_t size) {
 freeink::CallbackLock lock;
 return lun==0&&freeink::mediaReady()?freeink::mscRead(lba,offset,buffer,size):-1;
}
int32_t tud_msc_write10_cb(uint8_t lun,uint32_t lba,uint32_t offset,uint8_t* buffer,uint32_t size) {
 freeink::CallbackLock lock;
 return lun==0&&freeink::mediaReady()?freeink::mscWrite(lba,offset,buffer,size):-1;
}
bool tud_msc_start_stop_cb(uint8_t lun,uint8_t,bool start,bool eject) {
 freeink::CallbackLock lock;
 if(lun!=0)return false;
 if(!start && !freeink::syncMedia())return false;
 if(eject&&!start)if(auto* owner=freeink::gOwner.load())owner->markEjected();
 return true;
}
int32_t tud_msc_scsi_cb(uint8_t lun,const uint8_t command[16],void* buffer,uint16_t size) {
 freeink::CallbackLock lock;
 if(lun==0) {
  if(command[0]==0x35 || command[0]==0x91)return freeink::syncMedia()?0:-1;
  if(command[0]==0x5a && freeink::mediaReady()) {
   const uint8_t header[8]={0,6,0,0,0,0,0,0};
   const uint16_t count=std::min<uint16_t>(size,sizeof(header));
   if(count && !buffer)return -1;
   if(count)memcpy(buffer,header,count);
   return count;
  }
 }
 tud_msc_set_sense(lun,SCSI_SENSE_ILLEGAL_REQUEST,0x20,0);return -1;
}
}
