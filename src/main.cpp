// All comments in the code must always be in English.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <deque>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static constexpr int SD_SCK = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS = 12;

static constexpr uint16_t UI_BG = 0x0000;
static constexpr uint16_t UI_FG = 0xFFFF;
static constexpr uint16_t UI_DIM = 0x8410;
static constexpr uint16_t UI_HEADER = 0x18C3;
static constexpr uint16_t UI_INPUT = 0x1082;
static constexpr uint16_t UI_ACCENT = 0x07E0;
static constexpr uint16_t UI_WARN = 0xFD20;
static constexpr uint16_t UI_PANE = 0x0841;
static constexpr uint16_t UI_HILITE_BG = 0x2104;

static constexpr int SCREEN_W = 240;
static constexpr int SCREEN_H = 135;
static constexpr int HEADER_H = 14;
static constexpr int INPUT_H = 26;
static constexpr int BODY_Y = HEADER_H + 1;
static constexpr int BODY_H = SCREEN_H - HEADER_H - INPUT_H - 2;
static constexpr int CHAR_W = 6;
static constexpr int CHAR_H = 8;
static constexpr int ROW_H = CHAR_H + 2;
static constexpr int NICK_PANE_W = 76;
static constexpr int TIMESTAMP_W_CHARS = 6;
static constexpr int CONFIG_BUTTON_PIN = 0;
static constexpr uint32_t CONFIG_BUTTON_SHORT_DEBOUNCE_MS = 180;
static constexpr uint32_t CONFIG_BUTTON_LONG_PRESS_MS = 700;
static constexpr uint32_t TITLE_SCREEN_MS = 1800;

static constexpr size_t MAX_TAB_LINES = 350;
static constexpr size_t MAX_TABS = 24;
static constexpr size_t MAX_USERS_PER_TAB = 256;
static constexpr size_t MAX_CHANNEL_LIST_ENTRIES = 320;
static constexpr size_t MAX_INPUT_CHARS = 700;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr uint32_t PING_INTERVAL_MS = 60000;
static constexpr uint32_t PONG_TIMEOUT_MS = 25000;
static constexpr uint32_t UI_REFRESH_MS = 50;
static constexpr uint32_t STATE_SAVE_DEBOUNCE_MS = 1200;
static constexpr uint32_t TEXT_SCROLL_STEP_MS = 220;
static constexpr uint32_t BATTERY_POLL_MS = 5000;
static constexpr uint32_t BNC_CHANNEL_ATTACH_IDLE_MS = 5000;
static constexpr size_t IRC_RX_BYTE_BUDGET_PER_LOOP = 4096;
static constexpr size_t IRC_RX_LINE_BUDGET_PER_LOOP = 24;
static constexpr uint32_t KEYBOARD_ACTION_DEBOUNCE_MS = 120;
static constexpr size_t MAX_PENDING_SD_LOG_LINES = 192;
static constexpr size_t SD_LOG_FLUSH_MAX_LINES = 16;
static constexpr uint32_t SD_LOG_FLUSH_TIME_BUDGET_US = 2500;
static constexpr size_t MAX_IRC_COMMAND_QUEUE_ITEMS = 96;
static constexpr size_t MAX_UI_EVENT_QUEUE_ITEMS = 192;
static constexpr size_t MAX_UI_RAW_BATCH_LINES = 24;
static constexpr size_t UI_EVENT_PROCESS_BUDGET_PER_LOOP = 32;
static constexpr int IRC_TASK_STACK_SIZE = 12288;
static constexpr UBaseType_t IRC_TASK_PRIORITY = 1;
static constexpr BaseType_t IRC_TASK_CORE = 0;

static constexpr const char* CONFIG_PATH = "/irc/config.txt";
static constexpr const char* STATE_PATH = "/irc/state.txt";
static constexpr const char* METRICS_LOG_PATH = "/serialLog.txt";
static constexpr const char* DEFAULT_WIFI_SSID = "YOUR_WIFI";
static constexpr const char* APP_NAME = "Cardputer IRC";
static constexpr const char* APP_VERSION = "0.4";
static constexpr uint16_t DEFAULT_SCREEN_TIMEOUT_SEC = 10;
static constexpr uint8_t DEFAULT_SCREEN_BRIGHTNESS = 10;

extern const uint8_t title_boot_jpg_start[] asm("_binary_media_title_boot_jpg_start");
extern const uint8_t title_boot_jpg_end[] asm("_binary_media_title_boot_jpg_end");

enum class ProxyType {
  None,
  Socks5,
  HttpConnect
};

enum class TabType {
  Status,
  Channel,
  Query
};

enum class ColorMode {
  Full,
  Safe,
  Mono
};

enum class TextOverflowMode {
  Marquee,
  Wrap
};

enum class BouncerMode {
  Generic,
  Soju
};

enum ConfigFieldId {
  CFG_WIFI_SSID = 0,
  CFG_WIFI_PASS,
  CFG_IRC_SERVER,
  CFG_IRC_HOST,
  CFG_IRC_PORT,
  CFG_IRC_TLS,
  CFG_TLS_INSECURE,
  CFG_IRC_PASS,
  CFG_IRC_NICK,
  CFG_IRC_USER,
  CFG_IRC_REALNAME,
  CFG_AUTOJOIN,
  CFG_PROXY_TYPE,
  CFG_PROXY_HOST,
  CFG_PROXY_PORT,
  CFG_PROXY_USER,
  CFG_PROXY_PASS,
  CFG_BNC_ENABLED,
  CFG_BNC_MODE,
  CFG_BNC_USER,
  CFG_BNC_NETWORK,
  CFG_BNC_CLIENT,
  CFG_SOJU_BIND_NETID,
  CFG_BNC_PASS,
  CFG_SASL_ENABLED,
  CFG_SASL_USER,
  CFG_SASL_PASS,
  CFG_NICK_PANE,
  CFG_COLOR_MODE,
  CFG_PERSIST_TABS,
  CFG_SHOW_CONTROL_GLYPHS,
  CFG_TEXT_OVERFLOW,
  CFG_SERIAL_LOG,
  CFG_CHANNEL_LOG,
  CFG_SCREEN_TIMEOUT,
  CFG_SCREEN_BRIGHTNESS,
  CFG_LOG_ROOT,
  CFG_SAVE_AND_RECONNECT,
  CFG_EXIT_DISCARD,
  CFG_COUNT
};

struct IrcServerPreset {
  const char* id;
  const char* label;
  const char* host;
  uint16_t port;
  bool useTLS;
};

static const IrcServerPreset IRC_SERVER_PRESETS[] = {
  {"libera", "Libera.Chat", "irc.libera.chat", 6697, true},
  {"oftc", "OFTC", "irc.oftc.net", 6697, true},
  {"efnet", "EFnet", "irc.efnet.org", 6697, true},
  {"ircnet", "IRCnet", "irc.ircnet.com", 6667, false},
  {"dalnet", "DALnet", "irc.dal.net", 6667, false},
  {"undernet", "Undernet", "irc.undernet.org", 6667, false},
  {"quakenet", "QuakeNet", "irc.quakenet.org", 6667, false},
  {"custom", "Custom", "", 0, false},
};

static constexpr size_t IRC_SERVER_PRESET_COUNT = sizeof(IRC_SERVER_PRESETS) / sizeof(IRC_SERVER_PRESETS[0]);

struct Config {
  String wifiSSID;
  String wifiPass;

  String serverPreset = "libera";
  String endpointHost = "irc.libera.chat";
  uint16_t endpointPort = 6697;
  bool useTLS = true;
  bool tlsInsecure = true;

  String serverPass;
  String nick = "CardADV";
  String username = "cardputer";
  String realname = "Cardputer IRC";
  std::vector<String> autoJoin;

  ProxyType proxyType = ProxyType::None;
  String proxyHost;
  uint16_t proxyPort = 0;
  String proxyUser;
  String proxyPass;

  bool bncEnabled = false;
  BouncerMode bncMode = BouncerMode::Generic;
  String bncUser;
  String bncNetwork;
  String bncClient;
  String sojuBindNetId;
  String bncPass;

  bool saslEnabled = false;
  String saslUser;
  String saslPass;
  String saslMechanism = "PLAIN";

  bool nickPaneEnabled = true;
  uint32_t reconnectInitialMs = 3000;
  uint32_t reconnectMaxMs = 60000;

  ColorMode colorMode = ColorMode::Full;
  bool showControlGlyphs = true;
  bool persistTabs = true;
  TextOverflowMode textOverflowMode = TextOverflowMode::Marquee;
  bool serialLogEnabled = true;
  bool channelLogEnabled = false;
  uint16_t screenTimeoutSec = DEFAULT_SCREEN_TIMEOUT_SEC;
  uint8_t screenBrightness = DEFAULT_SCREEN_BRIGHTNESS;

  String logRoot = "/IRC";
};

struct TagEntry {
  String key;
  String value;
};

struct IrcMessage {
  String raw;
  String prefix;
  String command;
  std::vector<String> params;
  std::vector<TagEntry> tags;
};

struct ChatLine {
  String stampShort;
  String stampLog;
  String raw;
  String plain;
  bool highlight = false;
  bool own = false;
  bool notice = false;
};

struct ChannelListEntry {
  String name;
  uint16_t users = 0;
  String topic;
};

struct UserEntry {
  String nick;
  char prefix = 0;
};

struct SojuNetwork {
  String netId;
  String name;
  String state;
  String host;
  String port;
  String tls;
  String nickname;
  String username;
  String realname;
  String pass;
  String error;
};

struct ChannelListMetric {
  bool active = false;
  uint32_t startedAtMs = 0;
  String requestLine;
  size_t entries = 0;
  bool truncated = false;
};

struct ChannelJoinMetric {
  bool active = false;
  uint32_t startedAtMs = 0;
  size_t requestedCount = 0;
  size_t joinedCount = 0;
  std::vector<String> pendingChannels;
};

struct PingMetric {
  bool active = false;
  uint32_t startedAtMs = 0;
  String token;
};

struct BncChannelAttachMetric {
  bool active = false;
  uint32_t startedAtMs = 0;
  uint32_t lastEventAtMs = 0;
  size_t expectedCount = 0;
  String source;
  std::vector<String> channels;
};

struct PendingSdLogLine {
  String path;
  String line;
};

struct LogPathCacheEntry {
  String tabFolder;
  String path;
};

enum class IrcCommandType {
  ApplyConfig,
  SyncKnownChannels,
  SendRaw,
  JoinBatch,
  RequestChannelList,
  Reconnect,
  Quit,
  SojuAction
};

struct IrcCommand {
  IrcCommandType type = IrcCommandType::SendRaw;
  Config config;
  std::vector<String> channels;
  String line;
  String reason;
  String source;
  bool reconnect = false;
};

enum class UiEventType {
  StatusLine,
  MetricsLine,
  NetState,
  SelfNickChanged,
  SessionReset,
  RawLines
};

struct UiEvent {
  UiEventType type = UiEventType::StatusLine;
  String text;
  String text2;
  bool b1 = false;
  bool b2 = false;
  bool b3 = false;
  std::vector<String> lines;
};

struct Tab {
  String name;
  TabType type = TabType::Status;
  std::vector<ChatLine> lines;
  std::vector<UserEntry> users;
  String topic;
  bool unread = false;
  bool mention = false;
  bool usersDirty = false;
  bool namesBatchActive = false;
  int scroll = 0;
};

struct TextStyle {
  uint16_t fg = UI_FG;
  uint16_t bg = UI_BG;
  bool bold = false;
  bool underline = false;
  bool reverse = false;
};

class SimpleTransport {
 public:
  bool connect(const Config& cfg, String& error) {
    close();
    _cfg = cfg;

    String host = cfg.endpointHost;
    uint16_t port = cfg.endpointPort;

    if (cfg.proxyType == ProxyType::None) {
      if (cfg.useTLS) {
        if (cfg.tlsInsecure) {
          _secureClient.setInsecure();
        }
        if (!_secureClient.connect(host.c_str(), port)) {
          error = "TLS connect failed";
          return false;
        }
        _stream = &_secureClient;
      } else {
        if (!_client.connect(host.c_str(), port)) {
          error = "TCP connect failed";
          return false;
        }
        _stream = &_client;
      }
      _connected = true;
      return true;
    }

    if (cfg.useTLS) {
      error = "TLS over proxy is not enabled in this build";
      return false;
    }

    if (!_client.connect(cfg.proxyHost.c_str(), cfg.proxyPort)) {
      error = "Proxy connect failed";
      return false;
    }
    _stream = &_client;

    if (cfg.proxyType == ProxyType::Socks5) {
      if (!performSocks5(host, port, error)) {
        close();
        return false;
      }
    } else if (cfg.proxyType == ProxyType::HttpConnect) {
      if (!performHttpConnect(host, port, error)) {
        close();
        return false;
      }
    }

    _connected = true;
    return true;
  }

  void close() {
    _connected = false;
    if (_client.connected()) _client.stop();
    if (_secureClient.connected()) _secureClient.stop();
    _stream = nullptr;
  }

  bool connected() const {
    return _connected && _stream && _stream->connected();
  }

  int available() {
    return _stream ? _stream->available() : 0;
  }

  int read() {
    return _stream ? _stream->read() : -1;
  }

  size_t write(const String& s) {
    return _stream ? _stream->print(s) : 0;
  }

 private:
  WiFiClient _client;
  WiFiClientSecure _secureClient;
  Client* _stream = nullptr;
  Config _cfg;
  bool _connected = false;

  static String base64Encode(const String& in) {
    return base64EncodeBytes(reinterpret_cast<const uint8_t*>(in.c_str()), in.length());
  }

  static String base64EncodeBytes(const uint8_t* bytes, size_t len) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    int val = 0;
    int valb = -6;
    for (size_t i = 0; i < len; ++i) {
      val = (val << 8) + bytes[i];
      valb += 8;
      while (valb >= 0) {
        out += table[(val >> valb) & 0x3F];
        valb -= 6;
      }
    }
    if (valb > -6) out += table[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.length() % 4) out += '=';
    return out;
  }

  static bool readLineFromClient(WiFiClient& client, String& out, uint32_t timeoutMs) {
    out = "";
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
      while (client.available()) {
        char c = static_cast<char>(client.read());
        if (c == '\r') continue;
        if (c == '\n') return true;
        out += c;
      }
      delay(2);
    }
    return false;
  }

  bool performSocks5(const String& host, uint16_t port, String& error) {
    uint8_t greeting[4] = {0x05, 0x02, 0x00, 0x02};
    if (_cfg.proxyUser.isEmpty()) {
      greeting[1] = 0x01;
    }
    if (_client.write(greeting, _cfg.proxyUser.isEmpty() ? 3 : 4) == 0) {
      error = "SOCKS5 greeting write failed";
      return false;
    }

    uint8_t methodReply[2] = {0};
    if (_client.readBytes(methodReply, 2) != 2 || methodReply[0] != 0x05) {
      error = "SOCKS5 greeting reply invalid";
      return false;
    }

    if (methodReply[1] == 0xFF) {
      error = "SOCKS5 no auth method accepted";
      return false;
    }

    if (methodReply[1] == 0x02) {
      const size_t ulen = _cfg.proxyUser.length();
      const size_t plen = _cfg.proxyPass.length();
      std::vector<uint8_t> auth;
      auth.reserve(3 + ulen + plen);
      auth.push_back(0x01);
      auth.push_back(static_cast<uint8_t>(ulen));
      for (size_t i = 0; i < ulen; ++i) auth.push_back(static_cast<uint8_t>(_cfg.proxyUser[i]));
      auth.push_back(static_cast<uint8_t>(plen));
      for (size_t i = 0; i < plen; ++i) auth.push_back(static_cast<uint8_t>(_cfg.proxyPass[i]));
      if (_client.write(auth.data(), auth.size()) != auth.size()) {
        error = "SOCKS5 auth write failed";
        return false;
      }
      uint8_t authReply[2] = {0};
      if (_client.readBytes(authReply, 2) != 2 || authReply[1] != 0x00) {
        error = "SOCKS5 auth failed";
        return false;
      }
    }

    std::vector<uint8_t> req;
    req.push_back(0x05);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x03);
    req.push_back(static_cast<uint8_t>(host.length()));
    for (size_t i = 0; i < host.length(); ++i) req.push_back(static_cast<uint8_t>(host[i]));
    req.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
    req.push_back(static_cast<uint8_t>(port & 0xFF));

    if (_client.write(req.data(), req.size()) != req.size()) {
      error = "SOCKS5 connect write failed";
      return false;
    }

    uint8_t hdr[4] = {0};
    if (_client.readBytes(hdr, 4) != 4 || hdr[0] != 0x05) {
      error = "SOCKS5 connect reply invalid";
      return false;
    }
    if (hdr[1] != 0x00) {
      error = "SOCKS5 connect rejected";
      return false;
    }

    int skip = 0;
    if (hdr[3] == 0x01) skip = 4;
    else if (hdr[3] == 0x03) {
      uint8_t len = 0;
      if (_client.readBytes(&len, 1) != 1) {
        error = "SOCKS5 address length read failed";
        return false;
      }
      skip = len;
    } else if (hdr[3] == 0x04) {
      skip = 16;
    }

    for (int i = 0; i < skip + 2; ++i) {
      if (_client.read() < 0) {
        error = "SOCKS5 trailing read failed";
        return false;
      }
    }
    return true;
  }

  bool performHttpConnect(const String& host, uint16_t port, String& error) {
    String req = "CONNECT " + host + ":" + String(port) + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + String(port) + "\r\n";
    if (!_cfg.proxyUser.isEmpty()) {
      String auth = _cfg.proxyUser + ":" + _cfg.proxyPass;
      req += "Proxy-Authorization: Basic " + base64Encode(auth) + "\r\n";
    }
    req += "Proxy-Connection: Keep-Alive\r\n\r\n";

    if (_client.print(req) != req.length()) {
      error = "HTTP CONNECT write failed";
      return false;
    }

    String line;
    if (!readLineFromClient(_client, line, 5000)) {
      error = "HTTP CONNECT no response";
      return false;
    }
    if (line.indexOf("200") < 0) {
      error = "HTTP CONNECT rejected: " + line;
      return false;
    }

    while (readLineFromClient(_client, line, 5000)) {
      if (line.length() == 0) break;
    }
    return true;
  }
};

class IrcClientApp {
 public:
  IrcClientApp() : _frameBuffer(&M5Cardputer.Display) {}

  void begin() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextWrap(false);
    M5Cardputer.Display.fillScreen(UI_BG);
    M5Cardputer.Display.setTextColor(UI_FG, UI_BG);
    pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);
    initFrameBuffer();
    showBootTitle();
    _ircCommandMutex = xSemaphoreCreateMutex();
    _uiEventMutex = xSemaphoreCreateMutex();

    initSD();
    loadConfig();
    _ircCfg = _cfg;
    _selfNick = _cfg.nick;
    _ircSelfNick = _cfg.nick;
    _currentReconnectDelayMs = _cfg.reconnectInitialMs;
    writeMetricsLogLine(formatMetricsLogLine(String(APP_NAME) + " metrics ready: bouncer=" +
      (_cfg.bncEnabled ? String("enabled, type=") + bouncerModeToString(_cfg.bncMode) : String("disabled"))));
    applyConfiguredDisplaySettings();
    refreshBatteryStatus(true);
    _lastUserActivityMs = millis();
    ensureStatusTab();
    loadState();
    logStatus(String(APP_NAME) + " v" + APP_VERSION + " starting...");
    if (wifiNeedsSetup(_cfg)) {
      logStatus("Wi-Fi not configured, opening config");
      openConfigPage(CFG_WIFI_SSID);
    }
    startIrcTask();
    if (!syncKnownChannelsToIrc()) logCommandQueueFailure("known channel sync");
    if (!queueApplyConfig(_cfg, false)) logCommandQueueFailure("initial config");
  }

  void loop() {
    M5Cardputer.update();
    serviceUiEvents();
    serviceButtons();
    if (_configOpen) handleConfigKeyboard();
    else if (_channelListOpen) handleChannelListKeyboard();
    else handleKeyboard();
    serviceStateSave();
    serviceSdLogFlush();
    serviceTextScroll();
    refreshBatteryStatus();
    serviceDisplayTimeout();
    if (millis() - _lastUiRefresh >= UI_REFRESH_MS) {
      draw();
      _lastUiRefresh = millis();
    }
  }

 private:
  Config _cfg;
  Config _ircCfg;
  M5Canvas _frameBuffer;
  bool _useFrameBuffer = false;
  SimpleTransport _transport;
  std::vector<Tab> _tabs;
  int _activeTab = 0;
  String _input;
  String _rxBuffer;
  String _selfNick;
  String _ircSelfNick;
  bool _wifiReady = false;
  bool _uiWifiReady = false;
  String _uiLocalIp;
  bool _sdReady = false;
  bool _dirty = true;
  bool _ircRegistered = false;
  bool _uiTransportConnected = false;
  bool _uiIrcRegistered = false;
  bool _awaitingPong = false;
  String _lastPingToken;
  uint32_t _lastPingMs = 0;
  uint32_t _lastRxMs = 0;
  uint32_t _lastUiRefresh = 0;
  uint32_t _lastTextScrollTick = 0;
  uint32_t _nextWifiAttemptAt = 0;
  uint32_t _nextIrcReconnectAt = 0;
  uint32_t _currentReconnectDelayMs = 3000;
  bool _previousTransportState = false;
  uint32_t _lastBatteryPollMs = 0;
  int32_t _batteryLevel = -1;
  m5::Power_Class::is_charging_t _batteryChargeState = m5::Power_Class::charge_unknown;
  uint32_t _lastUserActivityMs = 0;
  uint32_t _lastEnterKeyMs = 0;
  uint32_t _lastDeleteKeyMs = 0;
  uint32_t _lastTabKeyMs = 0;
  uint32_t _lastActionCharMs = 0;
  char _lastActionChar = 0;
  bool _screenSleeping = false;
  ChannelListMetric _channelListMetric;
  ChannelJoinMetric _channelJoinMetric;
  BncChannelAttachMetric _bncChannelAttachMetric;
  bool _bncChannelAttachArmed = false;
  PingMetric _pingMetric;

  bool _capNegotiationDone = false;
  bool _capRequestSent = false;
  bool _capLsPending = false;
  String _capLsAccum;
  std::vector<String> _serverCaps;
  std::vector<String> _enabledCaps;
  String _sojuBoundNetId;
  bool _sojuBindSent = false;
  bool _sojuListRequested = false;
  String _sojuNetworkBatchId;
  std::vector<SojuNetwork> _sojuNetworks;
  std::vector<SojuNetwork> _sojuBatchNetworks;

  bool _saslInProgress = false;
  bool _saslCompleted = false;
  bool _saslWaitingForChallenge = false;
  std::vector<String> _knownChannels;

  String _desiredActiveTabName;
  bool _stateDirty = false;
  uint32_t _lastStateDirtyMs = 0;
  std::deque<PendingSdLogLine> _pendingSdLogs;
  std::vector<LogPathCacheEntry> _logPathCache;
  String _cachedLogRoot;
  String _cachedLogServerDir;
  String _cachedLogServerKey;
  String _cachedLogDate;

  bool _configOpen = false;
  bool _configEditing = false;
  Config _editCfg;
  String _configEditBuffer;
  int _configSelected = 0;
  int _configScroll = 0;
  bool _configButtonPrev = false;
  uint32_t _lastConfigButtonMs = 0;
  uint32_t _configButtonDownAt = 0;
  bool _configButtonLongHandled = false;
  bool _discardConfigButtonUntilRelease = false;

  bool _channelListOpen = false;
  bool _channelListLoading = false;
  bool _channelListTruncated = false;
  bool _channelListFilterPrompt = false;
  std::vector<ChannelListEntry> _channelList;
  String _channelListFilter;
  String _channelListFilterBuffer;
  int _channelListSelected = 0;
  int _channelListScroll = 0;

  String _chanTypes = "#&";
  String _ircChanTypes = "#&";
  String _prefixSymbols = "~&@%+";
  String _prefixModes = "qaohv";
  TaskHandle_t _ircTaskHandle = nullptr;
  SemaphoreHandle_t _ircCommandMutex = nullptr;
  SemaphoreHandle_t _uiEventMutex = nullptr;
  std::deque<IrcCommand> _ircCommandQueue;
  std::deque<UiEvent> _uiEventQueue;

  static String trimCopy(String s) {
    s.trim();
    return s;
  }

  static String lowerCopy(String s) {
    s.toLowerCase();
    return s;
  }

  static bool equalsIgnoreCase(const String& a, const String& b) {
    return lowerCopy(a) == lowerCopy(b);
  }

  static bool strToBool(const String& s) {
    String v = lowerCopy(trimCopy(s));
    return v == "1" || v == "true" || v == "yes" || v == "on";
  }

  static String normalizeServerPresetId(String s) {
    s = lowerCopy(trimCopy(s));
    if (s == "libera.chat" || s == "libera_chat" || s == "liberachat") return "libera";
    if (s == "oftc.net") return "oftc";
    if (s == "efnet.org") return "efnet";
    if (s == "ircnet.com" || s == "ircnet.net") return "ircnet";
    if (s == "dal.net") return "dalnet";
    if (s == "undernet.org") return "undernet";
    if (s == "quake.net") return "quakenet";
    return s;
  }

  static ProxyType parseProxyType(String s) {
    s = lowerCopy(trimCopy(s));
    if (s == "socks5") return ProxyType::Socks5;
    if (s == "http" || s == "http_connect" || s == "connect") return ProxyType::HttpConnect;
    return ProxyType::None;
  }

  static ColorMode parseColorMode(String s) {
    s = lowerCopy(trimCopy(s));
    if (s == "mono" || s == "off") return ColorMode::Mono;
    if (s == "safe" || s == "filtered") return ColorMode::Safe;
    return ColorMode::Full;
  }

  static TextOverflowMode parseTextOverflowMode(String s) {
    s = lowerCopy(trimCopy(s));
    if (s == "wrap" || s == "line_wrap" || s == "wrapped") return TextOverflowMode::Wrap;
    return TextOverflowMode::Marquee;
  }

  static BouncerMode parseBouncerMode(String s) {
    s = lowerCopy(trimCopy(s));
    if (s == "soju") return BouncerMode::Soju;
    return BouncerMode::Generic;
  }

  static String colorModeToString(ColorMode mode) {
    switch (mode) {
      case ColorMode::Full: return "full";
      case ColorMode::Safe: return "safe";
      case ColorMode::Mono: return "mono";
    }
    return "full";
  }

  static String textOverflowModeToString(TextOverflowMode mode) {
    switch (mode) {
      case TextOverflowMode::Marquee: return "marquee";
      case TextOverflowMode::Wrap: return "wrap";
    }
    return "marquee";
  }

  static String bouncerModeToString(BouncerMode mode) {
    switch (mode) {
      case BouncerMode::Generic: return "generic";
      case BouncerMode::Soju: return "soju";
    }
    return "generic";
  }

  static String proxyTypeToString(ProxyType type) {
    switch (type) {
      case ProxyType::None: return "none";
      case ProxyType::Socks5: return "socks5";
      case ProxyType::HttpConnect: return "http_connect";
    }
    return "none";
  }

  static String boolToOnOff(bool v) {
    return v ? "on" : "off";
  }

  static String formatSeconds(uint32_t elapsedMs) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(elapsedMs) / 1000.0);
    return String(buf);
  }

  static bool lockQueueMutex(SemaphoreHandle_t mutex, TickType_t waitTicks = pdMS_TO_TICKS(5)) {
    return mutex && xSemaphoreTake(mutex, waitTicks) == pdTRUE;
  }

  static void unlockQueueMutex(SemaphoreHandle_t mutex) {
    if (mutex) xSemaphoreGive(mutex);
  }

  static bool isDroppableUiEventType(UiEventType type) {
    return type == UiEventType::StatusLine || type == UiEventType::MetricsLine || type == UiEventType::RawLines;
  }

  static bool isCriticalUiEventType(UiEventType type) {
    return type == UiEventType::NetState || type == UiEventType::SelfNickChanged || type == UiEventType::SessionReset;
  }

  void logCommandQueueFailure(const String& action) {
    logStatus("Internal queue full: " + action + " not sent");
  }

  String formatMetricsLogLine(const String& message) const {
    return "[+" + formatSeconds(millis()) + "s] " + message;
  }

  void writeMetricsLogLine(const String& line) {
    if (!_cfg.serialLogEnabled || !_sdReady) return;
    File f = SD.open(METRICS_LOG_PATH, FILE_APPEND);
    if (!f) f = SD.open(METRICS_LOG_PATH, FILE_WRITE);
    if (!f) return;
    f.println(line);
    f.close();
  }

  bool enqueueIrcCommand(IrcCommand&& cmd) {
    if (!lockQueueMutex(_ircCommandMutex)) return false;
    if (_ircCommandQueue.size() >= MAX_IRC_COMMAND_QUEUE_ITEMS) {
      unlockQueueMutex(_ircCommandMutex);
      return false;
    }
    _ircCommandQueue.push_back(std::move(cmd));
    unlockQueueMutex(_ircCommandMutex);
    return true;
  }

  bool tryPopIrcCommand(IrcCommand& cmd) {
    if (!lockQueueMutex(_ircCommandMutex, 0)) return false;
    if (_ircCommandQueue.empty()) {
      unlockQueueMutex(_ircCommandMutex);
      return false;
    }
    cmd = std::move(_ircCommandQueue.front());
    _ircCommandQueue.pop_front();
    unlockQueueMutex(_ircCommandMutex);
    return true;
  }

  bool enqueueUiEvent(UiEvent&& evt) {
    if (!lockQueueMutex(_uiEventMutex)) return false;

    if (evt.type == UiEventType::NetState && !_uiEventQueue.empty() && _uiEventQueue.back().type == UiEventType::NetState) {
      _uiEventQueue.back() = std::move(evt);
      unlockQueueMutex(_uiEventMutex);
      return true;
    }

    if (evt.type == UiEventType::RawLines &&
        !_uiEventQueue.empty() &&
        _uiEventQueue.back().type == UiEventType::RawLines &&
        _uiEventQueue.back().text == evt.text &&
        !_uiEventQueue.back().lines.empty() &&
        _uiEventQueue.back().lines.size() < MAX_UI_RAW_BATCH_LINES &&
        !evt.lines.empty()) {
      for (const String& line : evt.lines) {
        if (_uiEventQueue.back().lines.size() >= MAX_UI_RAW_BATCH_LINES) break;
        _uiEventQueue.back().lines.push_back(line);
      }
      unlockQueueMutex(_uiEventMutex);
      return true;
    }

    if (_uiEventQueue.size() >= MAX_UI_EVENT_QUEUE_ITEMS && isCriticalUiEventType(evt.type)) {
      for (auto it = _uiEventQueue.begin(); it != _uiEventQueue.end(); ++it) {
        if (isDroppableUiEventType(it->type)) {
          _uiEventQueue.erase(it);
          break;
        }
      }
    }

    if (_uiEventQueue.size() >= MAX_UI_EVENT_QUEUE_ITEMS) {
      unlockQueueMutex(_uiEventMutex);
      return false;
    }
    _uiEventQueue.push_back(std::move(evt));
    unlockQueueMutex(_uiEventMutex);
    return true;
  }

  bool tryPopUiEvent(UiEvent& evt) {
    if (!lockQueueMutex(_uiEventMutex, 0)) return false;
    if (_uiEventQueue.empty()) {
      unlockQueueMutex(_uiEventMutex);
      return false;
    }
    evt = std::move(_uiEventQueue.front());
    _uiEventQueue.pop_front();
    unlockQueueMutex(_uiEventMutex);
    return true;
  }

  void emitStatusEvent(const String& message) {
    UiEvent evt;
    evt.type = UiEventType::StatusLine;
    evt.text = "*** " + message;
    enqueueUiEvent(std::move(evt));
  }

  void emitMetricsEvent(const String& message) {
    if (!_ircCfg.serialLogEnabled) return;
    UiEvent evt;
    evt.type = UiEventType::MetricsLine;
    evt.text = formatMetricsLogLine(message);
    enqueueUiEvent(std::move(evt));
  }

  void emitNetStateEvent() {
    UiEvent evt;
    evt.type = UiEventType::NetState;
    evt.text = _wifiReady ? WiFi.localIP().toString() : "";
    evt.b1 = _wifiReady;
    evt.b2 = _transport.connected();
    evt.b3 = _ircRegistered;
    enqueueUiEvent(std::move(evt));
  }

  void emitSelfNickChanged(const String& nick) {
    UiEvent evt;
    evt.type = UiEventType::SelfNickChanged;
    evt.text = nick;
    enqueueUiEvent(std::move(evt));
  }

  void emitSessionResetEvent() {
    UiEvent evt;
    evt.type = UiEventType::SessionReset;
    enqueueUiEvent(std::move(evt));
    emitNetStateEvent();
  }

  void enqueueUiRawLine(const String& command, const String& raw) {
    UiEvent evt;
    evt.type = UiEventType::RawLines;
    evt.text = command;
    evt.lines.push_back(raw);
    enqueueUiEvent(std::move(evt));
  }

  void metricsLog(const String& message) {
    emitMetricsEvent(message);
  }

  std::vector<String> collectKnownChannelTabs() const {
    std::vector<String> channels;
    for (const Tab& tab : _tabs) {
      if (tab.type != TabType::Channel) continue;
      bool exists = false;
      for (const String& existing : channels) {
        if (equalsIgnoreCase(existing, tab.name)) {
          exists = true;
          break;
        }
      }
      if (!exists) channels.push_back(tab.name);
    }
    return channels;
  }

  bool syncKnownChannelsToIrc() {
    IrcCommand cmd;
    cmd.type = IrcCommandType::SyncKnownChannels;
    cmd.channels = collectKnownChannelTabs();
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueApplyConfig(const Config& cfg, bool reconnect) {
    IrcCommand cmd;
    cmd.type = IrcCommandType::ApplyConfig;
    cmd.config = cfg;
    cmd.reconnect = reconnect;
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueRawCommand(const String& line) {
    if (line.isEmpty()) return false;
    IrcCommand cmd;
    cmd.type = IrcCommandType::SendRaw;
    cmd.line = line;
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueJoinBatch(const std::vector<String>& channels, const String& source) {
    if (channels.empty()) return false;
    IrcCommand cmd;
    cmd.type = IrcCommandType::JoinBatch;
    cmd.channels = channels;
    cmd.source = source;
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueChannelListRequest(const String& filter) {
    IrcCommand cmd;
    cmd.type = IrcCommandType::RequestChannelList;
    cmd.reason = filter;
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueReconnectCommand(const String& reason) {
    IrcCommand cmd;
    cmd.type = IrcCommandType::Reconnect;
    cmd.reason = reason;
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueQuitCommand() {
    IrcCommand cmd;
    cmd.type = IrcCommandType::Quit;
    return enqueueIrcCommand(std::move(cmd));
  }

  bool queueSojuAction(const String& args) {
    IrcCommand cmd;
    cmd.type = IrcCommandType::SojuAction;
    cmd.line = args;
    return enqueueIrcCommand(std::move(cmd));
  }

  void applyUiEvent(const UiEvent& evt) {
    switch (evt.type) {
      case UiEventType::StatusLine:
        appendLine(statusTab(), evt.text);
        break;
      case UiEventType::MetricsLine:
        writeMetricsLogLine(evt.text);
        break;
      case UiEventType::NetState:
        _uiWifiReady = evt.b1;
        _uiTransportConnected = evt.b2;
        _uiIrcRegistered = evt.b3;
        _uiLocalIp = evt.text;
        _dirty = true;
        break;
      case UiEventType::SelfNickChanged:
        _selfNick = evt.text;
        _cfg.nick = evt.text;
        _dirty = true;
        break;
      case UiEventType::SessionReset:
        _uiTransportConnected = false;
        _uiIrcRegistered = false;
        _channelListLoading = false;
        _channelList.clear();
        _channelListSelected = 0;
        _channelListScroll = 0;
        _channelListTruncated = false;
        _channelListOpen = false;
        _channelListFilterPrompt = false;
        _dirty = true;
        break;
      case UiEventType::RawLines:
        for (const String& raw : evt.lines) handleUiRawLine(raw);
        break;
    }
  }

  void serviceUiEvents() {
    UiEvent evt;
    size_t processed = 0;
    while (processed < UI_EVENT_PROCESS_BUDGET_PER_LOOP && tryPopUiEvent(evt)) {
      applyUiEvent(evt);
      ++processed;
    }
  }

  static void ircTaskEntry(void* arg) {
    static_cast<IrcClientApp*>(arg)->ircTaskLoop();
  }

  void startIrcTask() {
    if (_ircTaskHandle) return;
    xTaskCreatePinnedToCore(ircTaskEntry, "ircTask", IRC_TASK_STACK_SIZE, this, IRC_TASK_PRIORITY, &_ircTaskHandle, IRC_TASK_CORE);
  }

  void ircTaskLoop() {
    for (;;) {
      serviceIrcCommands();
      serviceWiFi();
      serviceIRC();
      serviceBncChannelAttachMetric();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  void finishChannelListMetric() {
    if (!_channelListMetric.active) return;
    uint32_t elapsedMs = millis() - _channelListMetric.startedAtMs;
    String message = "Channel list download finished: entries=" + String(_channelListMetric.entries);
    if (_channelListMetric.truncated) message += ", truncated=true";
    message += ", duration=" + formatSeconds(elapsedMs) + " seconds";
    metricsLog(message);
    _channelListMetric = ChannelListMetric();
  }

  void abortChannelListMetric(const String& reason) {
    if (!_channelListMetric.active) return;
    uint32_t elapsedMs = millis() - _channelListMetric.startedAtMs;
    metricsLog("Channel list download aborted: entries=" + String(_channelListMetric.entries) +
      ", duration=" + formatSeconds(elapsedMs) + " seconds, reason=" + reason);
    _channelListMetric = ChannelListMetric();
  }

  void beginChannelListMetric(const String& requestLine) {
    abortChannelListMetric("restarted");
    _channelListMetric.active = true;
    _channelListMetric.startedAtMs = millis();
    _channelListMetric.requestLine = requestLine;
    _channelListMetric.entries = 0;
    _channelListMetric.truncated = false;
    metricsLog("Channel list download started: request=\"" + requestLine + "\"");
  }

  void abortChannelJoinMetric(const String& reason) {
    if (!_channelJoinMetric.active) return;
    uint32_t elapsedMs = millis() - _channelJoinMetric.startedAtMs;
    metricsLog("Channel join batch aborted: joined=" + String(_channelJoinMetric.joinedCount) +
      "/" + String(_channelJoinMetric.requestedCount) +
      ", duration=" + formatSeconds(elapsedMs) + " seconds, reason=" + reason);
    _channelJoinMetric = ChannelJoinMetric();
  }

  void beginChannelJoinMetric(const std::vector<String>& channels, const String& source) {
    std::vector<String> uniqueChannels;
    for (const String& rawChannel : channels) {
      String channel = trimCopy(rawChannel);
      if (channel.isEmpty()) continue;
      bool exists = false;
      for (const String& existing : _channelJoinMetric.pendingChannels) {
        if (equalsIgnoreCase(existing, channel)) {
          exists = true;
          break;
        }
      }
      if (exists) continue;
      for (const String& existing : uniqueChannels) {
        if (equalsIgnoreCase(existing, channel)) {
          exists = true;
          break;
        }
      }
      if (!exists) uniqueChannels.push_back(channel);
    }
    if (uniqueChannels.empty()) return;
    abortBncChannelAttachMetric("explicit join batch started");

    bool wasActive = _channelJoinMetric.active;
    if (!_channelJoinMetric.active) {
      _channelJoinMetric.active = true;
      _channelJoinMetric.startedAtMs = millis();
      _channelJoinMetric.requestedCount = 0;
      _channelJoinMetric.joinedCount = 0;
      _channelJoinMetric.pendingChannels.clear();
    }

    for (const String& channel : uniqueChannels) {
      _channelJoinMetric.pendingChannels.push_back(channel);
      ++_channelJoinMetric.requestedCount;
    }

    metricsLog(String(wasActive ? "Channel join batch extended: requested=" : "Channel join batch started: requested=") +
      String(_channelJoinMetric.requestedCount) +
      ", source=" + source + ", channels=" + joinStrings(uniqueChannels, ","));
  }

  void noteJoinedChannel(const String& channel) {
    if (!_channelJoinMetric.active) return;

    for (size_t i = 0; i < _channelJoinMetric.pendingChannels.size(); ++i) {
      if (!equalsIgnoreCase(_channelJoinMetric.pendingChannels[i], channel)) continue;

      _channelJoinMetric.pendingChannels.erase(_channelJoinMetric.pendingChannels.begin() + i);
      ++_channelJoinMetric.joinedCount;
      metricsLog("Channel join acknowledged: channel=" + channel +
        ", progress=" + String(_channelJoinMetric.joinedCount) + "/" + String(_channelJoinMetric.requestedCount));

      if (_channelJoinMetric.pendingChannels.empty()) {
        uint32_t elapsedMs = millis() - _channelJoinMetric.startedAtMs;
        metricsLog("Channel join batch finished: joined=" + String(_channelJoinMetric.joinedCount) +
          "/" + String(_channelJoinMetric.requestedCount) +
          ", duration=" + formatSeconds(elapsedMs) + " seconds");
        _channelJoinMetric = ChannelJoinMetric();
      }
      return;
    }
  }

  std::vector<String> collectKnownBncChannels() const {
    std::vector<String> channels;
    auto addUnique = [&](const String& rawChannel) {
      String channel = trimCopy(rawChannel);
      if (channel.isEmpty() || !isRuntimeChannelName(channel)) return;
      for (const String& existing : channels) {
        if (equalsIgnoreCase(existing, channel)) return;
      }
      channels.push_back(channel);
    };

    for (const String& channel : _ircCfg.autoJoin) addUnique(channel);
    for (const String& channel : _knownChannels) addUnique(channel);
    return channels;
  }

  void beginBncChannelAttachMetric(const String& source) {
    if (!_ircCfg.bncEnabled) return;
    _bncChannelAttachArmed = true;
    if (_bncChannelAttachMetric.active) return;

    uint32_t now = millis();
    std::vector<String> knownChannels = collectKnownBncChannels();
    _bncChannelAttachMetric.active = true;
    _bncChannelAttachMetric.startedAtMs = now;
    _bncChannelAttachMetric.lastEventAtMs = now;
    _bncChannelAttachMetric.expectedCount = knownChannels.size();
    _bncChannelAttachMetric.source = source;
    _bncChannelAttachMetric.channels.clear();

    String message = "Bouncer channel attach batch started: source=" + source +
      ", expected=" + String(_bncChannelAttachMetric.expectedCount);
    metricsLog(message);
  }

  void abortBncChannelAttachMetric(const String& reason) {
    if (!_bncChannelAttachMetric.active) return;
    uint32_t elapsedMs = millis() - _bncChannelAttachMetric.startedAtMs;
    String message = "Bouncer channel attach batch aborted: observed=" +
      String(_bncChannelAttachMetric.channels.size());
    if (_bncChannelAttachMetric.expectedCount > 0) {
      message += "/" + String(_bncChannelAttachMetric.expectedCount);
    }
    message += ", duration=" + formatSeconds(elapsedMs) + " seconds, reason=" + reason;
    metricsLog(message);
    _bncChannelAttachMetric = BncChannelAttachMetric();
    _bncChannelAttachArmed = false;
  }

  void noteBncObservedChannel(const String& channel, const String& eventType) {
    if (!_ircCfg.bncEnabled || !_ircRegistered || channel.isEmpty() || !isRuntimeChannelName(channel)) return;
    if (_channelJoinMetric.active) return;
    if (!_bncChannelAttachMetric.active && !_bncChannelAttachArmed) return;

    uint32_t now = millis();
    if (!_bncChannelAttachMetric.active) {
      beginBncChannelAttachMetric(String("observed-") + lowerCopy(eventType));
    }
    _bncChannelAttachMetric.lastEventAtMs = now;

    for (const String& existing : _bncChannelAttachMetric.channels) {
      if (equalsIgnoreCase(existing, channel)) return;
    }

    _bncChannelAttachMetric.channels.push_back(channel);
    String message = "Bouncer channel observed: channel=" + channel +
      ", event=" + eventType + ", total=" + String(_bncChannelAttachMetric.channels.size());
    if (_bncChannelAttachMetric.expectedCount > 0) {
      message += "/" + String(_bncChannelAttachMetric.expectedCount);
    }
    metricsLog(message);
  }

  String observedChannelForMessage(const IrcMessage& msg) const {
    if ((msg.command == "PRIVMSG" || msg.command == "NOTICE" || msg.command == "TAGMSG" ||
         msg.command == "MODE" || msg.command == "TOPIC" || msg.command == "JOIN" ||
         msg.command == "PART" || msg.command == "KICK") &&
        !msg.params.empty()) {
      return msg.params[0];
    }
    if ((msg.command == "332" || msg.command == "333" || msg.command == "366") && msg.params.size() > 1) {
      return msg.params[1];
    }
    if (msg.command == "353" && msg.params.size() > 2) {
      return msg.params[2];
    }
    return "";
  }

  void noteBncObservedChannelFromMessage(const IrcMessage& msg) {
    String channel = observedChannelForMessage(msg);
    if (channel.isEmpty()) return;
    noteBncObservedChannel(channel, msg.command);
  }

  void serviceBncChannelAttachMetric() {
    if (!_bncChannelAttachMetric.active) return;

    uint32_t now = millis();
    if (now - _bncChannelAttachMetric.lastEventAtMs < BNC_CHANNEL_ATTACH_IDLE_MS) return;

    uint32_t elapsedMs = now - _bncChannelAttachMetric.startedAtMs;
    String message = "Bouncer channel attach batch finished: observed=" +
      String(_bncChannelAttachMetric.channels.size());
    if (_bncChannelAttachMetric.expectedCount > 0) {
      message += "/" + String(_bncChannelAttachMetric.expectedCount);
    }
    message += ", duration=" + formatSeconds(elapsedMs) + " seconds";
    metricsLog(message);
    _bncChannelAttachMetric = BncChannelAttachMetric();
    _bncChannelAttachArmed = false;
  }

  void abortPingMetric(const String& reason) {
    if (!_pingMetric.active) return;
    uint32_t elapsedMs = millis() - _pingMetric.startedAtMs;
    metricsLog("Server ping aborted: token=" + _pingMetric.token +
      ", duration=" + formatSeconds(elapsedMs) + " seconds, reason=" + reason);
    _pingMetric = PingMetric();
  }

  void beginPingMetric(const String& token) {
    _pingMetric.active = true;
    _pingMetric.startedAtMs = millis();
    _pingMetric.token = token;
    metricsLog("Server ping started: token=" + token + ", payload_bytes=" + String(token.length()));
  }

  void finishPingMetric(const String& token) {
    if (!_pingMetric.active) return;
    uint32_t elapsedMs = millis() - _pingMetric.startedAtMs;
    metricsLog("Server ping finished: token=" + token +
      ", duration=" + formatSeconds(elapsedMs) + " seconds");
    _pingMetric = PingMetric();
  }

  static uint16_t clampScreenTimeoutSeconds(long value) {
    return static_cast<uint16_t>(std::min<long>(3600, std::max<long>(0, value)));
  }

  static uint8_t clampScreenBrightnessLevel(long value) {
    return static_cast<uint8_t>(std::min<long>(10, std::max<long>(0, value)));
  }

  static uint8_t screenBrightnessToPwm(uint8_t level) {
    return static_cast<uint8_t>((static_cast<uint16_t>(level) * 255 + 5) / 10);
  }

  static String ellipsize(String s, int maxChars) {
    if (maxChars <= 0) return "";
    if (static_cast<int>(s.length()) <= maxChars) return s;
    if (maxChars == 1) return s.substring(0, 1);
    return s.substring(0, maxChars - 1) + "~";
  }

  static String maskSecret(const String& s) {
    if (s.isEmpty()) return "";
    String out;
    size_t n = std::min<size_t>(s.length(), 12);
    for (size_t i = 0; i < n; ++i) out += '*';
    if (s.length() > n) out += '+';
    return out;
  }

  void refreshBatteryStatus(bool force = false) {
    uint32_t now = millis();
    if (!force && now - _lastBatteryPollMs < BATTERY_POLL_MS) return;
    _lastBatteryPollMs = now;

    int32_t level = M5Cardputer.Power.getBatteryLevel();
    auto chargeState = M5Cardputer.Power.isCharging();
    if (level != _batteryLevel || chargeState != _batteryChargeState) {
      _batteryLevel = level;
      _batteryChargeState = chargeState;
      _dirty = true;
    }
  }

  void applyConfiguredDisplaySettings() {
    M5Cardputer.Display.setBrightness(screenBrightnessToPwm(_cfg.screenBrightness));
    if (_screenSleeping) {
      M5Cardputer.Display.sleep();
    } else if (_cfg.screenTimeoutSec == 0) {
      M5Cardputer.Display.wakeup();
    }
  }

  void wakeDisplay() {
    if (!_screenSleeping) return;
    _screenSleeping = false;
    M5Cardputer.Display.wakeup();
    _dirty = true;
  }

  void recordUserActivity() {
    _lastUserActivityMs = millis();
    wakeDisplay();
  }

  bool shouldHandleDebouncedKey(bool pressed, uint32_t& lastAtMs, uint32_t debounceMs = KEYBOARD_ACTION_DEBOUNCE_MS) {
    if (!pressed) return false;
    uint32_t now = millis();
    if (now - lastAtMs < debounceMs) return false;
    lastAtMs = now;
    return true;
  }

  bool shouldHandleDebouncedActionChar(char c, uint32_t debounceMs = KEYBOARD_ACTION_DEBOUNCE_MS) {
    uint32_t now = millis();
    if (_lastActionChar == c && now - _lastActionCharMs < debounceMs) return false;
    _lastActionChar = c;
    _lastActionCharMs = now;
    return true;
  }

  void serviceDisplayTimeout() {
    if (_screenSleeping) return;
    if (_cfg.screenTimeoutSec == 0) return;
    if (millis() - _lastUserActivityMs < static_cast<uint32_t>(_cfg.screenTimeoutSec) * 1000UL) return;
    M5Cardputer.Display.sleep();
    _screenSleeping = true;
  }

  static String currentDateStamp() {
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday);
    return String(buf);
  }

  static String currentTimeStampLong() {
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
    return String(buf);
  }

  static String currentTimeStampShort() {
    time_t now = time(nullptr);
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
    return String(buf);
  }

  static String nickFromPrefix(const String& prefix) {
    int bang = prefix.indexOf('!');
    if (bang >= 0) return prefix.substring(0, bang);
    return prefix;
  }

  static String stripIrcFormatting(const String& s) {
    String out;
    for (size_t i = 0; i < s.length(); ++i) {
      char c = s[i];
      if (c == 0x02 || c == 0x03 || c == 0x0F || c == 0x16 || c == 0x1D || c == 0x1F) {
        if (c == 0x03) {
          while (i + 1 < s.length() && isdigit(static_cast<unsigned char>(s[i + 1]))) ++i;
          if (i + 1 < s.length() && s[i + 1] == ',') {
            ++i;
            while (i + 1 < s.length() && isdigit(static_cast<unsigned char>(s[i + 1]))) ++i;
          }
        }
        continue;
      }
      out += c;
    }
    return out;
  }

  String sanitizeForDisplay(const String& s) const {
    String out;
    for (size_t i = 0; i < s.length(); ++i) {
      uint8_t c = static_cast<uint8_t>(s[i]);
      if (c == 0x02 || c == 0x03 || c == 0x0F || c == 0x16 || c == 0x1D || c == 0x1F) {
        out += static_cast<char>(c);
        continue;
      }
      if (c == '\t') {
        out += "    ";
        continue;
      }
      if (c < 32) {
        if (_cfg.showControlGlyphs) {
          out += '^';
          out += static_cast<char>(c + 64);
        }
        continue;
      }
      if (c == 127) {
        if (_cfg.showControlGlyphs) out += "^?";
        continue;
      }
      out += static_cast<char>(c);
    }
    return out;
  }

  static uint16_t ircColorTo565(int idx) {
    switch (idx & 15) {
      case 0: return 0xFFFF;
      case 1: return 0x0000;
      case 2: return 0x0015;
      case 3: return 0x0300;
      case 4: return 0xA800;
      case 5: return 0x780F;
      case 6: return 0xF81F;
      case 7: return 0xFD20;
      case 8: return 0xFFE0;
      case 9: return 0x07E0;
      case 10: return 0x07FF;
      case 11: return 0x041F;
      case 12: return 0x001F;
      case 13: return 0xF81F;
      case 14: return 0x7BEF;
      case 15: return 0xBDF7;
      default: return UI_FG;
    }
  }

  static bool isDigitString(const String& s) {
    if (s.length() == 0) return false;
    for (size_t i = 0; i < s.length(); ++i) {
      if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
  }

  static bool isNickChar(char c) {
    if (isalnum(static_cast<unsigned char>(c))) return true;
    return c == '-' || c == '_' || c == '[' || c == ']' || c == '\\' || c == '`' || c == '^' || c == '{' || c == '}' || c == '|';
  }

  static std::vector<String> splitCsv(String s) {
    std::vector<String> out;
    int start = 0;
    while (start <= static_cast<int>(s.length())) {
      int comma = s.indexOf(',', start);
      String item = comma < 0 ? s.substring(start) : s.substring(start, comma);
      item.trim();
      if (!item.isEmpty()) out.push_back(item);
      if (comma < 0) break;
      start = comma + 1;
    }
    return out;
  }

  static String joinStrings(const std::vector<String>& items, const String& sep) {
    String out;
    for (size_t i = 0; i < items.size(); ++i) {
      if (i) out += sep;
      out += items[i];
    }
    return out;
  }

  static String safeFileName(String s) {
    for (size_t i = 0; i < s.length(); ++i) {
      char c = s[i];
      if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '#')) {
        s.setCharAt(i, '_');
      }
    }
    return s;
  }

  bool isChannelName(const String& s) const {
    return !s.isEmpty() && _chanTypes.indexOf(s[0]) >= 0;
  }

  bool isRuntimeChannelName(const String& s) const {
    return !s.isEmpty() && _ircChanTypes.indexOf(s[0]) >= 0;
  }

  static const IrcServerPreset* findServerPresetById(const String& presetId) {
    String normalized = normalizeServerPresetId(presetId);
    for (size_t i = 0; i < IRC_SERVER_PRESET_COUNT; ++i) {
      if (normalized == IRC_SERVER_PRESETS[i].id) return &IRC_SERVER_PRESETS[i];
    }
    return nullptr;
  }

  static const IrcServerPreset* findServerPresetByEndpoint(const String& host, uint16_t port, bool useTLS) {
    for (size_t i = 0; i + 1 < IRC_SERVER_PRESET_COUNT; ++i) {
      const IrcServerPreset& preset = IRC_SERVER_PRESETS[i];
      if (equalsIgnoreCase(host, preset.host) && port == preset.port && useTLS == preset.useTLS) {
        return &preset;
      }
    }
    return nullptr;
  }

  static String serverPresetLabel(const String& presetId) {
    const IrcServerPreset* preset = findServerPresetById(presetId);
    return preset ? String(preset->label) : String("Custom");
  }

  static size_t serverPresetIndex(const String& presetId) {
    String normalized = normalizeServerPresetId(presetId);
    for (size_t i = 0; i < IRC_SERVER_PRESET_COUNT; ++i) {
      if (normalized == IRC_SERVER_PRESETS[i].id) return i;
    }
    return IRC_SERVER_PRESET_COUNT - 1;
  }

  static void applyServerPreset(Config& cfg, const String& presetId) {
    const IrcServerPreset* preset = findServerPresetById(presetId);
    if (!preset) {
      cfg.serverPreset = "custom";
      return;
    }

    cfg.serverPreset = preset->id;
    if (String(preset->id) == "custom") return;

    cfg.endpointHost = preset->host;
    cfg.endpointPort = preset->port;
    cfg.useTLS = preset->useTLS;
  }

  static void syncServerPresetFromEndpoint(Config& cfg) {
    const IrcServerPreset* preset = findServerPresetByEndpoint(cfg.endpointHost, cfg.endpointPort, cfg.useTLS);
    cfg.serverPreset = preset ? String(preset->id) : String("custom");
  }

  static bool wifiNeedsSetup(const Config& cfg) {
    String ssid = trimCopy(cfg.wifiSSID);
    return ssid.isEmpty() || equalsIgnoreCase(ssid, DEFAULT_WIFI_SSID);
  }

  static String normalizeCapName(const String& token) {
    int eq = token.indexOf('=');
    return eq >= 0 ? token.substring(0, eq) : token;
  }

  void initSD() {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    _sdReady = SD.begin(SD_CS, SPI, 25000000);
  }

  void initFrameBuffer() {
    _useFrameBuffer = false;
    _frameBuffer.deleteSprite();
    _frameBuffer.setColorDepth(16);
    _frameBuffer.setTextSize(1);
    _frameBuffer.setTextWrap(false);
    _frameBuffer.setTextColor(UI_FG, UI_BG);

    _frameBuffer.setPsram(true);
    if (!_frameBuffer.createSprite(SCREEN_W, SCREEN_H)) {
      _frameBuffer.setPsram(false);
      if (!_frameBuffer.createSprite(SCREEN_W, SCREEN_H)) return;
    }

    _useFrameBuffer = true;
  }

  void showBootTitle() {
    auto& gfx = drawTarget();
    size_t titleLen = static_cast<size_t>(title_boot_jpg_end - title_boot_jpg_start);
    gfx.fillScreen(UI_BG);
    if (titleLen > 0) {
      gfx.drawJpg(title_boot_jpg_start, titleLen, 0, 0);
    } else {
      gfx.setTextColor(UI_FG, UI_BG);
      gfx.setCursor(8, 20);
      gfx.println(String(APP_NAME) + " v" + APP_VERSION);
    }
    presentFrame();

    uint32_t start = millis();
    while (millis() - start < TITLE_SCREEN_MS) {
      M5Cardputer.update();
      delay(5);
    }
  }

  lgfx::LovyanGFX& drawTarget() {
    return _useFrameBuffer
      ? static_cast<lgfx::LovyanGFX&>(_frameBuffer)
      : static_cast<lgfx::LovyanGFX&>(M5Cardputer.Display);
  }

  void presentFrame() {
    if (_useFrameBuffer) _frameBuffer.pushSprite(0, 0);
  }

  void serviceTextScroll() {
    if (_screenSleeping || _configOpen || _channelListOpen || _tabs.empty()) return;
    if (useWrappedText()) return;
    if (!activeTabNeedsTextScroll()) return;

    uint32_t tick = millis() / TEXT_SCROLL_STEP_MS;
    if (tick != _lastTextScrollTick) {
      _lastTextScrollTick = tick;
      _dirty = true;
    }
  }

  void getVisibleBodyRange(const Tab& tab, int& start, int& end, int& maxLines) const {
    maxLines = bodyVisibleRows();
    int total = static_cast<int>(tab.lines.size());
    start = std::max(0, total - maxLines - tab.scroll);
    end = std::min(total, start + maxLines);
    if (end - start < maxLines && start > 0) start = std::max(0, end - maxLines);
  }

  bool useWrappedText() const {
    return _cfg.textOverflowMode == TextOverflowMode::Wrap;
  }

  int bodyVisibleRows() const {
    return std::max(1, BODY_H / ROW_H);
  }

  int bodyWidthForTab(const Tab& tab) const {
    bool pane = _cfg.nickPaneEnabled && tab.type == TabType::Channel;
    int paneWidth = pane ? NICK_PANE_W : 0;
    return SCREEN_W - paneWidth - 2;
  }

  int chatTextWidth(const Tab& tab) const {
    return bodyWidthForTab(tab) - (TIMESTAMP_W_CHARS * CHAR_W) - 4;
  }

  int chatTextWidth() const {
    if (_activeTab < 0 || _activeTab >= static_cast<int>(_tabs.size())) {
      return SCREEN_W - (TIMESTAMP_W_CHARS * CHAR_W) - 4;
    }
    return chatTextWidth(_tabs[_activeTab]);
  }

  int measureStyledTextColumns(const String& raw) const {
    int cols = 0;
    for (size_t i = 0; i < raw.length(); ++i) {
      char c = raw[i];
      switch (c) {
        case 0x02:
        case 0x0F:
        case 0x16:
        case 0x1D:
        case 0x1F:
          break;
        case 0x03: {
          int j = static_cast<int>(i) + 1;
          int digits = 0;
          while (j < static_cast<int>(raw.length()) && digits < 2 && isdigit(static_cast<unsigned char>(raw[j]))) {
            ++j;
            ++digits;
          }
          if (j < static_cast<int>(raw.length()) && raw[j] == ',') {
            ++j;
            digits = 0;
            while (j < static_cast<int>(raw.length()) && digits < 2 && isdigit(static_cast<unsigned char>(raw[j]))) {
              ++j;
              ++digits;
            }
          }
          i = j - 1;
          break;
        }
        default:
          ++cols;
          break;
      }
    }
    return cols;
  }

  int wrappedTextRows(const String& raw, int maxWidth) const {
    int charsPerRow = std::max(1, maxWidth / CHAR_W);
    int row = 0;
    int col = 0;
    bool sawPrintable = false;

    auto advanceRow = [&]() {
      ++row;
      col = 0;
    };

    for (size_t i = 0; i < raw.length(); ++i) {
      char c = raw[i];
      switch (c) {
        case 0x02:
        case 0x0F:
        case 0x16:
        case 0x1D:
        case 0x1F:
          break;
        case 0x03: {
          int j = static_cast<int>(i) + 1;
          int fgDigits = 0;
          while (j < static_cast<int>(raw.length()) && fgDigits < 2 && isdigit(static_cast<unsigned char>(raw[j]))) {
            ++j;
            ++fgDigits;
          }
          if (j < static_cast<int>(raw.length()) && raw[j] == ',') {
            ++j;
            int bgDigits = 0;
            while (j < static_cast<int>(raw.length()) && bgDigits < 2 && isdigit(static_cast<unsigned char>(raw[j]))) {
              ++j;
              ++bgDigits;
            }
          }
          i = j - 1;
          break;
        }
        case '\n':
          advanceRow();
          sawPrintable = true;
          break;
        default:
          sawPrintable = true;
          if (col >= charsPerRow) advanceRow();
          ++col;
          if (col >= charsPerRow) advanceRow();
          break;
      }
    }

    if (!sawPrintable) return 1;
    return std::max(1, row + (col > 0 ? 1 : 0));
  }

  int wrappedRowsForLine(const ChatLine& line, int bodyWidth) const {
    int textW = bodyWidth - (TIMESTAMP_W_CHARS * CHAR_W) - 4;
    if (textW <= 0) return 1;
    return wrappedTextRows(line.raw, textW);
  }

  int totalWrappedRows(const Tab& tab, int bodyWidth) const {
    int total = 0;
    for (const ChatLine& line : tab.lines) total += wrappedRowsForLine(line, bodyWidth);
    return std::max(0, total);
  }

  int maxTabScroll(const Tab& tab) const {
    if (useWrappedText()) {
      return std::max(0, totalWrappedRows(tab, bodyWidthForTab(tab)) - bodyVisibleRows());
    }
    return std::max(0, static_cast<int>(tab.lines.size()) - bodyVisibleRows());
  }

  void clampTabScroll(Tab& tab) const {
    int maxScroll = maxTabScroll(tab);
    if (tab.scroll < 0) tab.scroll = 0;
    if (tab.scroll > maxScroll) tab.scroll = maxScroll;
  }

  int scrollUnitsForLine(const Tab& tab, const ChatLine& line) const {
    return useWrappedText() ? wrappedRowsForLine(line, bodyWidthForTab(tab)) : 1;
  }

  void resetAllTabScrolls() {
    for (Tab& tab : _tabs) tab.scroll = 0;
  }

  int currentTextScrollOffsetCols(const ChatLine& line, int textWidth) const {
    int visibleCols = std::max(1, textWidth / CHAR_W);
    int totalCols = measureStyledTextColumns(line.raw);
    if (totalCols <= visibleCols) return 0;

    int overflow = totalCols - visibleCols;
    int cycle = overflow * 2;
    if (cycle <= 0) return 0;

    int phase = static_cast<int>((millis() / TEXT_SCROLL_STEP_MS) % cycle);
    return phase <= overflow ? phase : (cycle - phase);
  }

  bool activeTabNeedsTextScroll() const {
    if (_activeTab < 0 || _activeTab >= static_cast<int>(_tabs.size())) return false;
    if (useWrappedText()) return false;

    const Tab& tab = _tabs[_activeTab];
    int start = 0;
    int end = 0;
    int maxLines = 0;
    getVisibleBodyRange(tab, start, end, maxLines);

    int visibleCols = std::max(1, chatTextWidth(tab) / CHAR_W);
    for (int i = start; i < end; ++i) {
      if (measureStyledTextColumns(tab.lines[i].raw) > visibleCols) return true;
    }
    return false;
  }

  void ensureDirRecursive(const String& path) {
    if (!_sdReady || path.isEmpty()) return;
    String current;
    for (size_t i = 0; i < path.length(); ++i) {
      current += path[i];
      if (path[i] == '/' && current.length() > 1 && !SD.exists(current)) {
        SD.mkdir(current);
      }
    }
    if (!SD.exists(path)) SD.mkdir(path);
  }

  void refreshLogPathCache() {
    if (!_sdReady) return;

    String root = _cfg.logRoot.isEmpty() ? "/IRC" : _cfg.logRoot;
    String serverKey = safeFileName(_cfg.endpointHost.isEmpty() ? "unknown_server" : _cfg.endpointHost);
    String date = currentDateStamp();
    if (root == _cachedLogRoot && serverKey == _cachedLogServerKey && date == _cachedLogDate) return;

    _cachedLogRoot = root;
    _cachedLogServerKey = serverKey;
    _cachedLogDate = date;
    _cachedLogServerDir = root + "/" + serverKey;
    _logPathCache.clear();
    ensureDirRecursive(root);
    ensureDirRecursive(_cachedLogServerDir);
  }

  String resolveLogPath(const String& tabName) {
    if (!_sdReady) return "";
    refreshLogPathCache();

    String tabFolder = logTabFolderName(tabName);
    for (const LogPathCacheEntry& entry : _logPathCache) {
      if (entry.tabFolder == tabFolder) return entry.path;
    }

    String dir = _cachedLogServerDir + "/" + tabFolder;
    ensureDirRecursive(dir);

    LogPathCacheEntry entry;
    entry.tabFolder = tabFolder;
    entry.path = dir + "/" + _cachedLogDate + ".log";
    _logPathCache.push_back(entry);
    return entry.path;
  }

  void serviceSdLogFlush() {
    if (!_sdReady) return;
    if (!_cfg.channelLogEnabled) {
      _pendingSdLogs.clear();
      return;
    }
    if (_pendingSdLogs.empty()) return;

    uint32_t startedAtUs = micros();
    size_t flushedLines = 0;
    while (!_pendingSdLogs.empty() && flushedLines < SD_LOG_FLUSH_MAX_LINES) {
      if (static_cast<uint32_t>(micros() - startedAtUs) >= SD_LOG_FLUSH_TIME_BUDGET_US) break;

      const String path = _pendingSdLogs.front().path;
      File f = SD.open(path, FILE_APPEND);
      if (!f) f = SD.open(path, FILE_WRITE);
      if (!f) {
        _pendingSdLogs.pop_front();
        continue;
      }

      while (!_pendingSdLogs.empty() &&
             _pendingSdLogs.front().path == path &&
             flushedLines < SD_LOG_FLUSH_MAX_LINES) {
        f.println(_pendingSdLogs.front().line);
        _pendingSdLogs.pop_front();
        ++flushedLines;
        if (static_cast<uint32_t>(micros() - startedAtUs) >= SD_LOG_FLUSH_TIME_BUDGET_US) break;
      }
      f.close();
    }
  }


  void serviceButtons() {
    bool pressed = digitalRead(CONFIG_BUTTON_PIN) == LOW;
    uint32_t now = millis();

    if (_discardConfigButtonUntilRelease) {
      if (!pressed) _discardConfigButtonUntilRelease = false;
      _configButtonPrev = pressed;
      return;
    }

    if (pressed && !_configButtonPrev && now - _lastConfigButtonMs > CONFIG_BUTTON_SHORT_DEBOUNCE_MS) {
      if (_screenSleeping) {
        recordUserActivity();
        _discardConfigButtonUntilRelease = true;
        _configButtonPrev = pressed;
        _lastConfigButtonMs = now;
        return;
      }
      recordUserActivity();
      _configButtonDownAt = now;
      _configButtonLongHandled = false;
    }

    if (pressed && !_configButtonLongHandled && now - _configButtonDownAt >= CONFIG_BUTTON_LONG_PRESS_MS) {
      _configButtonLongHandled = true;
      if (_channelListOpen) closeChannelListPage();
      if (_configOpen) closeConfigPage();
      else openConfigPage();
      _lastConfigButtonMs = now;
    }

    if (!pressed && _configButtonPrev) {
      recordUserActivity();
      if (!_configButtonLongHandled && now - _lastConfigButtonMs > CONFIG_BUTTON_SHORT_DEBOUNCE_MS) {
        if (!_configOpen && !_channelListOpen) {
          _cfg.nickPaneEnabled = !_cfg.nickPaneEnabled;
          appendLine(statusTab(), String("*** Nick pane ") + (_cfg.nickPaneEnabled ? "on" : "off"));
          markStateDirty();
        }
        _lastConfigButtonMs = now;
      }
      _configButtonLongHandled = false;
    }

    _configButtonPrev = pressed;
  }

  void openConfigPage(int initialSelection = 0) {
    _editCfg = _cfg;
    _configOpen = true;
    _configEditing = false;
    _configEditBuffer = "";
    _configSelected = std::max(0, std::min(initialSelection, CFG_COUNT - 1));
    _configScroll = 0;
    _dirty = true;
  }

  void closeConfigPage() {
    _configOpen = false;
    _configEditing = false;
    _configEditBuffer = "";
    _dirty = true;
  }

  void resetChannelListState() {
    _channelListOpen = false;
    _channelListLoading = false;
    _channelListTruncated = false;
    _channelListFilterPrompt = false;
    _channelList.clear();
    _channelListFilter = "";
    _channelListFilterBuffer = "";
    _channelListSelected = 0;
    _channelListScroll = 0;
    _dirty = true;
  }

  void openChannelListPage(bool refresh = true) {
    if (!_uiTransportConnected || !_uiIrcRegistered) {
      logStatus("Channel list requires an IRC connection");
      return;
    }
    _channelListOpen = true;
    _channelListFilterPrompt = true;
    _channelListFilter = "";
    _channelListFilterBuffer = "";
    _channelListSelected = 0;
    _channelListScroll = 0;
    _dirty = true;
  }

  void startChannelListSearch(const String& filter, bool refresh = true) {
    if (!_uiTransportConnected || !_uiIrcRegistered) {
      logStatus("Channel list requires an IRC connection");
      return;
    }
    _channelListOpen = true;
    _channelListFilterPrompt = false;
    _channelListFilter = trimCopy(filter);
    _channelListFilterBuffer = _channelListFilter;
    _channelListSelected = 0;
    _channelListScroll = 0;
    _dirty = true;
    if (refresh || _channelList.empty()) requestChannelList();
  }

  bool channelListMatchesFilter(const ChannelListEntry& entry) const {
    if (_channelListFilter.isEmpty()) return true;
    return lowerCopy(entry.name).indexOf(lowerCopy(_channelListFilter)) >= 0;
  }

  std::vector<int> filteredChannelListIndices() const {
    std::vector<int> indices;
    indices.reserve(_channelList.size());
    for (size_t i = 0; i < _channelList.size(); ++i) {
      if (channelListMatchesFilter(_channelList[i])) indices.push_back(static_cast<int>(i));
    }
    return indices;
  }

  void applyChannelListFilterAndRequest(bool refresh = true) {
    _channelListFilter = trimCopy(_channelListFilterBuffer);
    _channelListSelected = 0;
    _channelListScroll = 0;
    _channelListFilterPrompt = false;
    if (refresh || _channelList.empty()) requestChannelList();
    else _dirty = true;
  }

  void closeChannelListPage() {
    _channelListOpen = false;
    _channelListFilterPrompt = false;
    _dirty = true;
  }

  void requestChannelList() {
    if (!_uiTransportConnected || !_uiIrcRegistered) return;
    _channelListLoading = true;
    _channelListTruncated = false;
    _channelList.clear();
    _channelListSelected = 0;
    _channelListScroll = 0;
    if (!queueChannelListRequest(_channelListFilter)) {
      _channelListLoading = false;
      logCommandQueueFailure("channel list request");
    }
    _dirty = true;
  }

  void moveChannelListSelection(int delta) {
    std::vector<int> visible = filteredChannelListIndices();
    if (visible.empty()) return;
    _channelListSelected += delta;
    if (_channelListSelected < 0) _channelListSelected = static_cast<int>(visible.size()) - 1;
    if (_channelListSelected >= static_cast<int>(visible.size())) _channelListSelected = 0;

    int visibleRows = bodyVisibleRows();
    if (_channelListSelected < _channelListScroll) _channelListScroll = _channelListSelected;
    if (_channelListSelected >= _channelListScroll + visibleRows) {
      _channelListScroll = _channelListSelected - visibleRows + 1;
    }
    if (_channelListScroll < 0) _channelListScroll = 0;
    _dirty = true;
  }

  void addChannelListEntry(const String& name, uint16_t users, const String& topic) {
    if (name.isEmpty()) return;
    for (ChannelListEntry& entry : _channelList) {
      if (equalsIgnoreCase(entry.name, name)) {
        entry.users = users;
        entry.topic = ellipsize(topic, 80);
        if (_channelListOpen) _dirty = true;
        return;
      }
    }
    if (_channelList.size() >= MAX_CHANNEL_LIST_ENTRIES) {
      _channelListTruncated = true;
      return;
    }
    ChannelListEntry entry;
    entry.name = name;
    entry.users = users;
    entry.topic = ellipsize(topic, 80);
    _channelList.push_back(entry);
    if (_channelListOpen) _dirty = true;
  }

  void finalizeChannelList() {
    _channelListLoading = false;
    std::sort(_channelList.begin(), _channelList.end(), [&](const ChannelListEntry& a, const ChannelListEntry& b) {
      if (a.users != b.users) return a.users > b.users;
      return lowerCopy(a.name) < lowerCopy(b.name);
    });
    int filteredCount = static_cast<int>(filteredChannelListIndices().size());
    if (_channelListSelected >= filteredCount) _channelListSelected = std::max(0, filteredCount - 1);
    String msg = "Channel list ready: " + String(_channelList.size()) + " entries";
    if (!_channelListFilter.isEmpty()) msg += ", filter matches " + String(filteredCount);
    if (_channelListTruncated) msg += " (truncated)";
    logStatus(msg);
    _dirty = true;
  }

  void joinSelectedChannelFromList() {
    std::vector<int> visible = filteredChannelListIndices();
    if (visible.empty()) return;
    if (_channelListSelected < 0 || _channelListSelected >= static_cast<int>(visible.size())) return;
    const ChannelListEntry& entry = _channelList[visible[_channelListSelected]];
    if (entry.name.isEmpty()) return;
    if (!queueJoinBatch({entry.name}, "channel-list")) {
      logCommandQueueFailure("channel join");
      return;
    }
    Tab& tab = getOrCreateTab(entry.name, TabType::Channel);
    _activeTab = static_cast<int>(&tab - &_tabs[0]);
    tab.unread = false;
    tab.mention = false;
    tab.scroll = 0;
    closeChannelListPage();
    markStateDirty();
  }

  bool configFieldIsAction(int idx) const {
    return idx == CFG_SAVE_AND_RECONNECT || idx == CFG_EXIT_DISCARD;
  }

  bool configFieldIsTextEntry(int idx) const {
    switch (idx) {
      case CFG_WIFI_SSID:
      case CFG_WIFI_PASS:
      case CFG_IRC_HOST:
      case CFG_IRC_PORT:
      case CFG_IRC_PASS:
      case CFG_IRC_NICK:
      case CFG_IRC_USER:
      case CFG_IRC_REALNAME:
      case CFG_AUTOJOIN:
      case CFG_PROXY_HOST:
      case CFG_PROXY_PORT:
      case CFG_PROXY_USER:
      case CFG_PROXY_PASS:
      case CFG_BNC_USER:
      case CFG_BNC_NETWORK:
      case CFG_BNC_CLIENT:
      case CFG_SOJU_BIND_NETID:
      case CFG_BNC_PASS:
      case CFG_SASL_USER:
      case CFG_SASL_PASS:
      case CFG_SCREEN_TIMEOUT:
      case CFG_SCREEN_BRIGHTNESS:
      case CFG_LOG_ROOT:
        return true;
      case CFG_IRC_SERVER:
        return false;
      default:
        return false;
    }
  }

  String getConfigFieldLabel(int idx) const {
    switch (idx) {
      case CFG_WIFI_SSID: return "wifi_ssid";
      case CFG_WIFI_PASS: return "wifi_pass";
      case CFG_IRC_SERVER: return "irc_server";
      case CFG_IRC_HOST: return "irc_host";
      case CFG_IRC_PORT: return "irc_port";
      case CFG_IRC_TLS: return "irc_use_tls";
      case CFG_TLS_INSECURE: return "tls_insecure";
      case CFG_IRC_PASS: return "irc_pass";
      case CFG_IRC_NICK: return "irc_nick";
      case CFG_IRC_USER: return "irc_user";
      case CFG_IRC_REALNAME: return "irc_realname";
      case CFG_AUTOJOIN: return "autojoin";
      case CFG_PROXY_TYPE: return "proxy_type";
      case CFG_PROXY_HOST: return "proxy_host";
      case CFG_PROXY_PORT: return "proxy_port";
      case CFG_PROXY_USER: return "proxy_user";
      case CFG_PROXY_PASS: return "proxy_pass";
      case CFG_BNC_ENABLED: return "bnc_enabled";
      case CFG_BNC_MODE: return "bnc_mode";
      case CFG_BNC_USER: return "bnc_user";
      case CFG_BNC_NETWORK: return "bnc_network";
      case CFG_BNC_CLIENT: return "bnc_client";
      case CFG_SOJU_BIND_NETID: return "soju_bind_netid";
      case CFG_BNC_PASS: return "bnc_pass";
      case CFG_SASL_ENABLED: return "sasl_enabled";
      case CFG_SASL_USER: return "sasl_user";
      case CFG_SASL_PASS: return "sasl_pass";
      case CFG_NICK_PANE: return "nick_pane";
      case CFG_COLOR_MODE: return "color_mode";
      case CFG_PERSIST_TABS: return "persist_tabs";
      case CFG_SHOW_CONTROL_GLYPHS: return "ctrl_glyphs";
      case CFG_TEXT_OVERFLOW: return "text_overflow";
      case CFG_SERIAL_LOG: return "metrics_log_enabled";
      case CFG_CHANNEL_LOG: return "channel_log_enabled";
      case CFG_SCREEN_TIMEOUT: return "screen_timeout_sec";
      case CFG_SCREEN_BRIGHTNESS: return "screen_brightness";
      case CFG_LOG_ROOT: return "log_root";
      case CFG_SAVE_AND_RECONNECT: return "Save+Reconnect";
      case CFG_EXIT_DISCARD: return "Exit/Discard";
      default: return "";
    }
  }

  String getConfigFieldValue(int idx, bool masked = true) const {
    switch (idx) {
      case CFG_WIFI_SSID: return _editCfg.wifiSSID;
      case CFG_WIFI_PASS: return masked ? maskSecret(_editCfg.wifiPass) : _editCfg.wifiPass;
      case CFG_IRC_SERVER: return serverPresetLabel(_editCfg.serverPreset);
      case CFG_IRC_HOST: return _editCfg.endpointHost;
      case CFG_IRC_PORT: return String(_editCfg.endpointPort);
      case CFG_IRC_TLS: return boolToOnOff(_editCfg.useTLS);
      case CFG_TLS_INSECURE: return boolToOnOff(_editCfg.tlsInsecure);
      case CFG_IRC_PASS: return masked ? maskSecret(_editCfg.serverPass) : _editCfg.serverPass;
      case CFG_IRC_NICK: return _editCfg.nick;
      case CFG_IRC_USER: return _editCfg.username;
      case CFG_IRC_REALNAME: return _editCfg.realname;
      case CFG_AUTOJOIN: return joinStrings(_editCfg.autoJoin, ",");
      case CFG_PROXY_TYPE: return proxyTypeToString(_editCfg.proxyType);
      case CFG_PROXY_HOST: return _editCfg.proxyHost;
      case CFG_PROXY_PORT: return String(_editCfg.proxyPort);
      case CFG_PROXY_USER: return _editCfg.proxyUser;
      case CFG_PROXY_PASS: return masked ? maskSecret(_editCfg.proxyPass) : _editCfg.proxyPass;
      case CFG_BNC_ENABLED: return boolToOnOff(_editCfg.bncEnabled);
      case CFG_BNC_MODE: return bouncerModeToString(_editCfg.bncMode);
      case CFG_BNC_USER: return _editCfg.bncUser;
      case CFG_BNC_NETWORK: return _editCfg.bncNetwork;
      case CFG_BNC_CLIENT: return _editCfg.bncClient;
      case CFG_SOJU_BIND_NETID: return _editCfg.sojuBindNetId;
      case CFG_BNC_PASS: return masked ? maskSecret(_editCfg.bncPass) : _editCfg.bncPass;
      case CFG_SASL_ENABLED: return boolToOnOff(_editCfg.saslEnabled);
      case CFG_SASL_USER: return _editCfg.saslUser;
      case CFG_SASL_PASS: return masked ? maskSecret(_editCfg.saslPass) : _editCfg.saslPass;
      case CFG_NICK_PANE: return boolToOnOff(_editCfg.nickPaneEnabled);
      case CFG_COLOR_MODE: return colorModeToString(_editCfg.colorMode);
      case CFG_PERSIST_TABS: return boolToOnOff(_editCfg.persistTabs);
      case CFG_SHOW_CONTROL_GLYPHS: return boolToOnOff(_editCfg.showControlGlyphs);
      case CFG_TEXT_OVERFLOW: return textOverflowModeToString(_editCfg.textOverflowMode);
      case CFG_SERIAL_LOG: return boolToOnOff(_editCfg.serialLogEnabled);
      case CFG_CHANNEL_LOG: return boolToOnOff(_editCfg.channelLogEnabled);
      case CFG_SCREEN_TIMEOUT: return String(_editCfg.screenTimeoutSec);
      case CFG_SCREEN_BRIGHTNESS: return String(_editCfg.screenBrightness);
      case CFG_LOG_ROOT: return _editCfg.logRoot;
      case CFG_SAVE_AND_RECONNECT: return "[enter]";
      case CFG_EXIT_DISCARD: return "[enter]";
      default: return "";
    }
  }

  void setConfigFieldValue(int idx, const String& value) {
    switch (idx) {
      case CFG_WIFI_SSID: _editCfg.wifiSSID = value; break;
      case CFG_WIFI_PASS: _editCfg.wifiPass = value; break;
      case CFG_IRC_HOST:
        _editCfg.endpointHost = value;
        syncServerPresetFromEndpoint(_editCfg);
        break;
      case CFG_IRC_PORT:
        _editCfg.endpointPort = static_cast<uint16_t>(std::max<long>(0, value.toInt()));
        syncServerPresetFromEndpoint(_editCfg);
        break;
      case CFG_IRC_PASS: _editCfg.serverPass = value; break;
      case CFG_IRC_NICK: _editCfg.nick = value; break;
      case CFG_IRC_USER: _editCfg.username = value; break;
      case CFG_IRC_REALNAME: _editCfg.realname = value; break;
      case CFG_AUTOJOIN: _editCfg.autoJoin = splitCsv(value); break;
      case CFG_PROXY_HOST: _editCfg.proxyHost = value; break;
      case CFG_PROXY_PORT: _editCfg.proxyPort = static_cast<uint16_t>(std::max<long>(0, value.toInt())); break;
      case CFG_PROXY_USER: _editCfg.proxyUser = value; break;
      case CFG_PROXY_PASS: _editCfg.proxyPass = value; break;
      case CFG_BNC_MODE: _editCfg.bncMode = parseBouncerMode(value); break;
      case CFG_BNC_USER: _editCfg.bncUser = value; break;
      case CFG_BNC_NETWORK: _editCfg.bncNetwork = value; break;
      case CFG_BNC_CLIENT: _editCfg.bncClient = value; break;
      case CFG_SOJU_BIND_NETID: _editCfg.sojuBindNetId = value; break;
      case CFG_BNC_PASS: _editCfg.bncPass = value; break;
      case CFG_SASL_USER: _editCfg.saslUser = value; break;
      case CFG_SASL_PASS: _editCfg.saslPass = value; break;
      case CFG_TEXT_OVERFLOW: _editCfg.textOverflowMode = parseTextOverflowMode(value); break;
      case CFG_CHANNEL_LOG: _editCfg.channelLogEnabled = strToBool(value); break;
      case CFG_SCREEN_TIMEOUT: _editCfg.screenTimeoutSec = clampScreenTimeoutSeconds(value.toInt()); break;
      case CFG_SCREEN_BRIGHTNESS: _editCfg.screenBrightness = clampScreenBrightnessLevel(value.toInt()); break;
      case CFG_LOG_ROOT: _editCfg.logRoot = value; break;
      default: break;
    }
  }

  void moveConfigSelection(int delta) {
    _configSelected += delta;
    if (_configSelected < 0) _configSelected = CFG_COUNT - 1;
    if (_configSelected >= CFG_COUNT) _configSelected = 0;
    int visibleRows = bodyVisibleRows();
    if (_configSelected < _configScroll) _configScroll = _configSelected;
    if (_configSelected >= _configScroll + visibleRows) _configScroll = _configSelected - visibleRows + 1;
    if (_configScroll < 0) _configScroll = 0;
    _dirty = true;
  }

  void saveConfigToSD(const Config& cfg) {
    if (!_sdReady) return;
    ensureDirRecursive("/irc");
    if (SD.exists(CONFIG_PATH)) SD.remove(CONFIG_PATH);
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    if (!f) return;

    f.println("wifi_ssid=" + cfg.wifiSSID);
    f.println("wifi_pass=" + cfg.wifiPass);
    f.println("irc_server_preset=" + cfg.serverPreset);
    f.println("irc_host=" + cfg.endpointHost);
    f.println("irc_port=" + String(cfg.endpointPort));
    f.println("irc_use_tls=" + String(cfg.useTLS ? "true" : "false"));
    f.println("tls_insecure=" + String(cfg.tlsInsecure ? "true" : "false"));
    f.println("irc_nick=" + cfg.nick);
    f.println("irc_user=" + cfg.username);
    f.println("irc_realname=" + cfg.realname);
    f.println("irc_pass=" + cfg.serverPass);
    f.println("autojoin=" + joinStrings(cfg.autoJoin, ","));
    f.println("proxy_type=" + proxyTypeToString(cfg.proxyType));
    f.println("proxy_host=" + cfg.proxyHost);
    f.println("proxy_port=" + String(cfg.proxyPort));
    f.println("proxy_user=" + cfg.proxyUser);
    f.println("proxy_pass=" + cfg.proxyPass);
    f.println("bnc_enabled=" + String(cfg.bncEnabled ? "true" : "false"));
    f.println("bnc_mode=" + bouncerModeToString(cfg.bncMode));
    f.println("bnc_user=" + cfg.bncUser);
    f.println("bnc_network=" + cfg.bncNetwork);
    f.println("bnc_client=" + cfg.bncClient);
    f.println("soju_bind_netid=" + cfg.sojuBindNetId);
    f.println("bnc_pass=" + cfg.bncPass);
    f.println("sasl_enabled=" + String(cfg.saslEnabled ? "true" : "false"));
    f.println("sasl_user=" + cfg.saslUser);
    f.println("sasl_pass=" + cfg.saslPass);
    f.println("sasl_mechanism=" + cfg.saslMechanism);
    f.println("nick_pane_enabled=" + String(cfg.nickPaneEnabled ? "true" : "false"));
    f.println("reconnect_initial_ms=" + String(cfg.reconnectInitialMs));
    f.println("reconnect_max_ms=" + String(cfg.reconnectMaxMs));
    f.println("channel_log_enabled=" + String(cfg.channelLogEnabled ? "true" : "false"));
    f.println("log_root=" + cfg.logRoot);
    f.println("color_mode=" + colorModeToString(cfg.colorMode));
    f.println("show_control_glyphs=" + String(cfg.showControlGlyphs ? "true" : "false"));
    f.println("persist_tabs=" + String(cfg.persistTabs ? "true" : "false"));
    f.println("text_overflow=" + textOverflowModeToString(cfg.textOverflowMode));
    f.println("metrics_log_enabled=" + String(cfg.serialLogEnabled ? "true" : "false"));
    f.println("screen_timeout_sec=" + String(cfg.screenTimeoutSec));
    f.println("screen_brightness=" + String(cfg.screenBrightness));
    f.close();
  }

  void activateConfigField() {
    switch (_configSelected) {
      case CFG_IRC_SERVER: {
        size_t next = (serverPresetIndex(_editCfg.serverPreset) + 1) % IRC_SERVER_PRESET_COUNT;
        applyServerPreset(_editCfg, IRC_SERVER_PRESETS[next].id);
        break;
      }
      case CFG_IRC_TLS:
        _editCfg.useTLS = !_editCfg.useTLS;
        syncServerPresetFromEndpoint(_editCfg);
        break;
      case CFG_TLS_INSECURE:
        _editCfg.tlsInsecure = !_editCfg.tlsInsecure;
        break;
      case CFG_PROXY_TYPE:
        if (_editCfg.proxyType == ProxyType::None) _editCfg.proxyType = ProxyType::Socks5;
        else if (_editCfg.proxyType == ProxyType::Socks5) _editCfg.proxyType = ProxyType::HttpConnect;
        else _editCfg.proxyType = ProxyType::None;
        break;
      case CFG_BNC_ENABLED:
        _editCfg.bncEnabled = !_editCfg.bncEnabled;
        break;
      case CFG_BNC_MODE:
        _editCfg.bncMode = (_editCfg.bncMode == BouncerMode::Generic) ? BouncerMode::Soju : BouncerMode::Generic;
        break;
      case CFG_SASL_ENABLED:
        _editCfg.saslEnabled = !_editCfg.saslEnabled;
        break;
      case CFG_NICK_PANE:
        _editCfg.nickPaneEnabled = !_editCfg.nickPaneEnabled;
        break;
      case CFG_COLOR_MODE:
        if (_editCfg.colorMode == ColorMode::Full) _editCfg.colorMode = ColorMode::Safe;
        else if (_editCfg.colorMode == ColorMode::Safe) _editCfg.colorMode = ColorMode::Mono;
        else _editCfg.colorMode = ColorMode::Full;
        break;
      case CFG_PERSIST_TABS:
        _editCfg.persistTabs = !_editCfg.persistTabs;
        break;
      case CFG_SHOW_CONTROL_GLYPHS:
        _editCfg.showControlGlyphs = !_editCfg.showControlGlyphs;
        break;
      case CFG_TEXT_OVERFLOW:
        _editCfg.textOverflowMode = (_editCfg.textOverflowMode == TextOverflowMode::Marquee)
          ? TextOverflowMode::Wrap
          : TextOverflowMode::Marquee;
        break;
      case CFG_SERIAL_LOG:
        _editCfg.serialLogEnabled = !_editCfg.serialLogEnabled;
        break;
      case CFG_CHANNEL_LOG:
        _editCfg.channelLogEnabled = !_editCfg.channelLogEnabled;
        break;
      case CFG_SAVE_AND_RECONNECT:
        if (_editCfg.reconnectInitialMs == 0) _editCfg.reconnectInitialMs = 3000;
        if (_editCfg.reconnectMaxMs < _editCfg.reconnectInitialMs) _editCfg.reconnectMaxMs = _editCfg.reconnectInitialMs;
        saveConfigToSD(_editCfg);
        if (_cfg.textOverflowMode != _editCfg.textOverflowMode || _cfg.nickPaneEnabled != _editCfg.nickPaneEnabled) {
          resetAllTabScrolls();
        }
        _cfg = _editCfg;
        _selfNick = _cfg.nick;
        applyConfiguredDisplaySettings();
        if (_cfg.screenTimeoutSec == 0) wakeDisplay();
        _lastUserActivityMs = millis();
        _uiWifiReady = false;
        _uiTransportConnected = false;
        _uiIrcRegistered = false;
        markStateDirty();
        if (!syncKnownChannelsToIrc()) logCommandQueueFailure("known channel sync");
        if (!queueApplyConfig(_cfg, true)) logCommandQueueFailure("config apply");
        closeConfigPage();
        logStatus("Config saved to SD");
        return;
      case CFG_EXIT_DISCARD:
        closeConfigPage();
        logStatus("Config page closed without saving");
        return;
      default:
        if (configFieldIsTextEntry(_configSelected)) {
          _configEditing = true;
          _configEditBuffer = getConfigFieldValue(_configSelected, false);
        }
        break;
    }
    _dirty = true;
  }

  void handleConfigKeyboard() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    recordUserActivity();

    auto ks = M5Cardputer.Keyboard.keysState();

    if (_configEditing) {
      for (char c : ks.word) {
        if (c >= 32 && c != '\t' && _configEditBuffer.length() < MAX_INPUT_CHARS) {
          _configEditBuffer += c;
        }
      }
      if (shouldHandleDebouncedKey(ks.del, _lastDeleteKeyMs) && !_configEditBuffer.isEmpty()) {
        _configEditBuffer.remove(_configEditBuffer.length() - 1);
      }
      if (shouldHandleDebouncedKey(ks.enter, _lastEnterKeyMs)) {
        setConfigFieldValue(_configSelected, _configEditBuffer);
        _configEditing = false;
        _configEditBuffer = "";
      }
      if (shouldHandleDebouncedKey(ks.tab, _lastTabKeyMs)) {
        setConfigFieldValue(_configSelected, _configEditBuffer);
        _configEditing = false;
        _configEditBuffer = "";
        moveConfigSelection(1);
      }
      _dirty = true;
      return;
    }

    if (shouldHandleDebouncedKey(ks.tab, _lastTabKeyMs)) moveConfigSelection(1);
    if (shouldHandleDebouncedKey(ks.del, _lastDeleteKeyMs)) moveConfigSelection(-1);
    if (shouldHandleDebouncedKey(ks.enter, _lastEnterKeyMs)) activateConfigField();

    for (char c : ks.word) {
      if ((c == '.' || c == ';' || c == ' ') && !shouldHandleDebouncedActionChar(c)) continue;
      if (c == '.') moveConfigSelection(1);
      else if (c == ';') moveConfigSelection(-1);
      else if (c == ' ') activateConfigField();
    }
  }

  void loadConfig() {
    _cfg = Config();
    _selfNick = _cfg.nick;
    _currentReconnectDelayMs = _cfg.reconnectInitialMs;

    if (!_sdReady || !SD.exists(CONFIG_PATH)) {
      logStatus("No /irc/config.txt found, using defaults");
      return;
    }

    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f) {
      logStatus("Config open failed");
      return;
    }

    bool hasServerPreset = false;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.replace("\r", "");
      line.trim();
      if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      String key = trimCopy(line.substring(0, eq));
      String value = trimCopy(line.substring(eq + 1));
      key.toLowerCase();

      if (key == "wifi_ssid") _cfg.wifiSSID = value;
      else if (key == "wifi_pass") _cfg.wifiPass = value;
      else if (key == "irc_server_preset" || key == "irc_server") {
        _cfg.serverPreset = normalizeServerPresetId(value);
        hasServerPreset = true;
      }
      else if (key == "irc_host" || key == "endpoint_host") _cfg.endpointHost = value;
      else if (key == "irc_port" || key == "endpoint_port") _cfg.endpointPort = static_cast<uint16_t>(value.toInt());
      else if (key == "irc_use_tls") _cfg.useTLS = strToBool(value);
      else if (key == "tls_insecure") _cfg.tlsInsecure = strToBool(value);
      else if (key == "irc_pass" || key == "server_pass") _cfg.serverPass = value;
      else if (key == "irc_nick" || key == "nick") _cfg.nick = value;
      else if (key == "irc_user" || key == "username") _cfg.username = value;
      else if (key == "irc_realname" || key == "realname") _cfg.realname = value;
      else if (key == "autojoin") _cfg.autoJoin = splitCsv(value);
      else if (key == "proxy_type") _cfg.proxyType = parseProxyType(value);
      else if (key == "proxy_host") _cfg.proxyHost = value;
      else if (key == "proxy_port") _cfg.proxyPort = static_cast<uint16_t>(value.toInt());
      else if (key == "proxy_user") _cfg.proxyUser = value;
      else if (key == "proxy_pass") _cfg.proxyPass = value;
      else if (key == "bnc_enabled") _cfg.bncEnabled = strToBool(value);
      else if (key == "bnc_mode") _cfg.bncMode = parseBouncerMode(value);
      else if (key == "bnc_user") _cfg.bncUser = value;
      else if (key == "bnc_network") _cfg.bncNetwork = value;
      else if (key == "bnc_client") _cfg.bncClient = value;
      else if (key == "soju_bind_netid") _cfg.sojuBindNetId = value;
      else if (key == "bnc_pass") _cfg.bncPass = value;
      else if (key == "sasl_enabled") _cfg.saslEnabled = strToBool(value);
      else if (key == "sasl_user") _cfg.saslUser = value;
      else if (key == "sasl_pass") _cfg.saslPass = value;
      else if (key == "sasl_mechanism") _cfg.saslMechanism = value;
      else if (key == "nick_pane_enabled") _cfg.nickPaneEnabled = strToBool(value);
      else if (key == "reconnect_initial_ms") _cfg.reconnectInitialMs = static_cast<uint32_t>(value.toInt());
      else if (key == "reconnect_max_ms") _cfg.reconnectMaxMs = static_cast<uint32_t>(value.toInt());
      else if (key == "channel_log_enabled" || key == "chat_log_enabled") _cfg.channelLogEnabled = strToBool(value);
      else if (key == "log_root") _cfg.logRoot = value;
      else if (key == "color_mode") _cfg.colorMode = parseColorMode(value);
      else if (key == "show_control_glyphs") _cfg.showControlGlyphs = strToBool(value);
      else if (key == "persist_tabs") _cfg.persistTabs = strToBool(value);
      else if (key == "text_overflow" || key == "chat_text_mode") _cfg.textOverflowMode = parseTextOverflowMode(value);
      else if (key == "metrics_log_enabled" || key == "serial_log_enabled" || key == "serial_metrics_enabled") {
        _cfg.serialLogEnabled = strToBool(value);
      }
      else if (key == "screen_timeout_sec") _cfg.screenTimeoutSec = clampScreenTimeoutSeconds(value.toInt());
      else if (key == "screen_brightness") _cfg.screenBrightness = clampScreenBrightnessLevel(value.toInt());
    }

    f.close();
    if (hasServerPreset) {
      applyServerPreset(_cfg, _cfg.serverPreset);
    } else {
      syncServerPresetFromEndpoint(_cfg);
    }
    if (_cfg.reconnectInitialMs == 0) _cfg.reconnectInitialMs = 3000;
    if (_cfg.reconnectMaxMs < _cfg.reconnectInitialMs) _cfg.reconnectMaxMs = _cfg.reconnectInitialMs;
    _selfNick = _cfg.nick;
    _currentReconnectDelayMs = _cfg.reconnectInitialMs;
    logStatus("Config loaded from SD");
  }

  void loadState() {
    if (!_cfg.persistTabs || !_sdReady || !SD.exists(STATE_PATH)) return;

    File f = SD.open(STATE_PATH, FILE_READ);
    if (!f) return;

    ensureStatusTab();
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.replace("\r", "");
      line.trim();
      if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;
      int eq = line.indexOf('=');
      if (eq < 0) continue;
      String key = trimCopy(line.substring(0, eq));
      String value = trimCopy(line.substring(eq + 1));
      key.toLowerCase();

      if (key == "channel") {
        if (!value.isEmpty()) getOrCreateTab(value, TabType::Channel);
      } else if (key == "query") {
        if (!value.isEmpty()) getOrCreateTab(value, TabType::Query);
      } else if (key == "active") {
        _desiredActiveTabName = value;
      } else if (key == "nick_pane_enabled") {
        _cfg.nickPaneEnabled = strToBool(value);
      } else if (key == "color_mode") {
        _cfg.colorMode = parseColorMode(value);
      }
    }
    f.close();

    if (!_desiredActiveTabName.isEmpty()) {
      Tab* t = findTab(_desiredActiveTabName);
      if (t) {
        _activeTab = static_cast<int>(t - &_tabs[0]);
      }
    }
    _dirty = true;
  }

  void markStateDirty() {
    if (!_cfg.persistTabs) return;
    _stateDirty = true;
    _lastStateDirtyMs = millis();
  }

  void serviceStateSave() {
    if (!_stateDirty || !_sdReady || !_cfg.persistTabs) return;
    if (millis() - _lastStateDirtyMs < STATE_SAVE_DEBOUNCE_MS) return;

    ensureDirRecursive("/irc");
    if (SD.exists(STATE_PATH)) SD.remove(STATE_PATH);

    File f = SD.open(STATE_PATH, FILE_WRITE);
    if (!f) return;

    f.println("active=" + _tabs[_activeTab].name);
    f.println("nick_pane_enabled=" + String(_cfg.nickPaneEnabled ? "true" : "false"));
    f.println("color_mode=" + colorModeToString(_cfg.colorMode));
    for (size_t i = 1; i < _tabs.size(); ++i) {
      if (_tabs[i].type == TabType::Channel) f.println("channel=" + _tabs[i].name);
      else if (_tabs[i].type == TabType::Query) f.println("query=" + _tabs[i].name);
    }
    f.close();
    _stateDirty = false;
  }

  void serviceIrcCommands() {
    IrcCommand cmd;
    while (tryPopIrcCommand(cmd)) {
      switch (cmd.type) {
        case IrcCommandType::ApplyConfig:
          _ircCfg = cmd.config;
          _ircSelfNick = _ircCfg.nick;
          _currentReconnectDelayMs = _ircCfg.reconnectInitialMs;
          if (cmd.reconnect) {
            scheduleReconnect("Reconnect after config save");
          } else {
            _nextWifiAttemptAt = 0;
            _nextIrcReconnectAt = 0;
          }
          break;
        case IrcCommandType::SyncKnownChannels:
          _knownChannels = cmd.channels;
          break;
        case IrcCommandType::SendRaw:
          ircSendRaw(cmd.line);
          break;
        case IrcCommandType::JoinBatch: {
          if (cmd.channels.empty()) break;
          beginChannelJoinMetric(cmd.channels, cmd.source.isEmpty() ? "manual" : cmd.source);
          std::vector<String> joined;
          for (const String& channel : cmd.channels) {
            String c = trimCopy(channel);
            if (c.isEmpty()) continue;
            bool exists = false;
            for (const String& known : _knownChannels) {
              if (equalsIgnoreCase(known, c)) {
                exists = true;
                break;
              }
            }
            if (!exists) _knownChannels.push_back(c);
            joined.push_back(c);
          }
          if (!joined.empty()) ircSendRaw("JOIN " + joinStrings(joined, ","));
          break;
        }
        case IrcCommandType::RequestChannelList:
          if (_transport.connected() && _ircRegistered) {
            beginChannelListMetric(cmd.reason.isEmpty() ? "LIST" : "LIST (filter=\"" + cmd.reason + "\")");
            ircSendRaw("LIST");
          } else {
            emitStatusEvent("Channel list requires an IRC connection");
          }
          break;
        case IrcCommandType::Reconnect:
          scheduleReconnect(cmd.reason.isEmpty() ? String("Manual reconnect") : cmd.reason);
          break;
        case IrcCommandType::Quit:
          ircSendRaw("QUIT :Bye from Cardputer");
          _transport.close();
          _ircRegistered = false;
          emitSessionResetEvent();
          break;
        case IrcCommandType::SojuAction:
          handleSojuCommand(cmd.line);
          break;
      }
    }
  }

  void connectWiFi() {
    if (wifiNeedsSetup(_ircCfg)) {
      emitStatusEvent("Wi-Fi SSID missing or placeholder");
      return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(_ircCfg.wifiSSID.c_str(), _ircCfg.wifiPass.c_str());

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    _wifiReady = WiFi.status() == WL_CONNECTED;
    if (_wifiReady) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      emitStatusEvent("Wi-Fi connected: " + WiFi.localIP().toString());
      _nextWifiAttemptAt = 0;
    } else {
      emitStatusEvent("Wi-Fi connect timeout");
    }
    emitNetStateEvent();
  }

  void serviceWiFi() {
    if (wifiNeedsSetup(_ircCfg)) return;
    if (WiFi.status() == WL_CONNECTED) {
      if (!_wifiReady) {
        _wifiReady = true;
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        emitStatusEvent("Wi-Fi connected: " + WiFi.localIP().toString());
        emitNetStateEvent();
      }
      return;
    }

    bool wasReady = _wifiReady;
    _wifiReady = false;
    if (wasReady) emitNetStateEvent();
    if (millis() < _nextWifiAttemptAt) return;

    _nextWifiAttemptAt = millis() + 5000;
    WiFi.disconnect();
    WiFi.begin(_ircCfg.wifiSSID.c_str(), _ircCfg.wifiPass.c_str());
    emitStatusEvent("Wi-Fi reconnect...");
  }

  void scheduleReconnect(const String& reason) {
    if (!reason.isEmpty()) emitStatusEvent(reason);
    abortChannelListMetric(reason.isEmpty() ? String("reconnect") : reason);
    abortChannelJoinMetric(reason.isEmpty() ? String("reconnect") : reason);
    abortBncChannelAttachMetric(reason.isEmpty() ? String("reconnect") : reason);
    abortPingMetric(reason.isEmpty() ? String("reconnect") : reason);
    _transport.close();
    _ircRegistered = false;
    _awaitingPong = false;
    _capNegotiationDone = false;
    _capRequestSent = false;
    _capLsPending = false;
    _capLsAccum = "";
    _serverCaps.clear();
    _enabledCaps.clear();
    _sojuBoundNetId = "";
    _sojuBindSent = false;
    _sojuListRequested = false;
    _sojuNetworkBatchId = "";
    _sojuBatchNetworks.clear();
    _sojuNetworks.clear();
    _saslInProgress = false;
    _saslCompleted = false;
    _saslWaitingForChallenge = false;
    _nextIrcReconnectAt = millis() + _currentReconnectDelayMs;
    _currentReconnectDelayMs = std::min(_currentReconnectDelayMs * 2U, _ircCfg.reconnectMaxMs);
    emitSessionResetEvent();
  }

  void resetReconnectBackoff() {
    _currentReconnectDelayMs = _ircCfg.reconnectInitialMs;
    _nextIrcReconnectAt = 0;
  }

  void serviceIRC() {
    if (!_wifiReady || _ircCfg.endpointHost.isEmpty()) return;

    bool nowConnected = _transport.connected();
    if (_previousTransportState && !nowConnected) {
      scheduleReconnect("IRC disconnected");
      nowConnected = false;
    }
    _previousTransportState = nowConnected;

    if (!nowConnected) {
      if (millis() < _nextIrcReconnectAt) return;

      String error;
      emitStatusEvent("Connecting IRC...");
      uint32_t connectStartedAtMs = millis();
      metricsLog("IRC session configuration: bouncer=" +
        (_ircCfg.bncEnabled ? String("enabled, type=") + bouncerModeToString(_ircCfg.bncMode) : String("disabled")));
      metricsLog("IRC transport connection started: host=" + _ircCfg.endpointHost +
        ", port=" + String(_ircCfg.endpointPort) +
        ", tls=" + String(_ircCfg.useTLS ? "true" : "false"));
      if (_transport.connect(_ircCfg, error)) {
        metricsLog("IRC transport connection finished: host=" + _ircCfg.endpointHost +
          ", duration=" + formatSeconds(millis() - connectStartedAtMs) + " seconds");
        _previousTransportState = true;
        _lastRxMs = millis();
        _awaitingPong = false;
        _capNegotiationDone = false;
        _capRequestSent = false;
        _capLsPending = false;
        _capLsAccum = "";
        _serverCaps.clear();
        _enabledCaps.clear();
        _sojuBoundNetId = "";
        _sojuBindSent = false;
        _sojuListRequested = false;
        _sojuNetworkBatchId = "";
        _sojuBatchNetworks.clear();
        _sojuNetworks.clear();
        _saslInProgress = false;
        _saslCompleted = false;
        _saslWaitingForChallenge = false;
        performRegistration();
        emitStatusEvent("IRC transport connected");
        emitNetStateEvent();
      } else {
        metricsLog("IRC transport connection failed: host=" + _ircCfg.endpointHost +
          ", duration=" + formatSeconds(millis() - connectStartedAtMs) + " seconds, error=" + error);
        scheduleReconnect("Connect failed: " + error);
      }
      return;
    }

    size_t rxBytesProcessed = 0;
    size_t rxLinesProcessed = 0;
    while (_transport.available() &&
           rxBytesProcessed < IRC_RX_BYTE_BUDGET_PER_LOOP &&
           rxLinesProcessed < IRC_RX_LINE_BUDGET_PER_LOOP) {
      int ch = _transport.read();
      if (ch < 0) break;
      ++rxBytesProcessed;
      char c = static_cast<char>(ch);
      if (c == '\r') continue;
      if (c == '\n') {
        if (!_rxBuffer.isEmpty()) {
          handleRawLine(_rxBuffer);
          _rxBuffer = "";
          ++rxLinesProcessed;
        }
      } else {
        _rxBuffer += c;
      }
      _lastRxMs = millis();
    }

    if (!_awaitingPong && millis() - _lastRxMs > PING_INTERVAL_MS) {
      _lastPingToken = String(millis());
      beginPingMetric(_lastPingToken);
      ircSendRawNoEcho("PING :" + _lastPingToken);
      _lastPingMs = millis();
      _awaitingPong = true;
      emitStatusEvent("Ping -> " + _lastPingToken);
    }

    if (_awaitingPong && millis() - _lastPingMs > PONG_TIMEOUT_MS) {
      scheduleReconnect("Ping timeout");
    }
  }

  void performRegistration() {
    String passLine = buildPassLine();
    if (!passLine.isEmpty()) ircSendRawNoEcho("PASS " + passLine);
    ircSendRawNoEcho("CAP LS 302");
    ircSendRawNoEcho("NICK " + _ircCfg.nick);
    ircSendRawNoEcho("USER " + buildRegistrationUsername() + " 0 * :" + _ircCfg.realname);
  }

  String buildSojuIdentity(const String& overrideUser = "") const {
    String user = trimCopy(overrideUser);
    if (user.isEmpty()) user = _ircCfg.bncUser;
    if (user.isEmpty()) return user;

    bool hasNetwork = user.indexOf('/') >= 0;
    bool hasClient = user.indexOf('@') >= 0;
    if (!hasNetwork && !_ircCfg.bncNetwork.isEmpty()) {
      user += "/" + _ircCfg.bncNetwork;
    }
    if (!hasClient && !_ircCfg.bncClient.isEmpty()) {
      user += "@" + _ircCfg.bncClient;
    }
    return user;
  }

  String buildRegistrationUsername() const {
    if (_ircCfg.bncEnabled && _ircCfg.bncMode == BouncerMode::Soju) {
      String user = buildSojuIdentity();
      if (!user.isEmpty()) return user;
    }
    return _ircCfg.username;
  }

  String buildPassLine() const {
    if (!_ircCfg.serverPass.isEmpty()) return _ircCfg.serverPass;
    if (_ircCfg.bncEnabled && !_ircCfg.bncPass.isEmpty()) {
      if (_ircCfg.bncMode == BouncerMode::Soju) {
        return _ircCfg.saslEnabled ? "" : _ircCfg.bncPass;
      }
      String left = _ircCfg.bncUser;
      if (!_ircCfg.bncNetwork.isEmpty()) left += "/" + _ircCfg.bncNetwork;
      if (!left.isEmpty()) return left + ":" + _ircCfg.bncPass;
      return _ircCfg.bncPass;
    }
    return "";
  }

  String buildSaslUser() const {
    if (_ircCfg.bncEnabled && _ircCfg.bncMode == BouncerMode::Soju) {
      String explicitUser = trimCopy(_ircCfg.saslUser);
      if (!explicitUser.isEmpty()) return buildSojuIdentity(explicitUser);
      String user = buildSojuIdentity();
      if (!user.isEmpty()) return user;
    }
    if (!_ircCfg.saslUser.isEmpty()) return _ircCfg.saslUser;
    if (_ircCfg.bncEnabled && !_ircCfg.bncUser.isEmpty()) return _ircCfg.bncUser;
    return _ircCfg.nick;
  }

  String buildSaslPass() const {
    if (!_ircCfg.saslPass.isEmpty()) return _ircCfg.saslPass;
    if (_ircCfg.bncEnabled && !_ircCfg.bncPass.isEmpty()) return _ircCfg.bncPass;
    return "";
  }

  void ircSendRawNoEcho(const String& line) {
    if (!_transport.connected()) return;
    _transport.write(line + "\r\n");
  }

  void ircSendRaw(const String& line) {
    if (!_transport.connected()) return;
    _transport.write(line + "\r\n");
  }

  void sendRaw(const String& line) {
    if (line.isEmpty()) return;
    if (!_uiTransportConnected) {
      logStatus("IRC is not connected");
      return;
    }
    if (!queueRawCommand(line)) {
      logCommandQueueFailure("IRC command");
      return;
    }
    appendLine(statusTab(), "--> " + line, currentTimeStampShort(), currentTimeStampLong());
  }

  static String decodeTagValue(const String& in) {
    String out;
    for (size_t i = 0; i < in.length(); ++i) {
      if (in[i] == '\\' && i + 1 < in.length()) {
        char n = in[++i];
        if (n == ':') out += ';';
        else if (n == 's') out += ' ';
        else if (n == 'r') out += '\r';
        else if (n == 'n') out += '\n';
        else out += n;
      } else {
        out += in[i];
      }
    }
    return out;
  }

  IrcMessage parseMessage(const String& raw) {
    IrcMessage msg;
    msg.raw = raw;

    int i = 0;
    if (raw.startsWith("@")) {
      int space = raw.indexOf(' ');
      if (space > 1) {
        String tags = raw.substring(1, space);
        int start = 0;
        while (start <= static_cast<int>(tags.length())) {
          int semi = tags.indexOf(';', start);
          String token = semi < 0 ? tags.substring(start) : tags.substring(start, semi);
          int eq = token.indexOf('=');
          TagEntry tag;
          tag.key = eq >= 0 ? token.substring(0, eq) : token;
          tag.value = eq >= 0 ? decodeTagValue(token.substring(eq + 1)) : "";
          msg.tags.push_back(tag);
          if (semi < 0) break;
          start = semi + 1;
        }
        i = space + 1;
      }
    }

    if (i < raw.length() && raw[i] == ':') {
      int sp = raw.indexOf(' ', i);
      if (sp > i) {
        msg.prefix = raw.substring(i + 1, sp);
        i = sp + 1;
      }
    }

    int cmdEnd = raw.indexOf(' ', i);
    if (cmdEnd < 0) {
      msg.command = raw.substring(i);
      return msg;
    }

    msg.command = raw.substring(i, cmdEnd);
    i = cmdEnd + 1;

    while (i < raw.length()) {
      if (raw[i] == ':') {
        msg.params.push_back(raw.substring(i + 1));
        break;
      }
      int sp = raw.indexOf(' ', i);
      if (sp < 0) {
        msg.params.push_back(raw.substring(i));
        break;
      }
      msg.params.push_back(raw.substring(i, sp));
      while (sp < raw.length() && raw[sp] == ' ') ++sp;
      i = sp;
    }

    return msg;
  }

  String getTagValue(const IrcMessage& msg, const String& key) const {
    for (const TagEntry& tag : msg.tags) {
      if (tag.key == key) return tag.value;
    }
    return "";
  }

  std::vector<TagEntry> parseTagEntries(const String& raw) const {
    std::vector<TagEntry> entries;
    int start = 0;
    while (start <= static_cast<int>(raw.length())) {
      int semi = raw.indexOf(';', start);
      String token = semi < 0 ? raw.substring(start) : raw.substring(start, semi);
      if (!token.isEmpty()) {
        int eq = token.indexOf('=');
        TagEntry entry;
        entry.key = eq >= 0 ? token.substring(0, eq) : token;
        entry.value = eq >= 0 ? decodeTagValue(token.substring(eq + 1)) : "";
        entries.push_back(entry);
      }
      if (semi < 0) break;
      start = semi + 1;
    }
    return entries;
  }

  void applySojuNetworkAttribute(SojuNetwork& network, const String& key, const String& value) {
    if (key == "name") network.name = value;
    else if (key == "state") network.state = value;
    else if (key == "host") network.host = value;
    else if (key == "port") network.port = value;
    else if (key == "tls") network.tls = value;
    else if (key == "nickname") network.nickname = value;
    else if (key == "username") network.username = value;
    else if (key == "realname") network.realname = value;
    else if (key == "pass") network.pass = value;
    else if (key == "error") network.error = value;
  }

  void applySojuNetworkAttributes(SojuNetwork& network, const String& attrsRaw) {
    for (const TagEntry& entry : parseTagEntries(attrsRaw)) {
      applySojuNetworkAttribute(network, entry.key, entry.value);
    }
  }

  void removeSojuNetworkById(std::vector<SojuNetwork>& list, const String& netId) {
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i].netId == netId) {
        list.erase(list.begin() + i);
        return;
      }
    }
  }

  SojuNetwork& ensureSojuNetwork(std::vector<SojuNetwork>& list, const String& netId) {
    for (SojuNetwork& network : list) {
      if (network.netId == netId) return network;
    }
    SojuNetwork network;
    network.netId = netId;
    list.push_back(network);
    return list.back();
  }

  String sojuNetworkLabel(const SojuNetwork& network) const {
    if (!network.name.isEmpty()) return network.name;
    if (!network.host.isEmpty()) return network.host;
    return network.netId;
  }

  String summarizeSojuNetwork(const SojuNetwork& network) const {
    String line = "[" + network.netId + "] " + sojuNetworkLabel(network);
    if (!network.state.isEmpty()) line += " state=" + network.state;
    if (!network.host.isEmpty() && !equalsIgnoreCase(network.host, sojuNetworkLabel(network))) line += " host=" + network.host;
    if (!network.port.isEmpty()) line += ":" + network.port;
    if (!network.tls.isEmpty()) line += String(" tls=") + (network.tls == "1" ? "on" : "off");
    if (!network.error.isEmpty()) line += " error=" + network.error;
    return line;
  }

  void appendSojuNetworksToStatus() {
    if (_sojuNetworks.empty()) {
      emitStatusEvent("soju networks: none");
      return;
    }
    emitStatusEvent("soju networks: " + String(_sojuNetworks.size()));
    for (const SojuNetwork& network : _sojuNetworks) {
      emitStatusEvent(summarizeSojuNetwork(network));
    }
  }

  String messageStampShort(const IrcMessage& msg) const {
    String t = getTagValue(msg, "time");
    if (t.length() >= 16 && t.indexOf('T') >= 0) {
      return t.substring(11, 16);
    }
    return currentTimeStampShort();
  }

  String messageStampLog(const IrcMessage& msg) const {
    String t = getTagValue(msg, "time");
    if (t.length() >= 19 && t.indexOf('T') >= 0) {
      return t.substring(11, 19);
    }
    return currentTimeStampLong();
  }

  void handleRawLine(const String& raw) {
    IrcMessage msg = parseMessage(raw);

    if (msg.command == "PING") {
      String payload = msg.params.empty() ? "cardputer" : msg.params.back();
      ircSendRawNoEcho("PONG :" + payload);
      emitStatusEvent("Ping <- " + payload);
      return;
    }

    if (msg.command == "PONG") {
      _awaitingPong = false;
      finishPingMetric(msg.params.empty() ? _lastPingToken : msg.params.back());
      emitStatusEvent("Pong " + (msg.params.empty() ? "" : msg.params.back()));
      return;
    }

    if (msg.command == "CAP") {
      handleCap(msg);
      return;
    }

    if (msg.command == "AUTHENTICATE") {
      handleAuthenticate(msg);
      return;
    }

    if (msg.command == "BATCH") {
      handleBatch(msg);
      return;
    }

    if (msg.command == "BOUNCER") {
      handleBouncer(msg);
      return;
    }

    if (msg.command == "FAIL") {
      handleFail(msg);
      return;
    }

    if (msg.command == "001") {
      _ircRegistered = true;
      _ircSelfNick = _ircCfg.nick;
      emitSelfNickChanged(_ircSelfNick);
      resetReconnectBackoff();
      emitStatusEvent("Registered on IRC");
      emitNetStateEvent();
      beginBncChannelAttachMetric("post-registration");
      if (capEnabled("soju.im/bouncer-networks") && !capEnabled("soju.im/bouncer-networks-notify")) {
        ircSendRawNoEcho("BOUNCER LISTNETWORKS");
      }
      autoJoinRestoredChannels();
      return;
    }

    if (msg.command == "005") {
      for (size_t i = 1; i + 1 < msg.params.size(); ++i) {
        String token = msg.params[i];
        if (token.startsWith("CHANTYPES=")) {
          _ircChanTypes = token.substring(strlen("CHANTYPES="));
        }
        if (token.startsWith("BOUNCER_NETID")) {
          int eq = token.indexOf('=');
          _sojuBoundNetId = eq >= 0 ? token.substring(eq + 1) : "";
        }
      }
      enqueueUiRawLine(msg.command, raw);
      return;
    }

    noteBncObservedChannelFromMessage(msg);

    if (msg.command == "433") {
      _ircCfg.nick += "_";
      _ircSelfNick = _ircCfg.nick;
      emitSelfNickChanged(_ircSelfNick);
      ircSendRaw("NICK " + _ircCfg.nick);
      emitStatusEvent("Nick in use, retrying as " + _ircCfg.nick);
      return;
    }

    if (msg.command == "900" || msg.command == "903") {
      _saslCompleted = true;
      _saslInProgress = false;
      _saslWaitingForChallenge = false;
      enqueueUiRawLine(msg.command, raw);
      if (!_capNegotiationDone) {
        maybeSendSojuBind();
        ircSendRaw("CAP END");
        _capNegotiationDone = true;
      }
      return;
    }

    if (msg.command == "904" || msg.command == "905" || msg.command == "906" || msg.command == "907") {
      _saslInProgress = false;
      _saslWaitingForChallenge = false;
      enqueueUiRawLine(msg.command, raw);
      if (!_capNegotiationDone) {
        ircSendRaw("CAP END");
        _capNegotiationDone = true;
      }
      return;
    }

    if (msg.command == "TAGMSG") {
      // Ignore TAGMSG in the UI to avoid flooding from typing/activity tags.
      return;
    }
    if (msg.command == "JOIN" && !msg.params.empty()) {
      String nick = nickFromPrefix(msg.prefix);
      String channel = msg.params[0];
      if (equalsIgnoreCase(nick, _ircSelfNick)) {
        noteJoinedChannel(channel);
        bool exists = false;
        for (const String& known : _knownChannels) {
          if (equalsIgnoreCase(known, channel)) {
            exists = true;
            break;
          }
        }
        if (!exists) _knownChannels.push_back(channel);
      }
      enqueueUiRawLine(msg.command, raw);
      return;
    }
    if (msg.command == "PART" && !msg.params.empty()) {
      String nick = nickFromPrefix(msg.prefix);
      if (equalsIgnoreCase(nick, _ircSelfNick)) {
        String channel = msg.params[0];
        for (size_t i = 0; i < _knownChannels.size(); ++i) {
          if (equalsIgnoreCase(_knownChannels[i], channel)) {
            _knownChannels.erase(_knownChannels.begin() + i);
            break;
          }
        }
      }
      enqueueUiRawLine(msg.command, raw);
      return;
    }
    if (msg.command == "NICK" && !msg.params.empty()) {
      String oldNick = nickFromPrefix(msg.prefix);
      if (equalsIgnoreCase(oldNick, _ircSelfNick)) {
        _ircSelfNick = msg.params[0];
        _ircCfg.nick = _ircSelfNick;
        emitSelfNickChanged(_ircSelfNick);
      }
      enqueueUiRawLine(msg.command, raw);
      return;
    }
    if (msg.command == "321") {
      _channelListMetric.entries = 0;
      _channelListMetric.truncated = false;
      enqueueUiRawLine(msg.command, raw);
      return;
    }
    if (msg.command == "322") {
      if (_channelListMetric.active) {
        if (_channelListMetric.entries < MAX_CHANNEL_LIST_ENTRIES) ++_channelListMetric.entries;
        else _channelListMetric.truncated = true;
      }
      enqueueUiRawLine(msg.command, raw);
      return;
    }
    if (msg.command == "323") {
      enqueueUiRawLine(msg.command, raw);
      finishChannelListMetric();
      return;
    }

    enqueueUiRawLine(msg.command, raw);
  }

  void handleUiRawLine(const String& raw) {
    IrcMessage msg = parseMessage(raw);

    if (msg.command == "001") {
      appendLine(statusTab(), "*** Registered on IRC", messageStampShort(msg), messageStampLog(msg));
      return;
    }

    if (msg.command == "005") {
      handleISupport(msg);
      appendLine(statusTab(), formatNumeric(msg), messageStampShort(msg), messageStampLog(msg));
      return;
    }

    if (msg.command == "900" || msg.command == "903" ||
        msg.command == "904" || msg.command == "905" ||
        msg.command == "906" || msg.command == "907") {
      appendLine(statusTab(), formatNumeric(msg), messageStampShort(msg), messageStampLog(msg));
      return;
    }

    if (msg.command == "JOIN") {
      handleJoin(msg);
      return;
    }
    if (msg.command == "PART") {
      handlePart(msg);
      return;
    }
    if (msg.command == "QUIT") {
      handleQuit(msg);
      return;
    }
    if (msg.command == "KICK") {
      handleKick(msg);
      return;
    }
    if (msg.command == "NICK") {
      handleNick(msg);
      return;
    }
    if (msg.command == "PRIVMSG") {
      handlePrivmsg(msg, false);
      return;
    }
    if (msg.command == "NOTICE") {
      handlePrivmsg(msg, true);
      return;
    }
    if (msg.command == "TAGMSG") {
      handleTagmsg(msg);
      return;
    }
    if (msg.command == "TOPIC") {
      handleTopicChange(msg);
      return;
    }
    if (msg.command == "332" || msg.command == "333") {
      handleTopicReply(msg);
      return;
    }
    if (msg.command == "353") {
      handleNames(msg);
      return;
    }
    if (msg.command == "366") {
      Tab* tab = msg.params.size() > 1 ? findTab(msg.params[1]) : nullptr;
      if (tab) {
        if (tab->namesBatchActive) {
          if (tab->usersDirty) sortUsers(*tab);
          tab->namesBatchActive = false;
        }
        appendLine(*tab, "*** End of NAMES", messageStampShort(msg), messageStampLog(msg));
      }
      return;
    }
    if (msg.command == "MODE") {
      handleMode(msg);
      return;
    }
    if (msg.command == "321" || msg.command == "322" || msg.command == "323") {
      handleChannelListNumeric(msg);
      return;
    }

    if (!msg.command.isEmpty() && isDigitString(msg.command)) {
      appendLine(statusTab(), formatNumeric(msg), messageStampShort(msg), messageStampLog(msg));
      return;
    }

    appendLine(statusTab(), raw, messageStampShort(msg), messageStampLog(msg));
  }

  void handleBatch(const IrcMessage& msg) {
    if (msg.params.empty()) return;
    String ref = msg.params[0];
    if (ref.length() < 2) return;

    char sign = ref[0];
    String batchId = ref.substring(1);
    if (sign == '+') {
      String type = msg.params.size() > 1 ? msg.params[1] : "";
      if (equalsIgnoreCase(type, "soju.im/bouncer-networks")) {
        _sojuNetworkBatchId = batchId;
        _sojuBatchNetworks.clear();
      }
      return;
    }

    if (sign == '-' && batchId == _sojuNetworkBatchId) {
      _sojuNetworkBatchId = "";
      _sojuNetworks = _sojuBatchNetworks;
      _sojuBatchNetworks.clear();
      if (_sojuListRequested) {
        appendSojuNetworksToStatus();
        _sojuListRequested = false;
      } else {
        emitStatusEvent("soju networks synced: " + String(_sojuNetworks.size()));
      }
    }
  }

  void handleBouncerNetwork(const IrcMessage& msg) {
    if (msg.params.size() < 2) return;
    String netId = msg.params[1];
    bool inBatch = !_sojuNetworkBatchId.isEmpty() && getTagValue(msg, "batch") == _sojuNetworkBatchId;
    std::vector<SojuNetwork>& list = inBatch ? _sojuBatchNetworks : _sojuNetworks;

    if (msg.params.size() >= 3 && msg.params[2] == "*") {
      removeSojuNetworkById(list, netId);
      if (!inBatch) emitStatusEvent("soju network removed: [" + netId + "]");
      if (_sojuBoundNetId == netId) _sojuBoundNetId = "";
      return;
    }

    String attrs = msg.params.size() >= 3 ? msg.params[2] : "";
    SojuNetwork& network = ensureSojuNetwork(list, netId);
    applySojuNetworkAttributes(network, attrs);
    if (!inBatch) emitStatusEvent("soju network: " + summarizeSojuNetwork(network));
    if (!inBatch && _ircCfg.bncEnabled && _ircCfg.bncMode == BouncerMode::Soju &&
        equalsIgnoreCase(network.state, "connected")) {
      beginBncChannelAttachMetric("soju-network-connected");
    }
  }

  void handleBouncer(const IrcMessage& msg) {
    if (msg.params.empty()) return;
    String sub = msg.params[0];
    sub.toUpperCase();

    if (sub == "NETWORK") {
      handleBouncerNetwork(msg);
      return;
    }
    if (sub == "ADDNETWORK" && msg.params.size() >= 2) {
      emitStatusEvent("soju addnetwork ok: [" + msg.params[1] + "]");
      return;
    }
    if (sub == "CHANGENETWORK" && msg.params.size() >= 2) {
      emitStatusEvent("soju changenetwork ok: [" + msg.params[1] + "]");
      return;
    }
    if (sub == "DELNETWORK" && msg.params.size() >= 2) {
      emitStatusEvent("soju delnetwork ok: [" + msg.params[1] + "]");
      return;
    }
    if (sub == "BIND" && msg.params.size() >= 2) {
      _sojuBoundNetId = msg.params[1];
      _sojuBindSent = true;
      emitStatusEvent("soju bound network: [" + _sojuBoundNetId + "]");
      return;
    }

    emitStatusEvent("BOUNCER " + joinStrings(msg.params, " "));
  }

  void handleFail(const IrcMessage& msg) {
    if (msg.params.empty()) {
      emitStatusEvent("FAIL");
      return;
    }
    if (equalsIgnoreCase(msg.params[0], "BOUNCER")) {
      String line = "BOUNCER FAIL";
      for (const String& param : msg.params) line += " " + param;
      emitStatusEvent(line);
      return;
    }
    emitStatusEvent("FAIL " + joinStrings(msg.params, " "));
  }

  void autoJoinRestoredChannels() {
    std::vector<String> joined;
    for (const String& c : _ircCfg.autoJoin) {
      if (!c.isEmpty()) joined.push_back(c);
    }
    for (const String& known : _knownChannels) {
      bool exists = false;
      for (const String& c : joined) {
        if (equalsIgnoreCase(c, known)) {
          exists = true;
          break;
        }
      }
      if (!exists) joined.push_back(known);
    }
    beginChannelJoinMetric(joined, "autojoin");
    for (const String& c : joined) {
      ircSendRaw("JOIN " + c);
    }
  }

  std::vector<String> splitCaps(const String& capList) const {
    std::vector<String> out;
    int start = 0;
    while (start < static_cast<int>(capList.length())) {
      while (start < static_cast<int>(capList.length()) && capList[start] == ' ') ++start;
      if (start >= static_cast<int>(capList.length())) break;
      int sp = capList.indexOf(' ', start);
      String token = sp < 0 ? capList.substring(start) : capList.substring(start, sp);
      if (!token.isEmpty()) out.push_back(token);
      if (sp < 0) break;
      start = sp + 1;
    }
    return out;
  }

  bool serverSupportsCap(const String& capName) const {
    for (const String& cap : _serverCaps) {
      if (equalsIgnoreCase(normalizeCapName(cap), capName)) return true;
    }
    return false;
  }

  bool capEnabled(const String& capName) const {
    for (const String& cap : _enabledCaps) {
      if (equalsIgnoreCase(cap, capName)) return true;
    }
    return false;
  }

  void setEnabledCap(const String& capName, bool enabled) {
    for (size_t i = 0; i < _enabledCaps.size(); ++i) {
      if (equalsIgnoreCase(_enabledCaps[i], capName)) {
        if (!enabled) _enabledCaps.erase(_enabledCaps.begin() + i);
        return;
      }
    }
    if (enabled) _enabledCaps.push_back(capName);
  }

  void maybeSendSojuBind() {
    if (!_ircCfg.bncEnabled || _ircCfg.bncMode != BouncerMode::Soju) return;
    if (_sojuBindSent || _ircRegistered) return;
    if (!capEnabled("soju.im/bouncer-networks")) return;

    String netId = trimCopy(_ircCfg.sojuBindNetId);
    if (netId.isEmpty()) return;

    bool waitingOnSasl = capEnabled("sasl") && _ircCfg.saslEnabled && !_saslCompleted;
    if (waitingOnSasl) return;

    ircSendRaw("BOUNCER BIND " + netId);
    _sojuBindSent = true;
  }

  void handleCap(const IrcMessage& msg) {
    if (msg.params.size() < 2) return;
    String sub = msg.params[1];
    sub.toUpperCase();

    if (sub == "LS" || sub == "NEW") {
      String chunk = msg.params.back();
      bool cont = msg.params.size() >= 4 && msg.params[2] == "*";
      if (!_capLsAccum.isEmpty()) _capLsAccum += ' ';
      _capLsAccum += chunk;
      _capLsPending = cont;
      if (cont) return;

      _serverCaps = splitCaps(_capLsAccum);
      _capLsAccum = "";
      _capLsPending = false;

      std::vector<String> want;
      if (serverSupportsCap("multi-prefix")) want.push_back("multi-prefix");
      if (serverSupportsCap("server-time")) want.push_back("server-time");
      if (serverSupportsCap("message-tags")) want.push_back("message-tags");
      if (_ircCfg.saslEnabled && serverSupportsCap("sasl")) want.push_back("sasl");
      if (_ircCfg.bncEnabled && _ircCfg.bncMode == BouncerMode::Soju && serverSupportsCap("soju.im/bouncer-networks")) {
        want.push_back("soju.im/bouncer-networks");
        if (serverSupportsCap("soju.im/bouncer-networks-notify")) {
          want.push_back("soju.im/bouncer-networks-notify");
        }
      }

      if (!want.empty() && (sub == "NEW" || !_capRequestSent)) {
        ircSendRaw("CAP REQ :" + joinStrings(want, " "));
        _capRequestSent = true;
      } else if (!_capNegotiationDone && !_saslInProgress) {
        ircSendRaw("CAP END");
        _capNegotiationDone = true;
      }
      return;
    }

    if (sub == "ACK") {
      std::vector<String> ackCaps = splitCaps(msg.params.back());
      bool saslAck = false;
      for (const String& cap : ackCaps) {
        String name = cap;
        bool disable = false;
        if (!name.isEmpty() && name[0] == '-') {
          disable = true;
          name.remove(0, 1);
        }
        name = normalizeCapName(name);
        setEnabledCap(name, !disable);
        if (equalsIgnoreCase(name, "sasl") && !disable) saslAck = true;
      }

      if (saslAck && _ircCfg.saslEnabled && !_saslCompleted && !_saslInProgress) {
        _saslInProgress = true;
        _saslWaitingForChallenge = true;
        ircSendRaw("AUTHENTICATE PLAIN");
      } else if (!_capNegotiationDone && !_saslInProgress) {
        maybeSendSojuBind();
        ircSendRaw("CAP END");
        _capNegotiationDone = true;
      }
      return;
    }

    if (sub == "NAK") {
      emitStatusEvent("CAP NAK: " + msg.params.back());
      if (!_capNegotiationDone && !_saslInProgress) {
        ircSendRaw("CAP END");
        _capNegotiationDone = true;
      }
      return;
    }

    if (sub == "DEL") {
      std::vector<String> delCaps = splitCaps(msg.params.back());
      for (const String& cap : delCaps) {
        setEnabledCap(normalizeCapName(cap), false);
      }
      emitStatusEvent("CAP DEL: " + msg.params.back());
      return;
    }
  }

  static String base64Encode(const String& in) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    int val = 0;
    int valb = -6;
    for (size_t i = 0; i < in.length(); ++i) {
      val = (val << 8) + static_cast<uint8_t>(in[i]);
      valb += 8;
      while (valb >= 0) {
        out += table[(val >> valb) & 0x3F];
        valb -= 6;
      }
    }
    if (valb > -6) out += table[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.length() % 4) out += '=';
    return out;
  }

  static String base64EncodeBytes(const uint8_t* bytes, size_t len) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    int val = 0;
    int valb = -6;
    for (size_t i = 0; i < len; ++i) {
      val = (val << 8) + bytes[i];
      valb += 8;
      while (valb >= 0) {
        out += table[(val >> valb) & 0x3F];
        valb -= 6;
      }
    }
    if (valb > -6) out += table[((val << 8) >> (valb + 8)) & 0x3F];
    while (out.length() % 4) out += '=';
    return out;
  }

  void sendSaslPlainPayload() {
    String user = buildSaslUser();
    String pass = buildSaslPass();
    std::vector<uint8_t> payload;
    payload.reserve(user.length() * 2 + pass.length() + 2);
    for (size_t i = 0; i < user.length(); ++i) payload.push_back(static_cast<uint8_t>(user[i]));
    payload.push_back(0);
    for (size_t i = 0; i < user.length(); ++i) payload.push_back(static_cast<uint8_t>(user[i]));
    payload.push_back(0);
    for (size_t i = 0; i < pass.length(); ++i) payload.push_back(static_cast<uint8_t>(pass[i]));

    String encoded = base64EncodeBytes(payload.data(), payload.size());
    const int chunkSize = 400;
    for (int i = 0; i < static_cast<int>(encoded.length()); i += chunkSize) {
      ircSendRaw("AUTHENTICATE " + encoded.substring(i, i + chunkSize));
    }
    if (encoded.length() % chunkSize == 0) {
      ircSendRaw("AUTHENTICATE +");
    }
    _saslWaitingForChallenge = false;
  }

  void handleAuthenticate(const IrcMessage& msg) {
    if (!_saslInProgress || msg.params.empty()) return;
    if (equalsIgnoreCase(_ircCfg.saslMechanism, "PLAIN") && msg.params[0] == "+") {
      sendSaslPlainPayload();
    }
  }

  void handleISupport(const IrcMessage& msg) {
    if (msg.params.size() < 3) return;
    for (size_t i = 1; i + 1 < msg.params.size(); ++i) {
      String token = msg.params[i];
      if (token.startsWith("CHANTYPES=")) {
        _chanTypes = token.substring(strlen("CHANTYPES="));
      } else if (token.startsWith("PREFIX=")) {
        int lp = token.indexOf('(');
        int rp = token.indexOf(')');
        if (lp >= 0 && rp > lp) {
          _prefixModes = token.substring(lp + 1, rp);
          _prefixSymbols = token.substring(rp + 1);
        }
      }
    }
  }

  String formatNumeric(const IrcMessage& msg) {
    String code = msg.command;
    if (code == "311" && msg.params.size() >= 6) {
      return "[311] WHOIS " + msg.params[1] + " user=" + msg.params[2] + " host=" + msg.params[3] + " name=" + msg.params[5];
    }
    if (code == "312" && msg.params.size() >= 3) {
      return "[312] WHOIS server " + msg.params[1] + " -> " + msg.params[2] + (msg.params.size() > 3 ? " (" + msg.params[3] + ")" : "");
    }
    if (code == "317" && msg.params.size() >= 3) {
      return "[317] WHOIS idle " + msg.params[1] + " " + msg.params[2] + "s";
    }
    if (code == "318" && msg.params.size() >= 2) {
      return "[318] End of WHOIS for " + msg.params[1];
    }
    if (code == "319" && msg.params.size() >= 3) {
      return "[319] WHOIS channels " + msg.params[1] + ": " + msg.params[2];
    }
    if (code == "322" && msg.params.size() >= 4) {
      return "[322] LIST " + msg.params[1] + " users=" + msg.params[2] + " topic=" + msg.params[3];
    }
    if (code == "331" && msg.params.size() >= 2) {
      return "[331] No topic for " + msg.params[1];
    }
    if (code == "332" && msg.params.size() >= 3) {
      return "[332] Topic for " + msg.params[1] + ": " + msg.params[2];
    }
    if (code == "333" && msg.params.size() >= 4) {
      return "[333] Topic set by " + msg.params[2] + " at " + msg.params[3];
    }
    if (code == "353" && msg.params.size() >= 4) {
      return "[353] Names " + msg.params[2] + ": " + msg.params[3];
    }
    if (code == "366" && msg.params.size() >= 2) {
      return "[366] End of NAMES for " + msg.params[1];
    }
    if (code == "900" || code == "903" || code == "904" || code == "905" || code == "906" || code == "907") {
      return "[" + code + "] " + (msg.params.empty() ? "SASL" : msg.params.back());
    }

    String out = "[" + msg.command + "]";
    for (size_t i = 1; i < msg.params.size(); ++i) {
      out += (i == 1 ? " " : " | ");
      out += msg.params[i];
    }
    return out;
  }

  bool lineMentionsNick(const String& text, const String& nick) const {
    if (nick.isEmpty()) return false;
    String hay = lowerCopy(stripIrcFormatting(text));
    String needle = lowerCopy(nick);
    int pos = 0;
    while (true) {
      pos = hay.indexOf(needle, pos);
      if (pos < 0) return false;
      bool leftOk = pos == 0 || !isNickChar(hay[pos - 1]);
      int rightPos = pos + needle.length();
      bool rightOk = rightPos >= static_cast<int>(hay.length()) || !isNickChar(hay[rightPos]);
      if (leftOk && rightOk) return true;
      pos += needle.length();
    }
  }

  char extractPrefixFromNick(String& nick) const {
    char prefix = 0;
    while (!nick.isEmpty() && _prefixSymbols.indexOf(nick[0]) >= 0) {
      prefix = nick[0];
      nick.remove(0, 1);
    }
    return prefix;
  }

  int prefixWeight(char prefix) const {
    int idx = _prefixSymbols.indexOf(prefix);
    return idx >= 0 ? idx : 99;
  }

  Tab& statusTab() {
    ensureStatusTab();
    return _tabs[0];
  }

  void ensureStatusTab() {
    if (_tabs.empty()) {
      Tab t;
      t.name = "status";
      t.type = TabType::Status;
      _tabs.push_back(t);
      _activeTab = 0;
    }
  }

  Tab* findTab(const String& name) {
    for (Tab& tab : _tabs) {
      if (equalsIgnoreCase(tab.name, name)) return &tab;
    }
    return nullptr;
  }

  Tab& getOrCreateTab(const String& name, TabType type) {
    if (Tab* existing = findTab(name)) return *existing;
    if (_tabs.size() >= MAX_TABS) return statusTab();
    Tab tab;
    tab.name = name;
    tab.type = type;
    _tabs.push_back(tab);
    _dirty = true;
    markStateDirty();
    return _tabs.back();
  }

  bool hasUser(const Tab& tab, const String& nick) const {
    for (const UserEntry& entry : tab.users) {
      if (equalsIgnoreCase(entry.nick, nick)) return true;
    }
    return false;
  }

  static int compareNickIgnoreCase(const String& a, const String& b) {
    size_t count = std::min(a.length(), b.length());
    for (size_t i = 0; i < count; ++i) {
      unsigned char ca = static_cast<unsigned char>(a[i]);
      unsigned char cb = static_cast<unsigned char>(b[i]);
      int diff = tolower(ca) - tolower(cb);
      if (diff != 0) return diff;
    }
    if (a.length() < b.length()) return -1;
    if (a.length() > b.length()) return 1;
    return 0;
  }

  void sortUsers(Tab& tab) {
    std::sort(tab.users.begin(), tab.users.end(), [&](const UserEntry& a, const UserEntry& b) {
      int wa = prefixWeight(a.prefix);
      int wb = prefixWeight(b.prefix);
      if (wa != wb) return wa < wb;
      return compareNickIgnoreCase(a.nick, b.nick) < 0;
    });
    tab.usersDirty = false;
  }

  void addOrUpdateUser(Tab& tab, const String& nick, char prefix = 0, bool sortNow = true) {
    if (nick.isEmpty()) return;
    for (UserEntry& entry : tab.users) {
      if (equalsIgnoreCase(entry.nick, nick)) {
        if (prefix && prefixWeight(prefix) < prefixWeight(entry.prefix)) {
          entry.prefix = prefix;
          if (sortNow) sortUsers(tab);
          else tab.usersDirty = true;
        }
        return;
      }
    }
    if (tab.users.size() >= MAX_USERS_PER_TAB) return;
    UserEntry entry;
    entry.nick = nick;
    entry.prefix = prefix;
    tab.users.push_back(entry);
    if (sortNow) sortUsers(tab);
    else tab.usersDirty = true;
  }

  void removeUser(Tab& tab, const String& nick) {
    for (size_t i = 0; i < tab.users.size(); ++i) {
      if (equalsIgnoreCase(tab.users[i].nick, nick)) {
        tab.users.erase(tab.users.begin() + i);
        return;
      }
    }
  }

  void clearUsers(Tab& tab) {
    tab.users.clear();
    tab.usersDirty = false;
    tab.namesBatchActive = false;
  }

  void appendLine(Tab& tab, const String& rawText, const String& stampShort = "", const String& stampLog = "", bool highlight = false, bool own = false, bool notice = false) {
    ChatLine line;
    line.raw = sanitizeForDisplay(rawText);
    line.plain = stripIrcFormatting(line.raw);
    line.stampShort = stampShort.isEmpty() ? currentTimeStampShort() : stampShort;
    line.stampLog = stampLog.isEmpty() ? currentTimeStampLong() : stampLog;
    line.highlight = highlight;
    line.own = own;
    line.notice = notice;
    tab.lines.push_back(line);
    if (tab.lines.size() > MAX_TAB_LINES) tab.lines.erase(tab.lines.begin());
    if (tab.scroll > 0) {
      tab.scroll += scrollUnitsForLine(tab, line);
      clampTabScroll(tab);
    }
    if (&tab != &_tabs[_activeTab]) {
      tab.unread = true;
      if (highlight) tab.mention = true;
    }
    logToSD(tab.name, line.stampLog + " " + line.plain);
    _dirty = true;
  }

  void logStatus(const String& s) {
    appendLine(statusTab(), "*** " + s);
  }

  String logTabFolderName(const String& tabName) const {
    if (tabName.isEmpty()) return "status";
    if (equalsIgnoreCase(tabName, "status")) return "status";
    if (isChannelName(tabName)) return safeFileName(tabName);
    return "query_" + safeFileName(tabName);
  }

  void logToSD(const String& tabName, const String& line) {
    if (!_sdReady || !_cfg.channelLogEnabled) return;
    PendingSdLogLine entry;
    entry.path = resolveLogPath(tabName);
    if (entry.path.isEmpty()) return;
    entry.line = line;
    if (_pendingSdLogs.size() >= MAX_PENDING_SD_LOG_LINES) {
      _pendingSdLogs.pop_front();
    }
    _pendingSdLogs.push_back(entry);
  }

  void handleJoin(const IrcMessage& msg) {
    String nick = nickFromPrefix(msg.prefix);
    String channel = msg.params.empty() ? "" : msg.params[0];
    if (channel.isEmpty()) return;

    Tab& tab = getOrCreateTab(channel, TabType::Channel);
    if (equalsIgnoreCase(nick, _selfNick)) {
      appendLine(tab, "*** Joined " + channel, messageStampShort(msg), messageStampLog(msg));
      tab.unread = false;
      tab.mention = false;
      markStateDirty();
    } else {
      appendLine(tab, "*** " + nick + " joined", messageStampShort(msg), messageStampLog(msg));
      addOrUpdateUser(tab, nick);
    }
  }

  void handlePart(const IrcMessage& msg) {
    if (msg.params.empty()) return;
    String nick = nickFromPrefix(msg.prefix);
    String channel = msg.params[0];
    String reason = msg.params.size() > 1 ? msg.params[1] : "";
    Tab* tab = findTab(channel);
    if (!tab) return;

    if (equalsIgnoreCase(nick, _selfNick)) {
      if (equalsIgnoreCase(reason, "detach")) {
        appendLine(*tab, "*** Detached " + channel, messageStampShort(msg), messageStampLog(msg));
      } else {
        appendLine(*tab, "*** You parted " + channel + (reason.isEmpty() ? "" : " (" + reason + ")"), messageStampShort(msg), messageStampLog(msg));
      }
      clearUsers(*tab);
    } else {
      appendLine(*tab, "*** " + nick + " parted" + (reason.isEmpty() ? "" : " (" + reason + ")"), messageStampShort(msg), messageStampLog(msg));
      removeUser(*tab, nick);
    }
  }

  void handleQuit(const IrcMessage& msg) {
    String nick = nickFromPrefix(msg.prefix);
    String reason = msg.params.empty() ? "" : msg.params[0];
    for (Tab& tab : _tabs) {
      if (tab.type == TabType::Channel && hasUser(tab, nick)) {
        appendLine(tab, "*** " + nick + " quit" + (reason.isEmpty() ? "" : " (" + reason + ")"), messageStampShort(msg), messageStampLog(msg));
        removeUser(tab, nick);
      }
    }
  }

  void handleKick(const IrcMessage& msg) {
    if (msg.params.size() < 2) return;
    Tab& tab = getOrCreateTab(msg.params[0], TabType::Channel);
    String victim = msg.params[1];
    String reason = msg.params.size() > 2 ? msg.params[2] : "";
    appendLine(tab, "*** " + victim + " was kicked by " + nickFromPrefix(msg.prefix) + (reason.isEmpty() ? "" : " (" + reason + ")"), messageStampShort(msg), messageStampLog(msg));
    removeUser(tab, victim);
  }

  void handleNick(const IrcMessage& msg) {
    if (msg.params.empty()) return;
    String oldNick = nickFromPrefix(msg.prefix);
    String newNick = msg.params[0];
    if (equalsIgnoreCase(oldNick, _selfNick)) {
      _selfNick = newNick;
      _cfg.nick = newNick;
    }
    for (Tab& tab : _tabs) {
      if (tab.type == TabType::Channel && hasUser(tab, oldNick)) {
        char prefix = 0;
        for (const UserEntry& entry : tab.users) {
          if (equalsIgnoreCase(entry.nick, oldNick)) {
            prefix = entry.prefix;
            break;
          }
        }
        removeUser(tab, oldNick);
        addOrUpdateUser(tab, newNick, prefix);
        appendLine(tab, "*** " + oldNick + " is now known as " + newNick, messageStampShort(msg), messageStampLog(msg));
      }
      if (tab.type == TabType::Query && equalsIgnoreCase(tab.name, oldNick)) {
        tab.name = newNick;
        markStateDirty();
      }
    }
  }

  void handlePrivmsg(const IrcMessage& msg, bool notice) {
    if (msg.params.size() < 2) return;

    String from = nickFromPrefix(msg.prefix);
    String target = msg.params[0];
    String text = msg.params[1];

    bool isAction = text.startsWith("\001ACTION ") && text.endsWith("\001");
    if (isAction) text = text.substring(8, text.length() - 1);

    Tab* tab = nullptr;
    if (isChannelName(target)) {
      tab = &getOrCreateTab(target, TabType::Channel);
    } else {
      tab = &getOrCreateTab(from, TabType::Query);
      markStateDirty();
    }

    bool highlight = isChannelName(target) && lineMentionsNick(text, _selfNick);
    String line;
    if (notice) line = "-" + from + "- " + text;
    else if (isAction) line = "* " + from + " " + text;
    else line = "<" + from + "> " + text;

    appendLine(*tab, line, messageStampShort(msg), messageStampLog(msg), highlight, false, notice);
  }

  void handleTagmsg(const IrcMessage& msg) {
    (void)msg;
    // TAGMSG is frequently used for ephemeral typing/activity tags on soju.
    // Ignore it in the UI to avoid flooding the status tab with raw lines.
  }

  void handleTopicReply(const IrcMessage& msg) {
    if (msg.command == "332" && msg.params.size() >= 3) {
      Tab& tab = getOrCreateTab(msg.params[1], TabType::Channel);
      tab.topic = msg.params[2];
      appendLine(tab, "*** Topic: " + msg.params[2], messageStampShort(msg), messageStampLog(msg));
    } else if (msg.command == "333" && msg.params.size() >= 4) {
      Tab& tab = getOrCreateTab(msg.params[1], TabType::Channel);
      appendLine(tab, "*** Topic set by " + msg.params[2] + " at " + msg.params[3], messageStampShort(msg), messageStampLog(msg));
    }
  }

  void handleTopicChange(const IrcMessage& msg) {
    if (msg.params.size() < 2) return;
    Tab& tab = getOrCreateTab(msg.params[0], TabType::Channel);
    tab.topic = msg.params[1];
    appendLine(tab, "*** " + nickFromPrefix(msg.prefix) + " changed topic to: " + msg.params[1], messageStampShort(msg), messageStampLog(msg));
  }

  void handleNames(const IrcMessage& msg) {
    if (msg.params.size() < 4) return;
    Tab& tab = getOrCreateTab(msg.params[2], TabType::Channel);
    if (!tab.namesBatchActive) {
      clearUsers(tab);
      tab.namesBatchActive = true;
    }

    int start = 0;
    String names = msg.params[3];
    while (start <= static_cast<int>(names.length())) {
      int sp = names.indexOf(' ', start);
      String nick = sp < 0 ? names.substring(start) : names.substring(start, sp);
      nick.trim();
      if (!nick.isEmpty()) {
        char prefix = extractPrefixFromNick(nick);
        addOrUpdateUser(tab, nick, prefix, false);
      }
      if (sp < 0) break;
      start = sp + 1;
    }
  }

  void updatePrefixFromMode(Tab& tab, const String& mode, const std::vector<String>& params, bool adding) {
    size_t paramIndex = 0;
    for (size_t i = 0; i < mode.length(); ++i) {
      char m = mode[i];
      if (m == '+') {
        adding = true;
        continue;
      }
      if (m == '-') {
        adding = false;
        continue;
      }
      int modeIdx = _prefixModes.indexOf(m);
      if (modeIdx >= 0 && paramIndex < params.size()) {
        String nick = params[paramIndex++];
        for (UserEntry& entry : tab.users) {
          if (equalsIgnoreCase(entry.nick, nick)) {
            entry.prefix = adding ? _prefixSymbols[modeIdx] : 0;
            break;
          }
        }
      } else if (paramIndex < params.size()) {
        ++paramIndex;
      }
    }
    sortUsers(tab);
  }

  void handleMode(const IrcMessage& msg) {
    if (msg.params.size() < 2) return;
    String target = msg.params[0];
    String mode = msg.params[1];
    std::vector<String> rest;
    for (size_t i = 2; i < msg.params.size(); ++i) rest.push_back(msg.params[i]);
    Tab* tab = findTab(target);
    if (!tab) tab = &statusTab();
    appendLine(*tab, "*** " + nickFromPrefix(msg.prefix) + " sets mode " + mode + (rest.empty() ? "" : " " + joinStrings(rest, " ")), messageStampShort(msg), messageStampLog(msg));
    if (tab->type == TabType::Channel) updatePrefixFromMode(*tab, mode, rest, true);
  }

  void handleChannelListNumeric(const IrcMessage& msg) {
    if (msg.command == "321") {
      _channelListLoading = true;
      _channelListTruncated = false;
      _channelList.clear();
      _channelListSelected = 0;
      _channelListScroll = 0;
      _dirty = true;
      return;
    }

    if (msg.command == "322") {
      if (msg.params.size() < 4) return;
      long users = msg.params[2].toInt();
      if (users < 0) users = 0;
      if (users > 65535) users = 65535;
      addChannelListEntry(msg.params[1], static_cast<uint16_t>(users), msg.params[3]);
      return;
    }

    if (msg.command == "323") {
      finalizeChannelList();
    }
  }

  void applyKeyboardScroll(Tab& tab, int delta) {
    tab.scroll = std::max(0, tab.scroll + delta);
    clampTabScroll(tab);
    _dirty = true;
  }

  bool handleFnScrollShortcut(char c) {
    if (_tabs.empty()) return false;

    Tab& tab = _tabs[_activeTab];
    int page = std::max(1, bodyVisibleRows() - 1);

    switch (c) {
      case ';':
        applyKeyboardScroll(tab, 1);
        return true;
      case '.':
        applyKeyboardScroll(tab, -1);
        return true;
      case ',':
        applyKeyboardScroll(tab, page);
        return true;
      case '/':
        applyKeyboardScroll(tab, -page);
        return true;
      default:
        return false;
    }
  }

  void handleKeyboard() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    recordUserActivity();

    auto ks = M5Cardputer.Keyboard.keysState();

    for (char c : ks.word) {
      if (ks.fn && shouldHandleDebouncedActionChar(c) && handleFnScrollShortcut(c)) {
        continue;
      }
      if (c == '`') {
        if (!shouldHandleDebouncedActionChar(c)) continue;
        openChannelListPage(true);
        continue;
      }
      if (c >= 32 || c == '\t') {
        if (_input.length() < MAX_INPUT_CHARS) _input += c;
      }
    }

    if (shouldHandleDebouncedKey(ks.del, _lastDeleteKeyMs) && !_input.isEmpty()) {
      _input.remove(_input.length() - 1);
    }

    if (shouldHandleDebouncedKey(ks.enter, _lastEnterKeyMs)) {
      submitInput();
    }

    if (shouldHandleDebouncedKey(ks.tab, _lastTabKeyMs)) {
      cycleTab(1);
    }

    _dirty = true;
  }

  void handleChannelListKeyboard() {
    if (!M5Cardputer.Keyboard.isChange()) return;
    if (!M5Cardputer.Keyboard.isPressed()) return;
    recordUserActivity();

    auto ks = M5Cardputer.Keyboard.keysState();

    if (_channelListFilterPrompt) {
      for (char c : ks.word) {
        if (c == '`') {
          if (!shouldHandleDebouncedActionChar(c)) continue;
          closeChannelListPage();
          return;
        }
        if (c >= 32 && c != '\t' && _channelListFilterBuffer.length() < MAX_INPUT_CHARS) {
          _channelListFilterBuffer += c;
        }
      }

      if (shouldHandleDebouncedKey(ks.del, _lastDeleteKeyMs) && !_channelListFilterBuffer.isEmpty()) {
        _channelListFilterBuffer.remove(_channelListFilterBuffer.length() - 1);
      }

      if (shouldHandleDebouncedKey(ks.enter, _lastEnterKeyMs)) {
        applyChannelListFilterAndRequest(true);
      }

      _dirty = true;
      return;
    }

    if (shouldHandleDebouncedKey(ks.enter, _lastEnterKeyMs)) {
      joinSelectedChannelFromList();
      return;
    }

    if (shouldHandleDebouncedKey(ks.tab, _lastTabKeyMs)) moveChannelListSelection(1);
    if (shouldHandleDebouncedKey(ks.del, _lastDeleteKeyMs)) moveChannelListSelection(-1);

    for (char c : ks.word) {
      if (c == '`') {
        if (!shouldHandleDebouncedActionChar(c)) continue;
        closeChannelListPage();
        return;
      }
      if ((c == '.' || c == ';') && !shouldHandleDebouncedActionChar(c)) continue;
      if (c == '.') moveChannelListSelection(1);
      else if (c == ';') moveChannelListSelection(-1);
    }
  }

  void submitInput() {
    String text = _input;
    _input = "";
    text.trim();
    if (text.isEmpty()) return;

    if (text.startsWith("/")) {
      appendLine(statusTab(), "[cmd] " + text);
      handleCommand(text);
      return;
    }

    Tab& tab = _tabs[_activeTab];
    if (tab.type != TabType::Channel && tab.type != TabType::Query) {
      appendLine(statusTab(), "*** No channel/query active. Use /join or /query.");
      return;
    }
    if (!_uiTransportConnected) {
      logStatus("IRC is not connected");
      return;
    }

    sendPrivmsg(tab.name, text);
    appendLine(tab, "<" + _selfNick + "> " + text, currentTimeStampShort(), currentTimeStampLong(), false, true, false);
    tab.scroll = 0;
  }

  void closeActiveTab() {
    if (_activeTab <= 0 || _activeTab >= static_cast<int>(_tabs.size())) return;
    if (_tabs[_activeTab].type == TabType::Channel && _uiTransportConnected) {
      if (!queueRawCommand("PART " + _tabs[_activeTab].name + " :Closing tab")) {
        logCommandQueueFailure("channel part");
      } else {
        appendLine(statusTab(), "--> PART " + _tabs[_activeTab].name + " :Closing tab", currentTimeStampShort(), currentTimeStampLong());
      }
    }
    _tabs.erase(_tabs.begin() + _activeTab);
    if (_activeTab >= static_cast<int>(_tabs.size())) _activeTab = static_cast<int>(_tabs.size()) - 1;
    if (_activeTab < 0) _activeTab = 0;
    _tabs[_activeTab].unread = false;
    _tabs[_activeTab].mention = false;
    _dirty = true;
    if (!syncKnownChannelsToIrc()) logCommandQueueFailure("known channel sync");
    markStateDirty();
  }

  void applyScrollCommand(Tab& tab, String args) {
    args.trim();
    String mode;
    String countStr;
    int sp = args.indexOf(' ');
    if (sp < 0) mode = args;
    else {
      mode = args.substring(0, sp);
      countStr = trimCopy(args.substring(sp + 1));
    }
    mode.toLowerCase();
    int n = countStr.isEmpty() ? 1 : std::max(1, static_cast<int>(countStr.toInt()));

    int page = std::max(1, bodyVisibleRows() - 1);
    if (mode == "up") tab.scroll += n;
    else if (mode == "down") tab.scroll = std::max(0, tab.scroll - n);
    else if (mode == "pageup") tab.scroll += n * page;
    else if (mode == "pagedown") tab.scroll = std::max(0, tab.scroll - (n * page));
    else if (mode == "top") tab.scroll = maxTabScroll(tab);
    else if (mode == "bottom") tab.scroll = 0;
    clampTabScroll(tab);
  }

  bool ensureSojuCommandReady(bool requireCap = true) {
    if (!_ircCfg.bncEnabled || _ircCfg.bncMode != BouncerMode::Soju) {
      emitStatusEvent("Soju mode is not enabled");
      return false;
    }
    if (requireCap && !capEnabled("soju.im/bouncer-networks")) {
      emitStatusEvent("soju.im/bouncer-networks is not active on this connection");
      return false;
    }
    return true;
  }

  void handleSojuCommand(String args) {
    String sub;
    String rest;
    int sp = args.indexOf(' ');
    if (sp < 0) sub = trimCopy(args);
    else {
      sub = trimCopy(args.substring(0, sp));
      rest = trimCopy(args.substring(sp + 1));
    }
    sub.toLowerCase();

    if (sub.isEmpty() || sub == "status") {
      emitStatusEvent("soju mode=" + bouncerModeToString(_ircCfg.bncMode) +
        " cap=" + String(capEnabled("soju.im/bouncer-networks") ? "on" : "off") +
        " notify=" + String(capEnabled("soju.im/bouncer-networks-notify") ? "on" : "off"));
      emitStatusEvent("soju bound=" + (_sojuBoundNetId.isEmpty() ? String("(none)") : _sojuBoundNetId) +
        " config_bind=" + (_ircCfg.sojuBindNetId.isEmpty() ? String("(none)") : _ircCfg.sojuBindNetId));
      return;
    }

    if (sub == "networks" || sub == "list") {
      if (!ensureSojuCommandReady()) return;
      _sojuListRequested = true;
      ircSendRaw("BOUNCER LISTNETWORKS");
      return;
    }

    if (sub == "cached") {
      appendSojuNetworksToStatus();
      return;
    }

    if (sub == "add") {
      if (!ensureSojuCommandReady()) return;
      if (rest.isEmpty()) {
        emitStatusEvent("Usage: /soju add name=...;host=...");
        return;
      }
      ircSendRaw("BOUNCER ADDNETWORK " + rest);
      return;
    }

    if (sub == "change") {
      if (!ensureSojuCommandReady()) return;
      int argSp = rest.indexOf(' ');
      if (argSp <= 0) {
        emitStatusEvent("Usage: /soju change <netid> key=value;...");
        return;
      }
      String netId = trimCopy(rest.substring(0, argSp));
      String attrs = trimCopy(rest.substring(argSp + 1));
      if (attrs.isEmpty()) {
        emitStatusEvent("Usage: /soju change <netid> key=value;...");
        return;
      }
      ircSendRaw("BOUNCER CHANGENETWORK " + netId + " " + attrs);
      return;
    }

    if (sub == "del" || sub == "delete" || sub == "rm") {
      if (!ensureSojuCommandReady()) return;
      if (rest.isEmpty()) {
        emitStatusEvent("Usage: /soju del <netid>");
        return;
      }
      ircSendRaw("BOUNCER DELNETWORK " + rest);
      return;
    }

    if (sub == "bind") {
      if (!ensureSojuCommandReady(false)) return;
      if (rest.isEmpty()) {
        emitStatusEvent("soju bind target = " + (_ircCfg.sojuBindNetId.isEmpty() ? String("(none)") : _ircCfg.sojuBindNetId));
        return;
      }
      if (rest == "off" || rest == "clear" || rest == "none") {
        _ircCfg.sojuBindNetId = "";
        _sojuBindSent = false;
        emitStatusEvent("Cleared soju bind target for next reconnect");
        return;
      }
      _ircCfg.sojuBindNetId = rest;
      _sojuBindSent = false;
      emitStatusEvent("soju bind target set to [" + rest + "] for next reconnect");
      if (_ircRegistered) {
        scheduleReconnect("Reconnecting to bind soju network " + rest);
      } else {
        maybeSendSojuBind();
      }
      return;
    }

    emitStatusEvent("Unknown /soju action: " + sub);
  }

  void handleCommand(const String& cmdLine) {
    String line = cmdLine.substring(1);
    String cmd;
    String args;
    int sp = line.indexOf(' ');
    if (sp < 0) cmd = line;
    else {
      cmd = line.substring(0, sp);
      args = line.substring(sp + 1);
    }
    cmd.toLowerCase();
    args.trim();

    if (cmd == "join") {
      if (!args.isEmpty()) {
        if (!_uiTransportConnected) {
          logStatus("IRC is not connected");
          return;
        }
        String joinTargets = args;
        int keySep = joinTargets.indexOf(' ');
        if (keySep >= 0) joinTargets = joinTargets.substring(0, keySep);
        if (!queueJoinBatch(splitCsv(joinTargets), "manual")) {
          logCommandQueueFailure("channel join");
          return;
        }
        appendLine(statusTab(), "--> JOIN " + joinTargets, currentTimeStampShort(), currentTimeStampLong());
        for (const String& ch : splitCsv(args)) getOrCreateTab(ch, TabType::Channel);
        markStateDirty();
        if (!syncKnownChannelsToIrc()) logCommandQueueFailure("known channel sync");
      }
      return;
    }

    if (cmd == "part") {
      String target = args;
      String reason;
      if (target.isEmpty()) {
        target = _tabs[_activeTab].name;
      } else {
        int p = target.indexOf(' ');
        if (p > 0) {
          reason = trimCopy(target.substring(p + 1));
          target = trimCopy(target.substring(0, p));
        }
      }
      if (!target.isEmpty()) {
        sendRaw("PART " + target + (reason.isEmpty() ? "" : " :" + reason));
      }
      return;
    }

    if (cmd == "detach") {
      String target = args.isEmpty() ? _tabs[_activeTab].name : args;
      if (!target.isEmpty()) {
        sendRaw("PART " + target + " :detach");
      }
      return;
    }

    if (cmd == "nick") {
      if (!args.isEmpty()) sendRaw("NICK " + args);
      return;
    }

    if (cmd == "msg") {
      int s = args.indexOf(' ');
      if (s > 0) {
        if (!_uiTransportConnected) {
          logStatus("IRC is not connected");
          return;
        }
        String target = trimCopy(args.substring(0, s));
        String text = trimCopy(args.substring(s + 1));
        sendPrivmsg(target, text);
        TabType type = isChannelName(target) ? TabType::Channel : TabType::Query;
        Tab& tab = getOrCreateTab(target, type);
        appendLine(tab, "<" + _selfNick + "> " + text, currentTimeStampShort(), currentTimeStampLong(), false, true, false);
        markStateDirty();
      }
      return;
    }

    if (cmd == "notice") {
      int s = args.indexOf(' ');
      if (s > 0) {
        String target = trimCopy(args.substring(0, s));
        String text = trimCopy(args.substring(s + 1));
        sendRaw("NOTICE " + target + " :" + text);
      }
      return;
    }

    if (cmd == "me") {
      Tab& tab = _tabs[_activeTab];
      if ((tab.type == TabType::Channel || tab.type == TabType::Query) && !args.isEmpty()) {
        if (!_uiTransportConnected) {
          logStatus("IRC is not connected");
          return;
        }
        sendRaw("PRIVMSG " + tab.name + " :\001ACTION " + args + "\001");
        appendLine(tab, "* " + _selfNick + " " + args, currentTimeStampShort(), currentTimeStampLong(), false, true, false);
      }
      return;
    }

    if (cmd == "topic") {
      Tab& tab = _tabs[_activeTab];
      if (tab.type == TabType::Channel) {
        if (args.isEmpty()) sendRaw("TOPIC " + tab.name);
        else sendRaw("TOPIC " + tab.name + " :" + args);
      }
      return;
    }

    if (cmd == "whois") {
      if (!args.isEmpty()) sendRaw("WHOIS " + args);
      return;
    }

    if (cmd == "who") {
      sendRaw("WHO " + (args.isEmpty() ? _tabs[_activeTab].name : args));
      return;
    }

    if (cmd == "names") {
      sendRaw("NAMES " + (args.isEmpty() ? _tabs[_activeTab].name : args));
      return;
    }

    if (cmd == "query") {
      if (!args.isEmpty()) {
        Tab& tab = getOrCreateTab(args, TabType::Query);
        _activeTab = static_cast<int>(&tab - &_tabs[0]);
        tab.unread = false;
        tab.mention = false;
        markStateDirty();
      }
      return;
    }

    if (cmd == "close") {
      closeActiveTab();
      return;
    }

    if (cmd == "tabs") {
      String lineOut = "Tabs:";
      for (size_t i = 0; i < _tabs.size(); ++i) {
        lineOut += " [" + String(i) + "]" + _tabs[i].name;
        if (_tabs[i].mention) lineOut += "!";
        else if (_tabs[i].unread) lineOut += "*";
      }
      appendLine(statusTab(), lineOut);
      return;
    }

    if (cmd == "switch") {
      if (isDigitString(args)) {
        int idx = args.toInt();
        if (idx >= 0 && idx < static_cast<int>(_tabs.size())) {
          _activeTab = idx;
          _tabs[_activeTab].unread = false;
          _tabs[_activeTab].mention = false;
          _tabs[_activeTab].scroll = 0;
          markStateDirty();
        }
      } else if (!args.isEmpty()) {
        Tab* tab = findTab(args);
        if (tab) {
          _activeTab = static_cast<int>(tab - &_tabs[0]);
          _tabs[_activeTab].unread = false;
          _tabs[_activeTab].mention = false;
          _tabs[_activeTab].scroll = 0;
          markStateDirty();
        }
      }
      return;
    }

    if (cmd == "next") {
      cycleTab(1);
      return;
    }

    if (cmd == "prev") {
      cycleTab(-1);
      return;
    }

    if (cmd == "scroll") {
      applyScrollCommand(_tabs[_activeTab], args);
      return;
    }

    if (cmd == "users" || cmd == "nicks") {
      Tab& tab = _tabs[_activeTab];
      if (tab.type == TabType::Channel) {
        String users = "Users(" + String(tab.users.size()) + "): ";
        for (size_t i = 0; i < tab.users.size(); ++i) {
          if (i) users += ' ';
          if (tab.users[i].prefix) users += tab.users[i].prefix;
          users += tab.users[i].nick;
        }
        appendLine(tab, users);
      }
      return;
    }

    if (cmd == "nicklist") {
      if (args.isEmpty()) {
        _cfg.nickPaneEnabled = !_cfg.nickPaneEnabled;
      } else {
        _cfg.nickPaneEnabled = strToBool(args);
      }
      appendLine(statusTab(), String("*** Nick pane ") + (_cfg.nickPaneEnabled ? "on" : "off"));
      markStateDirty();
      return;
    }

    if (cmd == "away") {
      if (args.isEmpty()) sendRaw("AWAY");
      else sendRaw("AWAY :" + args);
      return;
    }

    if (cmd == "list") {
      if (args.isEmpty()) openChannelListPage(true);
      else startChannelListSearch(args, true);
      return;
    }

    if (cmd == "colormode") {
      if (!args.isEmpty()) {
        _cfg.colorMode = parseColorMode(args);
        appendLine(statusTab(), "*** Color mode = " + colorModeToString(_cfg.colorMode));
        markStateDirty();
      }
      return;
    }

    if (cmd == "config") {
      openConfigPage();
      appendLine(statusTab(), "*** Config page opened");
      return;
    }

    if (cmd == "soju" || cmd == "bouncer") {
      String sub;
      String rest;
      int argSp = args.indexOf(' ');
      if (argSp < 0) sub = trimCopy(args);
      else {
        sub = trimCopy(args.substring(0, argSp));
        rest = trimCopy(args.substring(argSp + 1));
      }
      sub.toLowerCase();
      if (sub == "bind") {
        if (rest == "off" || rest == "clear" || rest == "none") _cfg.sojuBindNetId = "";
        else if (!rest.isEmpty()) _cfg.sojuBindNetId = rest;
      }
      if (!queueSojuAction(args)) logCommandQueueFailure("soju command");
      return;
    }

    if (cmd == "quote" || cmd == "raw") {
      if (!args.isEmpty()) sendRaw(args);
      return;
    }

    if (cmd == "reconnect") {
      if (!queueReconnectCommand("Manual reconnect")) logCommandQueueFailure("reconnect");
      return;
    }

    if (cmd == "quit") {
      if (!queueQuitCommand()) {
        logCommandQueueFailure("quit");
        return;
      }
      appendLine(statusTab(), "--> QUIT :Bye from Cardputer", currentTimeStampShort(), currentTimeStampLong());
      _uiTransportConnected = false;
      _uiIrcRegistered = false;
      return;
    }

    appendLine(statusTab(), "*** Unknown command: /" + cmd);
  }

  void sendPrivmsg(const String& target, const String& text) {
    sendRaw("PRIVMSG " + target + " :" + text);
  }

  void cycleTab(int delta) {
    if (_tabs.empty()) return;
    _activeTab += delta;
    if (_activeTab < 0) _activeTab = static_cast<int>(_tabs.size()) - 1;
    if (_activeTab >= static_cast<int>(_tabs.size())) _activeTab = 0;
    _tabs[_activeTab].unread = false;
    _tabs[_activeTab].mention = false;
    _tabs[_activeTab].scroll = 0;
    _dirty = true;
    markStateDirty();
  }

  void drawSplash(const String& a, const String& b, const String& c) {
    auto& gfx = drawTarget();
    gfx.fillScreen(UI_BG);
    gfx.setTextColor(UI_FG, UI_BG);
    gfx.setCursor(8, 20);
    gfx.println(String(APP_NAME) + " v" + APP_VERSION);
    gfx.setCursor(8, 45);
    gfx.println(a);
    gfx.setCursor(8, 60);
    gfx.println(b);
    gfx.setCursor(8, 75);
    gfx.println(c);
    presentFrame();
  }

  void draw() {
    if (_screenSleeping || !_dirty) return;
    _dirty = false;
    auto& gfx = drawTarget();
    gfx.fillScreen(UI_BG);
    if (_configOpen) {
      drawConfigPage();
    } else if (_channelListOpen) {
      drawChannelListPage();
    } else {
      drawHeader();
      drawBody();
      drawInput();
    }
    presentFrame();
  }

  void drawConfigPage() {
    auto& gfx = drawTarget();
    gfx.fillRect(0, 0, SCREEN_W, HEADER_H, UI_HEADER);
    gfx.setTextColor(UI_FG, UI_HEADER);
    gfx.setCursor(2, 3);
    gfx.print(_configEditing ? "CONFIG EDIT" : "CONFIG PAGE");
    String rhs = "hold G0";
    gfx.setCursor(SCREEN_W - static_cast<int>(rhs.length()) * CHAR_W - 2, 3);
    gfx.print(rhs);
    drawBatteryIndicator(gfx, UI_HEADER);

    gfx.fillRect(0, BODY_Y, SCREEN_W, BODY_H, UI_BG);

    int visibleRows = bodyVisibleRows();
    if (_configSelected < _configScroll) _configScroll = _configSelected;
    if (_configSelected >= _configScroll + visibleRows) _configScroll = _configSelected - visibleRows + 1;
    if (_configScroll < 0) _configScroll = 0;

    int y = BODY_Y + 1;
    int labelChars = 13;
    int valueChars = 24;

    for (int idx = _configScroll; idx < CFG_COUNT && idx < _configScroll + visibleRows; ++idx) {
      bool selected = idx == _configSelected;
      uint16_t bg = selected ? UI_HILITE_BG : UI_BG;
      gfx.fillRect(0, y, SCREEN_W, CHAR_H + 2, bg);
      gfx.setTextColor(selected ? UI_WARN : UI_DIM, bg);
      gfx.setCursor(2, y);
      gfx.print(selected ? (_configEditing ? "*" : ">") : " " );

      String label = ellipsize(getConfigFieldLabel(idx), labelChars);
      String value = ellipsize(getConfigFieldValue(idx, true), valueChars);

      gfx.setTextColor(UI_FG, bg);
      gfx.setCursor(10, y);
      gfx.print(label);

      if (!configFieldIsAction(idx)) {
        gfx.setTextColor(selected ? UI_ACCENT : UI_FG, bg);
        gfx.setCursor(10 + labelChars * CHAR_W, y);
        gfx.print(value);
      }

      y += ROW_H;
    }

    int inputY = SCREEN_H - INPUT_H;
    gfx.fillRect(0, inputY, SCREEN_W, INPUT_H, UI_INPUT);
    gfx.drawFastHLine(0, inputY, SCREEN_W, UI_DIM);
    gfx.setTextColor(UI_FG, UI_INPUT);

    if (_configEditing) {
      String hdr = ellipsize(getConfigFieldLabel(_configSelected), 16) + ":";
      gfx.setCursor(2, inputY + 3);
      gfx.print(hdr);

      int charsPerRow = (SCREEN_W - 4) / CHAR_W;
      String src = _configEditBuffer;
      int keep = charsPerRow * 2;
      if (static_cast<int>(src.length()) > keep) src = src.substring(src.length() - keep);
      String row1 = src.length() > static_cast<size_t>(charsPerRow) ? src.substring(0, charsPerRow) : src;
      String row2 = src.length() > static_cast<size_t>(charsPerRow) ? src.substring(charsPerRow) : "";
      gfx.setCursor(2, inputY + 10);
      gfx.print(ellipsize(row1, charsPerRow));
      if (!row2.isEmpty()) {
        gfx.setCursor(2, inputY + 18);
      gfx.print(ellipsize(row2, charsPerRow));
      }
    } else {
      gfx.setCursor(2, inputY + 4);
      gfx.print("; up  . down  ENT ok");
      gfx.setCursor(2, inputY + 13);
      gfx.print("TAB/DEL alt  hold G0");
    }
  }

  void drawChannelListPage() {
    auto& gfx = drawTarget();
    gfx.fillRect(0, 0, SCREEN_W, HEADER_H, UI_HEADER);
    gfx.setTextColor(UI_FG, UI_HEADER);
    gfx.setCursor(2, 3);
    if (_channelListFilterPrompt) gfx.print("CHANNEL FILTER");
    else gfx.print(_channelListLoading ? "CHANNELS LOAD" : "CHANNEL LIST");

    String rhs = "` close";
    gfx.setCursor(SCREEN_W - static_cast<int>(rhs.length()) * CHAR_W - 2, 3);
    gfx.print(rhs);
    drawBatteryIndicator(gfx, UI_HEADER);

    gfx.fillRect(0, BODY_Y, SCREEN_W, BODY_H, UI_BG);

    if (_channelListFilterPrompt) {
      gfx.setTextColor(UI_DIM, UI_BG);
      gfx.setCursor(8, BODY_Y + 10);
      gfx.print("Type a search filter");
      gfx.setCursor(8, BODY_Y + 22);
      gfx.print("Enter = full list if blank");
      gfx.setCursor(8, BODY_Y + 34);
      gfx.print("Matching is substring-based");
    } else {
      std::vector<int> visible = filteredChannelListIndices();
      int visibleRows = bodyVisibleRows();
      if (_channelListSelected < _channelListScroll) _channelListScroll = _channelListSelected;
      if (_channelListSelected >= _channelListScroll + visibleRows) {
        _channelListScroll = _channelListSelected - visibleRows + 1;
      }
      if (_channelListScroll < 0) _channelListScroll = 0;

      if (_channelListSelected >= static_cast<int>(visible.size())) {
        _channelListSelected = std::max(0, static_cast<int>(visible.size()) - 1);
      }

      if (_channelList.empty() && _channelListLoading) {
        gfx.setTextColor(UI_DIM, UI_BG);
        gfx.setCursor(8, BODY_Y + 10);
        gfx.print("Loading channel list...");
      } else if (_channelList.empty()) {
        gfx.setTextColor(UI_DIM, UI_BG);
        gfx.setCursor(8, BODY_Y + 10);
        gfx.print("No channels available");
      } else if (visible.empty()) {
        gfx.setTextColor(UI_DIM, UI_BG);
        gfx.setCursor(8, BODY_Y + 10);
        gfx.print("No channels match filter");
      } else {
        int y = BODY_Y + 1;
        for (int idx = _channelListScroll; idx < static_cast<int>(visible.size()) && idx < _channelListScroll + visibleRows; ++idx) {
          const ChannelListEntry& entry = _channelList[visible[idx]];
          bool selected = idx == _channelListSelected;
          uint16_t bg = selected ? UI_HILITE_BG : UI_BG;
          gfx.fillRect(0, y, SCREEN_W, CHAR_H + 2, bg);
          gfx.setTextColor(selected ? UI_WARN : UI_DIM, bg);
          gfx.setCursor(2, y);
          gfx.print(selected ? ">" : " ");

          String row = entry.name + " [" + String(entry.users) + "]";
          gfx.setTextColor(selected ? UI_ACCENT : UI_FG, bg);
          gfx.setCursor(10, y);
          gfx.print(ellipsize(row, 37));
          y += ROW_H;
        }
      }
    }

    int inputY = SCREEN_H - INPUT_H;
    gfx.fillRect(0, inputY, SCREEN_W, INPUT_H, UI_INPUT);
    gfx.drawFastHLine(0, inputY, SCREEN_W, UI_DIM);
    gfx.setTextColor(UI_FG, UI_INPUT);

    if (_channelListFilterPrompt) {
      gfx.setCursor(2, inputY + 4);
      gfx.print("Filter:");

      int charsPerRow = std::max(1, (SCREEN_W - 4) / CHAR_W);
      String src = _channelListFilterBuffer;
      if (static_cast<int>(src.length()) > charsPerRow) src = src.substring(src.length() - charsPerRow);
      gfx.setCursor(2, inputY + 13);
      gfx.print(ellipsize(src, charsPerRow));
      return;
    }

    String info;
    if (_channelList.empty()) {
      info = _channelListLoading ? "Waiting for LIST reply" : "Press ` to close";
    } else {
      std::vector<int> visible = filteredChannelListIndices();
      if (visible.empty()) {
        info = _channelListLoading ? "Filtering incoming channel list" : "No filter matches";
      } else {
        const ChannelListEntry& entry = _channelList[visible[_channelListSelected]];
        info = entry.topic.isEmpty() ? "No topic" : entry.topic;
      }
      if (!_channelListFilter.isEmpty()) info = "filter=" + _channelListFilter + " | " + info;
      if (_channelListTruncated) info = "(truncated) " + info;
    }
    gfx.setCursor(2, inputY + 4);
    gfx.print(ellipsize(info, (SCREEN_W - 4) / CHAR_W));
    gfx.setCursor(2, inputY + 13);
    gfx.print("; up  . down  ENT join");
  }

  void drawHeader() {
    auto& gfx = drawTarget();
    gfx.fillRect(0, 0, SCREEN_W, HEADER_H, UI_HEADER);
    gfx.setTextColor(UI_FG, UI_HEADER);

    String title = _tabs[_activeTab].name;
    if (_tabs[_activeTab].mention) title = "!" + title;
    else if (_tabs[_activeTab].unread) title = "*" + title;

    String net = _uiWifiReady ? (_uiLocalIp.isEmpty() ? String("online") : _uiLocalIp) : "offline";
    if (_uiTransportConnected) net += " IRC";
    if (!_uiIrcRegistered && _uiTransportConnected) net += "*";

    static constexpr int batteryBodyW = 18;
    static constexpr int batteryTipW = 2;
    static constexpr int batteryGapW = 1;
    static constexpr int batteryTotalW = batteryBodyW + batteryTipW + batteryGapW;
    int batteryX = (SCREEN_W - batteryTotalW) / 2;
    int leftChars = std::max(0, (batteryX - 4) / CHAR_W);
    int rightChars = std::max(0, (SCREEN_W - (batteryX + batteryTotalW) - 4) / CHAR_W);
    title = ellipsize(title, leftChars);
    net = ellipsize(net, rightChars);

    gfx.setCursor(2, 3);
    gfx.print(title);

    int rx = SCREEN_W - (net.length() * CHAR_W) - 2;
    int minRx = batteryX + batteryTotalW + 4;
    if (rx < minRx) rx = minRx;
    gfx.setCursor(rx, 3);
    gfx.print(net);
    drawBatteryIndicator(gfx, UI_HEADER);
  }

  void drawBatteryIndicator(lgfx::LovyanGFX& gfx, uint16_t bg) {
    static constexpr int bodyW = 18;
    static constexpr int bodyH = 8;
    static constexpr int tipW = 2;
    static constexpr int tipH = 4;
    static constexpr int gapW = 1;

    int x = (SCREEN_W - (bodyW + tipW + gapW)) / 2;
    int y = 3;

    gfx.fillRect(x - 1, y - 1, bodyW + tipW + gapW + 2, bodyH + 2, bg);
    gfx.drawRect(x, y, bodyW, bodyH, UI_FG);
    gfx.fillRect(x + bodyW + gapW, y + ((bodyH - tipH) / 2), tipW, tipH, UI_FG);
    gfx.fillRect(x + 1, y + 1, bodyW - 2, bodyH - 2, bg);

    if (_batteryLevel < 0) {
      gfx.drawFastHLine(x + 4, y + (bodyH / 2), bodyW - 8, UI_DIM);
      return;
    }

    int clampedLevel = std::max(0, std::min(100, static_cast<int>(_batteryLevel)));
    int innerW = bodyW - 4;
    int fillW = (innerW * clampedLevel + 99) / 100;
    uint16_t fillColor = clampedLevel <= 20 ? UI_WARN : UI_ACCENT;
    if (fillW > 0) {
      gfx.fillRect(x + 2, y + 2, fillW, bodyH - 4, fillColor);
    }

    if (_batteryChargeState == m5::Power_Class::is_charging) {
      gfx.drawLine(x + 8, y + 1, x + 6, y + 4, UI_FG);
      gfx.drawLine(x + 6, y + 4, x + 9, y + 4, UI_FG);
      gfx.drawLine(x + 9, y + 4, x + 7, y + 7, UI_FG);
    }
  }

  int bodyTextWidth() const {
    if (_activeTab < 0 || _activeTab >= static_cast<int>(_tabs.size())) return SCREEN_W - 2;
    return bodyWidthForTab(_tabs[_activeTab]);
  }

  void drawBody() {
    if (useWrappedText()) {
      drawWrappedBody();
      return;
    }
    drawMarqueeBody();
  }

  void drawMarqueeBody() {
    auto& gfx = drawTarget();
    const Tab& tab = _tabs[_activeTab];
    bool showPane = _cfg.nickPaneEnabled && tab.type == TabType::Channel;
    int textWidth = bodyWidthForTab(tab);
    int start = 0;
    int end = 0;
    int maxLines = 0;
    getVisibleBodyRange(tab, start, end, maxLines);

    gfx.fillRect(0, BODY_Y, SCREEN_W, BODY_H, UI_BG);

    int y = BODY_Y + 1;
    for (int i = start; i < end && y < BODY_Y + BODY_H - CHAR_H; ++i) {
      drawMarqueeChatLine(0, y, tab.lines[i], textWidth);
      y += ROW_H;
    }

    if (showPane) drawNickPane(tab);
  }

  void drawWrappedBody() {
    auto& gfx = drawTarget();
    Tab& tab = _tabs[_activeTab];
    bool showPane = _cfg.nickPaneEnabled && tab.type == TabType::Channel;
    int textWidth = bodyWidthForTab(tab);
    clampTabScroll(tab);
    int visibleRows = bodyVisibleRows();
    int totalRows = totalWrappedRows(tab, textWidth);
    int startRow = std::max(0, totalRows - visibleRows - tab.scroll);

    gfx.fillRect(0, BODY_Y, SCREEN_W, BODY_H, UI_BG);

    int y = BODY_Y + 1;
    int drawnRows = 0;
    int currentRow = 0;
    for (const ChatLine& line : tab.lines) {
      int lineRows = wrappedRowsForLine(line, textWidth);
      if (currentRow + lineRows <= startRow) {
        currentRow += lineRows;
        continue;
      }
      int skipRows = std::max(0, startRow - currentRow);
      int rowsLeft = visibleRows - drawnRows;
      if (rowsLeft <= 0) break;
      int usedRows = drawWrappedChatLine(0, y, line, textWidth, skipRows, rowsLeft);
      drawnRows += usedRows;
      y += usedRows * ROW_H;
      currentRow += lineRows;
      if (drawnRows >= visibleRows || y >= BODY_Y + BODY_H - CHAR_H) break;
    }

    if (showPane) drawNickPane(tab);
  }

  void drawMarqueeChatLine(int x, int y, const ChatLine& line, int maxWidth) {
    auto& gfx = drawTarget();
    uint16_t lineBg = line.highlight ? UI_HILITE_BG : UI_BG;
    gfx.fillRect(x, y, maxWidth, CHAR_H + 1, lineBg);
    if (line.highlight) {
      gfx.fillRect(x, y, 2, CHAR_H + 1, UI_WARN);
    }

    int stampX = x + 2;
    gfx.setTextColor(UI_DIM, lineBg);
    gfx.setCursor(stampX, y);
    String stamp = line.stampShort;
    if (stamp.length() > 5) stamp = stamp.substring(0, 5);
    gfx.print(stamp);

    int textX = x + 2 + TIMESTAMP_W_CHARS * CHAR_W;
    int textW = maxWidth - (TIMESTAMP_W_CHARS * CHAR_W) - 4;
    drawStyledText(textX, y, line.raw, textW, lineBg, currentTextScrollOffsetCols(line, textW));
  }

  int drawWrappedChatLine(int x, int y, const ChatLine& line, int maxWidth, int skipRows, int maxRows) {
    auto& gfx = drawTarget();
    uint16_t lineBg = line.highlight ? UI_HILITE_BG : UI_BG;
    int totalRows = wrappedRowsForLine(line, maxWidth);
    int rowsToDraw = std::max(0, std::min(maxRows, totalRows - skipRows));
    for (int row = 0; row < rowsToDraw; ++row) {
      int rowY = y + row * ROW_H;
      gfx.fillRect(x, rowY, maxWidth, CHAR_H + 1, lineBg);
    }
    if (line.highlight && skipRows == 0 && rowsToDraw > 0) {
      gfx.fillRect(x, y, 2, CHAR_H + 1, UI_WARN);
    }

    if (skipRows == 0 && rowsToDraw > 0) {
      int stampX = x + 2;
      gfx.setTextColor(UI_DIM, lineBg);
      gfx.setCursor(stampX, y);
      String stamp = line.stampShort;
      if (stamp.length() > 5) stamp = stamp.substring(0, 5);
      gfx.print(stamp);
    }

    int textX = x + 2 + TIMESTAMP_W_CHARS * CHAR_W;
    int textW = maxWidth - (TIMESTAMP_W_CHARS * CHAR_W) - 4;
    drawWrappedStyledText(textX, y, line.raw, textW, lineBg, skipRows, rowsToDraw);
    return rowsToDraw;
  }

  void drawNickPane(const Tab& tab) {
    auto& gfx = drawTarget();
    int x = SCREEN_W - NICK_PANE_W;
    gfx.fillRect(x, BODY_Y, NICK_PANE_W, BODY_H, UI_PANE);
    gfx.drawFastVLine(x, BODY_Y, BODY_H, UI_DIM);
    gfx.setTextColor(UI_FG, UI_PANE);
    gfx.setCursor(x + 3, BODY_Y + 2);
    String hdr = "Users " + String(tab.users.size());
    if (hdr.length() > 11) hdr = hdr.substring(0, 11);
    gfx.print(hdr);

    int y = BODY_Y + 12;
    int maxRows = (BODY_H - 14) / (CHAR_H + 1);
    for (int i = 0; i < maxRows && i < static_cast<int>(tab.users.size()); ++i) {
      String row;
      if (tab.users[i].prefix) row += tab.users[i].prefix;
      row += tab.users[i].nick;
      int maxChars = (NICK_PANE_W - 6) / CHAR_W;
      if (row.length() > static_cast<size_t>(maxChars)) row = row.substring(0, maxChars);
      uint16_t color = equalsIgnoreCase(tab.users[i].nick, _selfNick) ? UI_ACCENT : UI_FG;
      gfx.setTextColor(color, UI_PANE);
      gfx.setCursor(x + 3, y);
      gfx.print(row);
      y += CHAR_H + 1;
    }
  }

  std::vector<String> buildInputRows(int maxCharsPerRow, int maxRows) const {
    std::vector<String> rows;
    String src = "> " + _input;
    if (src.isEmpty()) src = ">";
    int totalKeep = maxCharsPerRow * maxRows;
    if (static_cast<int>(src.length()) > totalKeep) {
      src = src.substring(src.length() - totalKeep);
    }
    while (!src.isEmpty()) {
      int take = std::min(maxCharsPerRow, static_cast<int>(src.length()));
      rows.push_back(src.substring(0, take));
      src.remove(0, take);
    }
    if (rows.empty()) rows.push_back(">");
    while (static_cast<int>(rows.size()) < maxRows) rows.insert(rows.begin(), "");
    if (static_cast<int>(rows.size()) > maxRows) rows.erase(rows.begin(), rows.begin() + (rows.size() - maxRows));
    return rows;
  }

  void drawInput() {
    auto& gfx = drawTarget();
    int y = SCREEN_H - INPUT_H;
    gfx.fillRect(0, y, SCREEN_W, INPUT_H, UI_INPUT);
    gfx.drawFastHLine(0, y, SCREEN_W, UI_DIM);
    gfx.setTextColor(UI_FG, UI_INPUT);

    int charsPerRow = (SCREEN_W - 4) / CHAR_W;
    std::vector<String> rows = buildInputRows(charsPerRow, 2);
    gfx.setCursor(2, y + 4);
    gfx.print(rows[0]);
    gfx.setCursor(2, y + 13);
    gfx.print(rows[1]);
  }

  void drawStyledText(int x, int y, const String& raw, int maxWidth, uint16_t baseBg, int skipCols = 0) {
    auto& gfx = drawTarget();
    TextStyle st;
    st.fg = UI_FG;
    st.bg = baseBg;
    int cx = x;
    int skippedCols = 0;

    auto emitChar = [&](char out) {
      if (skippedCols < skipCols) {
        ++skippedCols;
        return;
      }
      if (cx + CHAR_W > x + maxWidth) return;
      uint16_t fg = st.reverse ? st.bg : st.fg;
      uint16_t bg = st.reverse ? st.fg : st.bg;
      gfx.fillRect(cx, y, CHAR_W, CHAR_H + 1, bg);
      gfx.setTextColor(fg, bg);
      gfx.setCursor(cx, y);
      gfx.print(out);
      if (st.underline) {
        gfx.drawFastHLine(cx, y + CHAR_H, CHAR_W, fg);
      }
      if (st.bold && cx + 1 < x + maxWidth) {
        gfx.setCursor(cx + 1, y);
        gfx.print(out);
      }
      cx += CHAR_W;
    };

    for (size_t i = 0; i < raw.length(); ++i) {
      char c = raw[i];
      switch (c) {
        case 0x02:
          st.bold = !st.bold;
          break;
        case 0x03: {
          int j = i + 1;
          String a, b;
          while (j < raw.length() && a.length() < 2 && isdigit(static_cast<unsigned char>(raw[j]))) a += raw[j++];
          if (j < raw.length() && raw[j] == ',') {
            ++j;
            while (j < raw.length() && b.length() < 2 && isdigit(static_cast<unsigned char>(raw[j]))) b += raw[j++];
          }
          if (_cfg.colorMode == ColorMode::Full || _cfg.colorMode == ColorMode::Safe) {
            if (a.isEmpty()) {
              st.fg = UI_FG;
              st.bg = baseBg;
            } else {
              st.fg = ircColorTo565(a.toInt());
              if (_cfg.colorMode == ColorMode::Full && !b.isEmpty()) st.bg = ircColorTo565(b.toInt());
              if (_cfg.colorMode == ColorMode::Safe) st.bg = baseBg;
            }
          } else {
            st.fg = UI_FG;
            st.bg = baseBg;
          }
          i = j - 1;
          break;
        }
        case 0x0F:
          st = TextStyle();
          st.fg = UI_FG;
          st.bg = baseBg;
          break;
        case 0x16:
          st.reverse = !st.reverse;
          break;
        case 0x1D:
          break;
        case 0x1F:
          st.underline = !st.underline;
          break;
        default:
          emitChar(c);
          break;
      }
    }
  }

  void drawWrappedStyledText(int x, int y, const String& raw, int maxWidth, uint16_t baseBg, int skipRows = 0, int maxRows = 1) {
    auto& gfx = drawTarget();
    TextStyle st;
    st.fg = UI_FG;
    st.bg = baseBg;
    int cx = x;
    int row = 0;
    int charsPerRow = std::max(1, maxWidth / CHAR_W);
    int col = 0;

    auto advanceRow = [&]() {
      ++row;
      cx = x;
      col = 0;
    };

    auto emitChar = [&](char out) {
      if (col >= charsPerRow) advanceRow();
      if (row >= skipRows && row < skipRows + maxRows) {
        int drawY = y + (row - skipRows) * ROW_H;
        uint16_t fg = st.reverse ? st.bg : st.fg;
        uint16_t bg = st.reverse ? st.fg : st.bg;
        gfx.fillRect(cx, drawY, CHAR_W, CHAR_H + 1, bg);
        gfx.setTextColor(fg, bg);
        gfx.setCursor(cx, drawY);
        gfx.print(out);
        if (st.underline) {
          gfx.drawFastHLine(cx, drawY + CHAR_H, CHAR_W, fg);
        }
        if (st.bold && cx + 1 < x + maxWidth) {
          gfx.setCursor(cx + 1, drawY);
          gfx.print(out);
        }
      }
      cx += CHAR_W;
      ++col;
    };

    for (size_t i = 0; i < raw.length(); ++i) {
      char c = raw[i];
      switch (c) {
        case 0x02:
          st.bold = !st.bold;
          break;
        case 0x03: {
          int j = static_cast<int>(i) + 1;
          String a, b;
          while (j < static_cast<int>(raw.length()) && a.length() < 2 && isdigit(static_cast<unsigned char>(raw[j]))) {
            a += raw[j++];
          }
          if (j < static_cast<int>(raw.length()) && raw[j] == ',') {
            ++j;
            while (j < static_cast<int>(raw.length()) && b.length() < 2 && isdigit(static_cast<unsigned char>(raw[j]))) {
              b += raw[j++];
            }
          }
          if (_cfg.colorMode == ColorMode::Full || _cfg.colorMode == ColorMode::Safe) {
            if (a.isEmpty()) {
              st.fg = UI_FG;
              st.bg = baseBg;
            } else {
              st.fg = ircColorTo565(a.toInt());
              if (_cfg.colorMode == ColorMode::Full && !b.isEmpty()) st.bg = ircColorTo565(b.toInt());
              if (_cfg.colorMode == ColorMode::Safe) st.bg = baseBg;
            }
          } else {
            st.fg = UI_FG;
            st.bg = baseBg;
          }
          i = j - 1;
          break;
        }
        case 0x0F:
          st = TextStyle();
          st.fg = UI_FG;
          st.bg = baseBg;
          break;
        case 0x16:
          st.reverse = !st.reverse;
          break;
        case 0x1D:
          break;
        case 0x1F:
          st.underline = !st.underline;
          break;
        case '\n':
          advanceRow();
          break;
        default:
          emitChar(c);
          break;
      }
      if (row >= skipRows + maxRows) break;
    }
  }
};

IrcClientApp app;

void setup() {
  app.begin();
}

void loop() {
  app.loop();
  delay(5);
}
