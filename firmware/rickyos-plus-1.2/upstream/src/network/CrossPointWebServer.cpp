#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#if FREEINK_DEVICE_METALIO_PLUS
#include <HalWifiDiagnostics.h>
#endif
#include <BookKey.h>
#include <Crypto.h>
#include <DeviceSecret.h>
#include <FsHelpers.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIncoming.h>
#include <Logging.h>
#include <Memory.h>
#include <ResumableFetch.h>
#include <SecureHttpClient.h>
#include <Util.h>
#include <WiFi.h>
#include <WolfsslCrypto.h>
#include <base64.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hash.h>
#ifndef SIMULATOR
#include <lwip/sockets.h>
#endif

#include <algorithm>
#include <cctype>
#include <new>
#ifndef SIMULATOR
#include <cerrno>
#endif
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "ProtectedPaths.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
namespace web_zh_cn {
#include "html/zh-CN/FilesPageHtml.generated.h"
#include "html/zh-CN/FontsPageHtml.generated.h"
#include "html/zh-CN/HomePageHtml.generated.h"
#include "html/zh-CN/SettingsPageHtml.generated.h"
}  // namespace web_zh_cn
namespace web_en {
#include "html/en/FilesPageHtml.generated.h"
#include "html/en/FontsPageHtml.generated.h"
#include "html/en/HomePageHtml.generated.h"
#include "html/en/SettingsPageHtml.generated.h"
}  // namespace web_en
#include "WifiPowerSaveGuard.h"
#include "html/js/jszip_minJs.generated.h"
#include "util/BookCacheUtils.h"
#include "util/DictionaryRegistry.h"
#include "util/TaskWatchdog.h"

namespace {
// Arduino's WebServer retains parsed request arguments until the next request.
// For JSON POSTs, that includes the complete "plain" body. Expose a narrowly
// scoped release operation so large request bodies do not remain resident
// between requests, and so outbound TLS can reuse that memory immediately.
class CrossPointHttpServer final : public WebServer {
 public:
  explicit CrossPointHttpServer(uint16_t port) : WebServer(port) {
#if FREEINK_DEVICE_METALIO_PLUS && !defined(SIMULATOR)
    // Short HTML/API replies should not wait for TCP delayed ACKs between
    // their headers and body. NetworkServer applies this to accepted clients.
    _server.setNoDelay(true);
#endif
  }

#if FREEINK_DEVICE_METALIO_PLUS && !defined(SIMULATOR)
  void handleClient() override {
    // Browsers speculatively open extra sockets. The single-client WebServer
    // otherwise lets an empty preconnection block every real request for 5 s.
    // Only expire a socket that has not sent its first request byte: parsed
    // headers, multipart uploads and WebSockets retain their existing limits.
    constexpr unsigned long EMPTY_CONNECTION_MS = 750;
    if (_currentStatus == HC_WAIT_READ && _currentClient.connected() && !_currentClient.available() &&
        millis() - _statusChange >= EMPTY_CONNECTION_MS) {
      _currentClient.stop();
      LOG_INF("WEB", "Expired empty HTTP preconnection");
    }
    WebServer::handleClient();
  }
#endif

  void releaseRequestArguments() {
#ifdef SIMULATOR
    WebServer::releaseRequestArguments();
#else
    if (_currentArgs) {
      delete[] _currentArgs;
      _currentArgs = nullptr;
    }
    _currentArgCount = 0;

    if (_postArgs) {
      delete[] _postArgs;
      _postArgs = nullptr;
    }
    _postArgsLen = 0;
#endif
  }
};

void releaseRequestArguments(WebServer* server) {
  static_cast<CrossPointHttpServer*>(server)->releaseRequestArguments();
}

// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};

// Formats the library index tracks (LibraryIndex isBookName): an upload of any
// of these must mark the index dirty so the next Library entry rebuilds it.
bool isLibraryBookFile(const String& filename) {
  return FsHelpers::hasReflowableBookExtension(std::string_view(filename.c_str(), filename.length()));
}
// A font sent through the plain file upload (not the font installer) still has to reach
// the font list; the registry is otherwise re-scanned only at boot.
bool isFontFile(const String& filename) {
  return FsHelpers::checkFileExtension(filename, ".ttf") || FsHelpers::checkFileExtension(filename, ".otf") ||
         FsHelpers::checkFileExtension(filename, ".ttc") || FsHelpers::checkFileExtension(filename, ".cpfont");
}
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr size_t FILE_LIST_BATCH_CAPACITY = 1400;

bool useChineseWebUi() { return I18N.getLanguage() == Language::ZH_CN; }

struct LocalizedPage {
  const char* data;
  size_t size;
};

LocalizedPage localizedPage(const char* chinese, const size_t chineseSize, const char* english,
                            const size_t englishSize) {
  return useChineseWebUi() ? LocalizedPage{chinese, chineseSize} : LocalizedPage{english, englishSize};
}

memory::ByteBuffer makeWebBuffer(const size_t size) {
  if constexpr (transferMemory::guarded) return transferMemory::allocateBuffer(size);
  // These activity-lifetime, sequential I/O buffers cannot be stack storage.
  // PSRAM keeps them out of the Read Pico panel's scarce internal SRAM; boards
  // without PSRAM retain the existing internal-RAM fallback.
  auto buffer = memory::makePsramByteBufferNoThrow(size);
  if (buffer) return buffer;
  return memory::makeInternalByteBufferNoThrow(size);
}

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
HalFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (name.equals(item)) {
      return true;
    }
  }
  return false;
}

}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

bool CrossPointWebServer::checkMemoryReserve(const bool startup) {
  if constexpr (!transferMemory::guarded) return true;
  const auto heap = HalMemory::getInternalHeap();
  if (transferMemory::healthy(heap, startup)) return true;
  memoryError = true;
  LOG_ERR("NETMEM", "%s refused: internal free/min/largest=%zu/%zu/%zu", startup ? "Startup" : "Service",
          heap.freeBytes, heap.minFreeBytes, heap.largestBlockBytes);
  return false;
}

bool CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return true;
  }
  memoryError = false;
  transferMemory::logSnapshot("before server");
  if (!checkMemoryReserve(true)) return false;

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return false;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server = makeUniqueNoThrow<CrossPointHttpServer>(port);
  if (!server) {
    LOG_ERR("WEB", "OOM: WebServer (%u bytes)", static_cast<unsigned>(sizeof(WebServer)));
    memoryError = true;
    return false;
  }

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
#if FREEINK_DEVICE_METALIO_PLUS
  if (!wifiAwakeLease.acquire()) {
    stop();
    return false;
  }
#else
  WiFi.setSleep(false);
#endif
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  upload.buffer = makeWebBuffer(UploadState::UPLOAD_BUFFER_SIZE);
  fontUpload.buffer = makeWebBuffer(FontUploadState::BUFFER_SIZE);
  if (!upload.buffer || !fontUpload.buffer) {
    LOG_ERR("WEB", "OOM: upload buffers (%u bytes each)", static_cast<unsigned>(UploadState::UPLOAD_BUFFER_SIZE));
    memoryError = true;
    stop();
    return false;
  }

  // Add Access-Control-Allow-* headers to every response so web-based clients
  // and PWAs on other origins can use the HTTP API. Preflight OPTIONS requests
  // are answered in handleNotFound().
  server->enableCORS(true);

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });



  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());
  transferMemory::logSnapshot("after routes");
  if (!checkMemoryReserve(false)) {
    stop();
    return false;
  }

  // Collect WebDAV headers and register handler.
  // If-None-Match is collected so the static-page handlers can answer conditional GETs with 304.
  const char* collectedHeaders[] = {"Depth",      "Destination", "Overwrite",    "If",
                                    "Lock-Token", "Timeout",     "If-None-Match"};
  server->collectHeaders(collectedHeaders, 7);
  auto davHandler = makeUniqueNoThrow<WebDAVHandler>();
  if (!davHandler) {
    LOG_ERR("WEB", "OOM: WebDAVHandler (%u bytes)", static_cast<unsigned>(sizeof(WebDAVHandler)));
    memoryError = true;
    stop();
    return false;
  }
  server->addHandler(davHandler.release());  // WebServer takes ownership.
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer = makeUniqueNoThrow<WebSocketsServer>(wsPort);
  if (!wsServer) {
    LOG_ERR("WEB", "OOM: WebSocketsServer (%u bytes)", static_cast<unsigned>(sizeof(WebSocketsServer)));
    memoryError = true;
    stop();
    return false;
  }
  wsInstance = this;
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  if (!udpActive) {
    LOG_ERR("WEB", "Failed to start discovery UDP on port %d", LOCAL_UDP_PORT);
    stop();
    return false;
  }
  LOG_DBG("WEB", "Discovery UDP enabled on port %d", LOCAL_UDP_PORT);

  // Reuse one request buffer for the server lifetime to avoid repeated heap churn.
  fileListBatch = makeWebBuffer(FILE_LIST_BATCH_CAPACITY);
  if (!fileListBatch) {
    LOG_ERR("WEB", "OOM: %zu-byte file list response buffer", FILE_LIST_BATCH_CAPACITY);
    memoryError = true;
    stop();
    return false;
  }
  transferMemory::logSnapshot("after server");
  if (!checkMemoryReserve(false)) {
    stop();
    return false;
  }

  running = true;
  lastMemoryCheck = millis();

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
  return true;
}



bool CrossPointWebServer::dropUploadIfCancelled() const {
  if (!uploadCancelCheck || !uploadCancelCheck()) return false;
  // WebServer's next read of the body then fails and it raises UPLOAD_FILE_ABORTED.
  server->client().stop();
  return true;
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running && !server && !wsServer && !upload.buffer && !fontUpload.buffer) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    fileListBatch.reset();
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first (was running=%d)", running);
  running = false;  // Set this FIRST to prevent handleClient from using server

  abortFontUpload();
  upload.file.close();
  upload.buffer.reset();
  upload.bufferPos = 0;
  fontUpload.buffer.reset();

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (wsInstance == this) wsInstance = nullptr;

  // begin() can retain its TX buffer when socket creation fails.
  udp.stop();
  udpActive = false;

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  if (server) server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  upload.buffer.reset();
  fontUpload.buffer.reset();
  fileListBatch.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
#if FREEINK_DEVICE_METALIO_PLUS
  wifiAwakeLease.release();
#endif
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  if constexpr (transferMemory::guarded) {
    if (millis() - lastMemoryCheck >= 250) {
      lastMemoryCheck = millis();
      if (!checkMemoryReserve(false)) {
        stop();
        return;
      }
    }
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
#if FREEINK_DEVICE_METALIO_PLUS
    plusWifiDiagnosticsSample();
#endif
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();
  // WebServer otherwise keeps the last request's argument strings allocated
  // until another request arrives. They are no longer observable once its
  // handler returns, so release them now instead of retaining a JSON body.
  releaseRequestArguments(server.get());

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

#ifndef SIMULATOR
static bool writeClientWithDeadline(NetworkClient& client, const char* data, size_t length,
                                    unsigned long responseStart) {
  constexpr unsigned long SEND_STALL_MS = 10000;
  constexpr unsigned long RESPONSE_DEADLINE_MS = 60000;
  size_t offset = 0;
  unsigned long lastProgress = millis();
  int lastSocketError = 0;

  while (offset < length) {
    const unsigned long now = millis();
    if (now - lastProgress >= SEND_STALL_MS || now - responseStart >= RESPONSE_DEADLINE_MS) {
#if FREEINK_DEVICE_METALIO_PLUS
      const auto heap = HalMemory::getInternalHeap();
      LOG_ERR("WEB", "Send deadline: chunk=%u/%u errno=%d internal=%u largest=%u", static_cast<unsigned>(offset),
              static_cast<unsigned>(length), lastSocketError, static_cast<unsigned>(heap.freeBytes),
              static_cast<unsigned>(heap.largestBlockBytes));
#endif
      return false;
    }

    const int socketFd = client.fd();
    if (socketFd < 0) return false;

    const ssize_t written = ::send(socketFd, data + offset, length - offset, MSG_DONTWAIT);
    if (written < 0) lastSocketError = errno;
    if (written > 0) {
      offset += static_cast<size_t>(written);
      lastProgress = millis();
      continue;
    }
    if (written == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ENOMEM)) {
      return false;
    }
    delay(1);
  }
  return true;
}
#endif

static bool sendCompressedContent(WebServer* server, const char* contentType, const char* data, size_t len) {
#ifdef SIMULATOR
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, contentType, data, len);
  return true;
#else
  constexpr size_t CHUNK_SIZE = 1400;
  server->sendHeader("Content-Encoding", "gzip");
  server->setContentLength(len);

  const unsigned long responseStart = millis();
  server->send(200, contentType, "");
  if (!server->client().connected()) {
    LOG_ERR("WEB", "Aborting %s response header after %lu ms", server->uri().c_str(), millis() - responseStart);
    server->client().stop();
    return false;
  }

  for (size_t offset = 0; offset < len; offset += CHUNK_SIZE) {
    const size_t chunkLength = std::min(CHUNK_SIZE, len - offset);
    if (!writeClientWithDeadline(server->client(), data + offset, chunkLength, responseStart)) {
      LOG_ERR("WEB", "Aborting %s response body at %u/%u after %lu ms", server->uri().c_str(),
              static_cast<unsigned>(offset), static_cast<unsigned>(len), millis() - responseStart);
      server->client().stop();
      return false;
    }
  }
#if FREEINK_DEVICE_METALIO_PLUS
  LOG_INF("WEBHTTP", "page=%s bytes=%u send_ms=%lu", server->uri().c_str(), static_cast<unsigned>(len),
          millis() - responseStart);
#endif
  return true;
#endif
}

// Canonical local address keeps portal pages out of the probe site's origin.
// Short response-header Strings only; no resident buffers or network task.
static void redirectToHotspot(WebServer* server, const uint16_t port) {
  String target = "http://" + WiFi.softAPIP().toString();
  if (port != 80) target += ":" + String(port);
  target += "/";
  server->sendHeader("Cache-Control", "no-store");
  server->sendHeader("Location", target, true);
  server->send(302, "text/plain", "");
}

void CrossPointWebServer::handleRoot() const {
#ifndef SIMULATOR
  if (apMode) {
    const String host = server->hostHeader();
    const String address = WiFi.softAPIP().toString();
    if (!host.isEmpty() && host != address && host != address + ":" + String(port)) {
      redirectToHotspot(server.get(), port);
      return;
    }
  }
#endif
  const auto page = localizedPage(web_zh_cn::HomePageHtml, sizeof(web_zh_cn::HomePageHtml), web_en::HomePageHtml,
                                  sizeof(web_en::HomePageHtml));
  if (sendCompressedContent(server.get(), "text/html", page.data, page.size)) {
    LOG_DBG("WEB", "Served root page");
  }
}

void CrossPointWebServer::handleJszip() const {
  if (sendCompressedContent(server.get(), "application/javascript", jszip_minJs, jszip_minJsCompressedSize)) {
    LOG_DBG("WEB", "Served jszip.min.js");
  }
}

void CrossPointWebServer::handleNotFound() const {
  // CORS preflight: routes are registered per-method, so OPTIONS requests land
  // here. The Access-Control-Allow-* headers are added by enableCORS().
  if (server->method() == HTTP_OPTIONS) {
    server->send(204, "text/plain", "");
    return;
  }

  // in AP mode, redirect unmatched browser/captive-portal requests to "/" so the OS auto-opens the browser
  // API requests (/api/*) still return 404 so XHR errors surface correctly
  // see https://en.wikipedia.org/wiki/Captive_portal#Detection
  if (apMode && !server->uri().startsWith("/api/")) {
    redirectToHotspot(server.get(), port);
    return;
  }

  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = BoardConfig::ACTIVE.name;

  char snBuf[33] = {0};
  bool valid = false;
#if !CONFIG_IDF_TARGET_ESP32
  // Classic ESP32's efuse table has no USER_DATA block (C3/S3 only)
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }
#endif

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<bool(const FileInfo&)>& callback) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    bool keepScanning = true;
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      keepScanning = callback(info);
    }

    file.close();
    if (!keepScanning) break;
    yield();                          // Yield to allow WiFi and other tasks to process during long scans
    resetTaskWatchdogIfSubscribed();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  const auto page = localizedPage(web_zh_cn::FilesPageHtml, sizeof(web_zh_cn::FilesPageHtml), web_en::FilesPageHtml,
                                  sizeof(web_en::FilesPageHtml));
  sendCompressedContent(server.get(), "text/html", page.data, page.size);
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (!fileListBatch) {
    LOG_ERR("WEB", "Cannot serve file list: %zu-byte response buffer unavailable", FILE_LIST_BATCH_CAPACITY);
    server->send(503, "application/json", "{\"error\":\"Insufficient memory\"}");
    return;
  }

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;
  const auto serializeFileInfo = [&](const FileInfo& info) {
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;

    const size_t measured = measureJson(doc);
    if (measured >= outputSize) {
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
      return static_cast<size_t>(0);
    }

    const size_t written = serializeJson(doc, output, outputSize);
    return written;
  };

  const unsigned long responseStart = millis();
#ifdef SIMULATOR
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
#else
  server->chunkResponseBegin("application/json");
#endif

  char* batch = reinterpret_cast<char*>(fileListBatch.get());
  size_t batchLen = 0;
  size_t entryCount = 0;
  size_t responseBytes = 0;
  bool responseOpen = server->client().connected();
  const auto flushBatch = [&]() {
    if (batchLen == 0) return true;
#ifdef SIMULATOR
    server->sendContent(batch, batchLen);
#else
    server->chunkWrite(batch, batchLen);
#endif
    responseBytes += batchLen;
    batchLen = 0;
    responseOpen = server->client().connected();
    return responseOpen;
  };

  batch[batchLen++] = '[';

  if (responseOpen)
    scanFiles(currentPath.c_str(), [&](const FileInfo& info) {
      if (!server->client().connected()) {
        responseOpen = false;
        return false;
      }
      const size_t written = serializeFileInfo(info);
      if (written == 0) return true;

      const size_t required = written + (entryCount > 0 ? 1 : 0);
      if (batchLen + required > FILE_LIST_BATCH_CAPACITY && !flushBatch()) return false;
      if (entryCount > 0) batch[batchLen++] = ',';
      memcpy(batch + batchLen, output, written);
      batchLen += written;
      ++entryCount;
      return true;
    });

  if (responseOpen) {
    if (batchLen + 1 > FILE_LIST_BATCH_CAPACITY) flushBatch();
  }
  if (responseOpen) {
    batch[batchLen++] = ']';
    flushBatch();
  }

  // Always finish the framework's chunked-response state, including disconnects.
#ifdef SIMULATOR
  server->sendContent("");
#else
  server->chunkResponseEnd();
#endif
  const unsigned long elapsed = millis() - responseStart;
  if (responseOpen) {
    LOG_INF("WEB", "Served %zu file entries (%zu bytes) in %lu ms", entryCount, responseBytes, elapsed);
  } else {
    LOG_ERR("WEB", "File list client disconnected after %zu entries and %lu ms", entryCount, elapsed);
  }
}

void CrossPointWebServer::streamFileToClient(HalFile& file) const {
  NetworkClient client = server->client();
  static constexpr size_t CHUNK_SIZE = 4096;
  // Off the stack: the web-server task also runs TLS and SD from this stack.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(CHUNK_SIZE);
  if (!buffer) {
    LOG_ERR("WEB", "OOM: %u byte stream buffer", (unsigned)CHUNK_SIZE);
    return;
  }

  bool ok = true;
  while (ok && file.available()) {
    const int result = file.read(buffer.get(), CHUNK_SIZE);
    if (result <= 0) break;
    const size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      resetTaskWatchdogIfSubscribed();
      const size_t wrote = client.write(buffer.get() + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        ok = false;
        break;
      }
      totalWritten += wrote;
    }
  }
  client.clear();
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".") || protectedpaths::isSensitivePath(itemPath.c_str())) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (itemName.equals(item)) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");
  streamFileToClient(file);
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    resetTaskWatchdogIfSubscribed();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.get(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    resetTaskWatchdogIfSubscribed();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

// Drop a partially written upload so a truncated book never lands in the library.
// The file is new: uploads refuse to overwrite an existing name.
static void removeUploadedFile(const CrossPointWebServer::UploadState& state) {
  String filePath = state.path;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += state.fileName;
  Storage.remove(filePath.c_str());
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  resetTaskWatchdogIfSubscribed();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    resetTaskWatchdogIfSubscribed();

    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;
    state.buffer.reset();

    if (!FsHelpers::isSafePathComponent(state.fileName)) {
      state.error = "Invalid file name";
      LOG_DBG("WEB", "[UPLOAD] Rejected unsafe filename: %s", state.fileName.c_str());
      return;
    }

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;
    if (protectedpaths::isSensitivePath(filePath.c_str())) {
      state.error = "Cannot write protected items";
      return;
    }

    // Check if file already exists - SD operations can be slow
    resetTaskWatchdogIfSubscribed();
    if (Storage.exists(filePath.c_str())) {
      state.error = "File already exists: " + state.fileName;
      LOG_DBG("WEB", "[UPLOAD] Collision: %s", filePath.c_str());
      return;
    }

    // The buffer spans upload callbacks; keep it off the task stack and release it at the end.
    state.buffer = makeWebBuffer(UploadState::UPLOAD_BUFFER_SIZE);
    if (!state.buffer) {
      state.error = tr(STR_MEMORY_ERROR);
      LOG_ERR("WEB", "OOM: upload buffer");
      return;
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    resetTaskWatchdogIfSubscribed();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      state.buffer.reset();
      return;
    }
    resetTaskWatchdogIfSubscribed();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (dropUploadIfCancelled()) return;
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.get() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            state.buffer.reset();
            removeUploadedFile(state);
            return;
          }
        }
      }

      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!server->client().connected()) return;
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();
      if (!state.error.isEmpty()) removeUploadedFile(state);

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache after uploading the file
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearBookCache(filePath.c_str());
        if (isLibraryBookFile(state.fileName)) {
          library::LibraryIncoming::add(filePath.c_str());
          library::markLibraryIndexDirty();
        }
        if (isFontFile(state.fileName)) sdFontSystem.markRegistryDirty();
      }
    }
    state.buffer.reset();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    state.buffer.reset();
    if (state.file) {
      state.file.close();
      removeUploadedFile(state);
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }
  if (!FsHelpers::isSafePathComponent(folderName)) {
    LOG_DBG("WEB", "Rejected unsafe folder name: %s", folderName.c_str());
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }
  if (isProtectedItemName(folderName)) {
    LOG_DBG("WEB", "Rejected protected folder name: %s", folderName.c_str());
    server->send(403, "text/plain", "Cannot create protected item");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;
  if (protectedpaths::isSensitivePath(itemPath.c_str()) || protectedpaths::isSensitivePath(newPath.c_str())) {
    file.close();
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) {
      server->send(403, "text/plain", "Cannot move into protected folder");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;
  if (protectedpaths::isSensitivePath(itemPath.c_str()) || protectedpaths::isSensitivePath(newPath.c_str())) {
    file.close();
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = normalizeWebPath(p.as<String>());

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Security check: prevent deletion of protected items
    const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

    // Hidden/system files and credential stores are protected
    if (itemName.startsWith(".") || protectedpaths::isSensitivePath(itemPath.c_str())) {
      failedItems += itemPath + " (hidden/system file); ";
      allSuccess = false;
      continue;
    }

    // Check against explicitly protected items
    bool isProtected = false;
    for (const auto* item : HIDDEN_ITEMS) {
      if (itemName.equals(item)) {
        isProtected = true;
        break;
      }
    }
    if (isProtected) {
      failedItems += itemPath + " (protected file); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      HalFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  const auto page = localizedPage(web_zh_cn::SettingsPageHtml, sizeof(web_zh_cn::SettingsPageHtml),
                                  web_en::SettingsPageHtml, sizeof(web_en::SettingsPageHtml));
  if (sendCompressedContent(server.get(), "text/html", page.data, page.size)) {
    LOG_DBG("WEB", "Served settings page");
  }
}

void CrossPointWebServer::handleGetSettings() const {
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  forEachSettingsListEntry(&sdFontSystem.registry(), [this, &doc, &output, &seenFirst](const SettingInfo& s) {
    if (!s.key) return;  // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumLabels()) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.signedValuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.signedValuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.obfuscated) {
          // Device-bound credentials may contain invalid text after an SD card move.
          doc["value"] = nullptr;
          doc["hasPassword"] =
              s.stringGetter
                  ? !s.stringGetter().empty()
                  : s.stringMaxLen > 0 && *(reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset) != '\0';
        } else if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        return;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      return;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
    yield();                          // Yield to allow WiFi and other tasks to process during a slow send
    resetTaskWatchdogIfSubscribed();  // Reset watchdog: each sendContent() is a blocking network write
  });

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  int applied = 0;

  forEachSettingsListEntry(&sdFontSystem.registry(), [&doc, &applied](const SettingInfo& s) {
    if (!s.key) return;
    if (!doc[s.key].is<JsonVariant>()) return;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumLabels().size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.signedValuePtr) {
            SETTINGS.*(s.signedValuePtr) = static_cast<int8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  });

  SETTINGS.saveToFile();
  Frontlight.setWarmth(SETTINGS.frontlightWarmth);
  Frontlight.setBrightness(SETTINGS.frontlightBrightness);

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// ---- OPDS Server API ----




// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto credentials = WIFI_STORE.getCredentialSummaries();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = credentials[i].hasPassword;
    doc["isLastConnected"] = credentials[i].isLastConnected;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    yield();                          // Yield to allow WiFi and other tasks to process during a slow send
    resetTaskWatchdogIfSubscribed();  // Reset watchdog: each sendContent() is a blocking network write
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served Wi-Fi credentials API (%zu network(s))", credentials.size());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }
    const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
    if (!credential) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credential->ssid;
    if (!hasPasswordField) {
      password = credential->password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }
  const auto ssid = WIFI_STORE.getSsidAt(static_cast<size_t>(idx));
  if (!ssid) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  if (!WIFI_STORE.removeCredential(*ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid->c_str());
  server->send(200, "text/plain", "OK");
}

bool CrossPointWebServer::readJsonBody(JsonDocument& out) const {
  if (!server->hasArg("plain")) {
    server->send(400, "application/json", "{\"error\":\"missing body\"}");
    return false;
  }
  if (deserializeJson(out, server->arg("plain")) != DeserializationError::Ok) {
    server->send(400, "application/json", "{\"error\":\"bad json\"}");
    return false;
  }
  return true;
}

void CrossPointWebServer::sendJson(const JsonDocument& doc) const {
  String out;
  if (!out.reserve(measureJson(doc))) {
    LOG_ERR("WEB", "OOM: JSON response");
    server->send(503, "application/json", "{\"error\":\"out of memory\"}");
    return;
  }
  serializeJson(doc, out);
  server->send(200, "application/json", out);
}




namespace {}  // namespace












// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          if (!FsHelpers::isSafePathComponent(wsUploadFileName)) {
            LOG_DBG("WS", "START rejected: invalid filename '%s'", wsUploadFileName.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid file name");
            return;
          }
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;
          if (protectedpaths::isSensitivePath(filePath.c_str())) {
            wsServer->sendTXT(num, "ERROR:Cannot write protected items");
            return;
          }

          resetTaskWatchdogIfSubscribed();
          if (Storage.exists(filePath.c_str())) {
            LOG_DBG("WS", "Upload collision: %s", filePath.c_str());
            wsServer->sendTXT(num, "ERROR:File already exists: " + wsUploadFileName);
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Open file for writing
          resetTaskWatchdogIfSubscribed();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          resetTaskWatchdogIfSubscribed();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCache(filePath.c_str());
            if (isLibraryBookFile(wsUploadFileName)) {
              library::LibraryIncoming::add(filePath.c_str());
              library::markLibraryIndexDirty();
            }
            if (isFontFile(wsUploadFileName)) sdFontSystem.markRegistryDirty();
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      resetTaskWatchdogIfSubscribed();
      size_t written = wsUploadFile.write(payload, length);
      resetTaskWatchdogIfSubscribed();

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache after uploading the file
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCache(filePath.c_str());
        if (isLibraryBookFile(wsUploadFileName)) {
              library::LibraryIncoming::add(filePath.c_str());
              library::markLibraryIndexDirty();
            }
        if (isFontFile(wsUploadFileName)) sdFontSystem.markRegistryDirty();

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  const auto page = localizedPage(web_zh_cn::FontsPageHtml, sizeof(web_zh_cn::FontsPageHtml), web_en::FontsPageHtml,
                                  sizeof(web_en::FontsPageHtml));
  if (sendCompressedContent(server.get(), "text/html", page.data, page.size)) {
    LOG_DBG("WEB", "Served fonts page");
  }
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  const_cast<SdCardFontSystem&>(sdFontSystem).refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      // Stat the file for size
      HalFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  sendJson(doc);
}

bool CrossPointWebServer::flushFontUploadBuffer() {
  if (!fontUpload.valid) return false;
  if (fontUpload.bufferPos == 0) return true;
  const size_t expected = fontUpload.bufferPos;
  const size_t written = fontUpload.file.write(fontUpload.buffer.get(), expected);
  fontUpload.bytesWritten += written;
  fontUpload.bufferPos = 0;
  if (written != expected) {
    LOG_ERR("WEB", "Font SD write failed: %zu/%zu bytes", written, expected);
    fontUpload.valid = false;
    fontUpload.storageError = true;
    return false;
  }
  return true;
}

void CrossPointWebServer::abortFontUpload() {
  fontUpload.file.close();
  // Only remove the temporary file owned by this request, never the installed font.
  if (!fontUpload.temporaryPath.empty()) {
    if (!Storage.remove(fontUpload.temporaryPath.c_str())) {
      LOG_ERR("WEB", "Cannot remove partial font: %s", fontUpload.temporaryPath.c_str());
    }
    fontUpload.temporaryPath.clear();
  }
  fontUpload.valid = false;
  fontUpload.complete = false;
  fontUpload.bufferPos = 0;
}

bool CrossPointWebServer::commitFontUpload() {
  // FAT renames are not a power-loss transaction. Keep the old file until the
  // completed upload is promoted, and leave its backup recoverable if rollback fails.
  const std::string backupPath = fontUpload.filePath + ".upload-backup";
  if (Storage.exists(backupPath.c_str())) {
    LOG_ERR("WEB", "Unresolved font backup: %s", backupPath.c_str());
    return false;
  }
  const bool replacing = Storage.exists(fontUpload.filePath.c_str());
  if (replacing && !Storage.rename(fontUpload.filePath.c_str(), backupPath.c_str())) return false;
  if (!Storage.rename(fontUpload.temporaryPath.c_str(), fontUpload.filePath.c_str())) {
    if (replacing && !Storage.rename(backupPath.c_str(), fontUpload.filePath.c_str())) {
      LOG_ERR("WEB", "Font rollback failed; original retained at %s", backupPath.c_str());
    }
    return false;
  }
  fontUpload.temporaryPath.clear();
  if (replacing && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("WEB", "Installed font; cannot remove old backup: %s", backupPath.c_str());
  }
  return true;
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      resetTaskWatchdogIfSubscribed();
      String family = server->arg("family");
      abortFontUpload();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.storageError = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;
      fontUpload.buffer.reset();

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a .cpfont basename of alphanumeric + hyphen + underscore. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      // Bound paths before directory creation: buildFontPath uses a fixed 128-byte buffer.
      const char* root = SdCardFontRegistry::findFamilyRoot(family.c_str());
      if (!root) root = SdCardFontRegistry::defaultWriteRoot();
      if (strlen(root) + family.length() + filename.length() + 2 >= 128) {
        LOG_ERR("WEB", "Font upload path too long");
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        fontUpload.storageError = true;
        break;
      }

      fontUpload.buffer = makeWebBuffer(FontUploadState::BUFFER_SIZE);
      if (!fontUpload.buffer) {
        LOG_ERR("WEB", "OOM: font upload buffer");
        break;
      }

      char path[128];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;

      const std::string temporaryPath = fontUpload.filePath + ".upload-part";
      if (Storage.exists(temporaryPath.c_str()) ||
          !Storage.openFileForWrite("WEB", temporaryPath.c_str(), fontUpload.file)) {
        LOG_ERR("WEB", "Cannot create temporary font file: %s", temporaryPath.c_str());
        fontUpload.storageError = true;
        break;
      }
      fontUpload.temporaryPath = temporaryPath;

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (dropUploadIfCancelled() || !fontUpload.valid) break;
      resetTaskWatchdogIfSubscribed();

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.get() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        // Check the buffered prefix, even if the first network chunk was <8 bytes.
        if (!fontUpload.magicChecked && fontUpload.bufferPos >= 8) {
          if (memcmp(fontUpload.buffer.get(), "CPFONT\0\0", 8) != 0) {
            LOG_ERR("WEB", "Invalid .cpfont magic bytes");
            fontUpload.valid = false;
            break;
          }
          fontUpload.magicChecked = true;
        }

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          if (!flushFontUploadBuffer()) break;
          resetTaskWatchdogIfSubscribed();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      // Flush remaining buffer
      if (fontUpload.valid) flushFontUploadBuffer();
      if (fontUpload.file.isOpen()) {
        fontUpload.file.close();
      }
      fontUpload.bufferPos = 0;
      fontUpload.buffer.reset();

      if (!fontUpload.magicChecked || fontUpload.bytesWritten != upload.totalSize) fontUpload.valid = false;
      if (fontUpload.valid && !commitFontUpload()) {
        fontUpload.valid = false;
        fontUpload.storageError = true;
      }
      if (!fontUpload.valid) abortFontUpload();
      fontUpload.complete = fontUpload.valid;

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      abortFontUpload();
      fontUpload.buffer.reset();
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid && fontUpload.complete) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else if (fontUpload.storageError) {
    server->send(500, "application/json", "{\"error\":\"SD card write failed; check space and card, then retry\"}");
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
