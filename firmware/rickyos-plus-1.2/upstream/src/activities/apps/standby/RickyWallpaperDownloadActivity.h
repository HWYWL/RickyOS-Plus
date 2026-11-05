#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

// Downloads the RickyOS default standby pictures (github.com/chinoryunqin/RickyOS-wallpapers)
// into /images/待机图片 and, when no standby picture is set yet, installs the first as
// /sleep.bmp. Manifests and files come over the same jsDelivr / fastly / GitHub routes as
// the font catalog. Leaving after any network use restarts to Home, like the font
// downloader, so WiFi and TLS memory is fully reclaimed.
class RickyWallpaperDownloadActivity final : public Activity {
 public:
  RickyWallpaperDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("WallpaperDownload", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class State : uint8_t { Connecting, Loading, Downloading, Done, Failed };
  struct Item {
    std::string name;  // file name on the card, without extension
    std::string file;  // published ASCII file name
    uint32_t size = 0;
    uint32_t crc32 = 0;
    bool sticker = false;  // a transparent sticker, kept apart from the standby pictures
  };

  void onWifiResult(bool connected);
  bool fetchManifest();
  bool downloadAll();
  bool downloadItem(const Item& item, const std::string& destPath);
  void installFirstIfUnset();

  State state_ = State::Connecting;
  std::vector<Item> items_;
  std::string baseUrl_;
  std::vector<std::string> mirrors_;
  std::vector<std::string> hosts_;  // file hosts in try order
  size_t preferredHost_ = 0;        // host that served the last picture
  size_t currentItem_ = 0;
  size_t bytesDone_ = 0;
  size_t bytesTotal_ = 0;
  size_t progressBytes_ = 0;  // bytes finished plus the current file's progress
  uint8_t lastPercent_ = 0;
  bool cancelRequested_ = false;
  bool usedNetwork_ = false;
  uint32_t enteredMs_ = 0;
};

