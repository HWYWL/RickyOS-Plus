#pragma once

#include <ArduinoJson.h>
#include <BoardConfig.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalWifiAwakeLease.h>
#endif
#include <HalStorage.h>
#include <Memory.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

#include "TransferMemory.h"

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    memory::ByteBuffer buffer;
    size_t bufferPos = 0;
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  bool begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }
  bool hasMemoryError() const { return memoryError; }

  WsUploadStatus getWsUploadStatus() const;

  // An HTTP upload holds handleClient() until its body is read, so this is polled on each chunk.
  // Returning true drops the client: the upload aborts and its partial file is removed.
  void setUploadCancelCheck(std::function<bool()> check) { uploadCancelCheck = std::move(check); }

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  memory::ByteBuffer fileListBatch;
  bool running = false;
#if FREEINK_DEVICE_METALIO_PLUS
  HalWifiAwakeLease wifiAwakeLease;
#endif
  bool memoryError = false;
  unsigned long lastMemoryCheck = 0;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;
  std::function<bool()> uploadCancelCheck;
  bool dropUploadIfCancelled() const;
  bool checkMemoryReserve(bool startup);

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // File scanning
  void scanFiles(const char* path, const std::function<bool(const FileInfo&)>& callback) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  // Streams an already-open file to the client in 4KB chunks, feeding the
  // watchdog per write and aborting cleanly (rather than looping past a dead
  // connection) if a write stalls. Caller sets headers/content-length and
  // closes the file.
  void streamFileToClient(HalFile& file) const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontUpload();
  void handleFontUploadData();
  bool flushFontUploadBuffer();
  void abortFontUpload();
  bool commitFontUpload();
  void handleFontDelete();

  // Font upload state
  struct FontUploadState {
    HalFile file;
    std::string familyName;
    std::string filePath;
    std::string temporaryPath;
    bool valid = false;
    bool magicChecked = false;
    bool complete = false;
    bool storageError = false;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    memory::ByteBuffer buffer;
    size_t bufferPos = 0;
  } fontUpload;

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();

  // Missing or malformed JSON sends a 400 response and returns false.
  bool readJsonBody(JsonDocument& out) const;
  void sendJson(const JsonDocument& doc) const;

};
