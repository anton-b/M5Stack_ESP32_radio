#ifndef HAS_BLUETOOTH
#define HAS_BLUETOOTH 0
#endif

// =============================================================================
//  M5Stack Basic / Core — интернет-радио с управлением на 3 кнопках
// =============================================================================
//  A / C      — станция назад / вперёд
//  B          — играть / стоп
//  удерж. B   — меню
//  удерж. A/C — громкость
//
//  В меню: выбор Wi-Fi сети и ввод пароля экранной клавиатурой.
//  Настройки хранятся в NVS (Preferences).
//
//  Звук: ESP8266Audio (MP3) -> AudioOutputM5Speaker -> встроенный динамик.
// =============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cstdlib>

#include "stations.h"
#include "AudioOutputM5Speaker.h"
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#if HAS_BLUETOOTH
#include "AudioOutputBT.h"
#include <BluetoothA2DPSource.h>
#endif
#include "fonts/font8.h"
#include "fonts/font11.h"
#include "fonts/font16b.h"
#include "fonts/font22b.h"

// -----------------------------------------------------------------------------
//  Палитра и шрифты
// -----------------------------------------------------------------------------
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static const uint16_t C_BG     = rgb(8, 11, 22);
static const uint16_t C_PANEL  = rgb(20, 27, 48);
static const uint16_t C_PANEL2 = rgb(32, 42, 72);
static const uint16_t C_ACCENT = rgb(0, 225, 255);
static const uint16_t C_PINK   = rgb(255, 66, 170);
static const uint16_t C_TEXT   = rgb(236, 242, 255);
static const uint16_t C_DIM    = rgb(118, 133, 170);
static const uint16_t C_LINE   = rgb(38, 49, 80);
static const uint16_t C_GREEN  = rgb(64, 255, 150);
static const uint16_t C_AMBER  = rgb(255, 190, 60);
static const uint16_t C_RED    = rgb(255, 72, 96);

static const lgfx::IFont* F_SM  = &Rf8;
static const lgfx::IFont* F_MD  = &Rf11;
static const lgfx::IFont* F_LG  = &Rf16b;
static const lgfx::IFont* F_XL  = &Rf22b;

static lgfx::LGFXBase* L = &M5.Display;

// -----------------------------------------------------------------------------
//  Состояние приложения
// -----------------------------------------------------------------------------
enum class Screen : uint8_t {
  Splash, Player, Stations, Menu, WifiScan, Password, WifiConnecting, Info, Output, Bluetooth
};

static Screen  screen         = Screen::Splash;
static bool    needFullDraw   = true;
static uint32_t splashStart   = 0;
static uint32_t lastDyn       = 0;
static uint32_t lastSecond    = 0;

// Логи можно временно выключить (на время бинарного дампа экрана).
static volatile bool logQuiet = false;
#define LOGF(...) do { if (!logQuiet) Serial.printf(__VA_ARGS__); } while (0)
#define LOGL(s)   do { if (!logQuiet) Serial.println(s); } while (0)

// Синтетические нажатия из COM-порта (для отладки без физических кнопок).
static volatile uint8_t synthClick[3] = {0, 0, 0};
static volatile uint8_t synthHold[3]  = {0, 0, 0};

static Preferences prefs;
static String  wifiSsid, wifiPass;
static int     volume         = 180;      // 0..255
static int     outMode        = 0;       // 0 встроенный, 1 Bluetooth, 2 I2S
static int     stationSel     = 0;
static int     listSel        = 0;
static bool    wantPlaying    = false;

static uint32_t volRepeat[2]  = {0, 0};

// Wi-Fi
static const int MAX_NET = 30;
struct NetInfo { char ssid[33]; int rssi; bool open; };
static NetInfo nets[MAX_NET];
static int     netCount   = 0;
static int     netSel     = 0;
static bool    scanning   = false;
static bool    scanError  = false;
static uint32_t scanStart = 0;

// Подключение
static String  connSsid, connPass;
static uint32_t connStart = 0;
static int8_t  connResult = 0;   // 0 ждём, 1 успех, -1 ошибка
static bool    connTried  = false;

// Экранная клавиатура
static String  passSsid, passInput;
static int     kbPage = 0;        // 0 строчные, 1 заглавные, 2 символы
static int     kbRow  = 0;
static int     kbCol  = 0;
static uint32_t lastCharAt = 0;

static const char* KB_LAYOUT[3][4] = {
  {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"},
  {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM,?!"},
  {"!@#$%^&*()", "-+=[]{};:'", "\"/\\|<>?,.~", "_`"},
};

// Меню
static int menuSel = 0;
static const char* MENU_ITEMS[] = {"Станции", "Выход звука", "Wi-Fi и пароль", "Информация", "Назад"};
static const int MENU_COUNT = 5;
static int outSel = 0;
static const char* OUT_ITEMS[] = {"Динамик", "Bluetooth", "I2S DAC", "Назад"};
static const int OUT_COUNT = 4;

// -----------------------------------------------------------------------------
//  Аудио: декодирование в отдельной задаче
// -----------------------------------------------------------------------------
namespace radio {

enum State : uint8_t { Stopped = 0, Buffering = 1, Playing = 2, Failed = 3 };

// 16 КБ буфера потока — экономит 16 КБ RAM для Bluetooth A2DP
static const size_t STREAM_BUF = 16 * 1024;
static const size_t CODEC_BUF  = 29192;       // максимум памяти MP3-декодера

static AudioOutputM5Speaker outM5(&M5.Speaker, 0);
static AudioOutputI2S        outI2S;
#if HAS_BLUETOOTH
static AudioOutputBT         outBT;
#endif
static AudioOutput*          outp = &outM5;
static int                   _mode = 0;   // 0 встроенный, 1 Bluetooth, 2 I2S

static AudioGeneratorMP3*        decoder = nullptr;
static AudioFileSourceICYStream* file    = nullptr;
static AudioFileSourceBuffer*    buff    = nullptr;

static uint8_t* streamBuf = nullptr;
static uint8_t codecBuf[CODEC_BUF];

static std::atomic<uint8_t>  _state{Stopped};
static std::atomic<int>      _playing{-1};
static std::atomic<int>      _request{-1};
static std::atomic<bool>     _stopReq{false};
static std::atomic<uint32_t> _fill{0};       // 0..1000
static std::atomic<int>      _level{0};      // 0..100

static char titleBuf[192] = "";
static portMUX_TYPE titleMux = portMUX_INITIALIZER_UNLOCKED;

// ----- UTF-8 / CP1251 --------------------------------------------------------
static const uint16_t CP1251_HI[64] = {
  0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
  0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
  0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
  0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,0x00A0,
  0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,
  0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,0x00B0,
  0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,
  0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,0x00BF
};

static bool isValidUtf8(const char* s) {
  while (*s) {
    unsigned char c = (unsigned char)*s++;
    if (c < 0x80) continue;
    int n = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
    if (n < 0) return false;
    while (n--) {
      if (((unsigned char)*s & 0xC0) != 0x80) return false;
      s++;
    }
  }
  return true;
}

static void putUtf8(String& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// Метаданные приходят то в UTF-8, то в CP1251 — приводим к UTF-8.
static void decodeMeta(const char* in, char* out, size_t outSize) {
  String s;
  if (isValidUtf8(in)) {
    s = in;
  } else {
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
      uint32_t cp;
      if (*p < 0x80) cp = *p;
      else if (*p < 0xC0) cp = CP1251_HI[*p - 0x80];
      else cp = 0x410 + (*p - 0xC0);
      putUtf8(s, cp);
    }
  }
  s.trim();
  strncpy(out, s.c_str(), outSize - 1);
  out[outSize - 1] = 0;
}

// ----- колбэки ---------------------------------------------------------------
static void mdCallback(void*, const char* type, bool, const char* str) {
  if (strcmp(type, "StreamTitle") != 0 || !str) return;
  char tmp[192];
  decodeMeta(str, tmp, sizeof(tmp));
  portENTER_CRITICAL(&titleMux);
  strncpy(titleBuf, tmp, sizeof(titleBuf) - 1);
  titleBuf[sizeof(titleBuf) - 1] = 0;
  portEXIT_CRITICAL(&titleMux);
  LOGF("[radio] title: %s\n", tmp);
}

static void monitor(void*, const int16_t* samples, size_t frames) {
  if (!samples || !frames) { _level.store(_level.load() * 3 / 4); return; }
  int peak = 0;
  const size_t n = frames * 2;
  for (size_t i = 0; i < n; ++i) {
    int v = samples[i];
    if (v < 0) v = -v;
    if (v > peak) peak = v;
  }
  int lv = peak * 100 / 32768;
  int cur = _level.load();
  if (lv > cur) _level.store(lv);
  else _level.store(cur - (cur - lv) / 5 - 1);
  if (_level.load() < 0) _level.store(0);
}

static void setState(State s) { _state.store((uint8_t)s); }

static void clearTitle() {
  portENTER_CRITICAL(&titleMux);
  titleBuf[0] = 0;
  portEXIT_CRITICAL(&titleMux);
}

static void stopAll() {
  if (decoder) { decoder->stop(); delete decoder; decoder = nullptr; }
  if (buff)    { buff->close();    delete buff;    buff = nullptr; }
  if (file)    { file->close();    delete file;    file = nullptr; }
  outp->stop();
  _fill.store(0);
  _level.store(0);
}

static bool openStation(int idx) {
  stopAll();
  clearTitle();
  _playing.store(idx);
  setState(Buffering);
  LOGF("[radio] opening %d: %s\n", idx, STATIONS[idx].url);

  file = new AudioFileSourceICYStream(STATIONS[idx].url);
  if (!file->isOpen()) {
    LOGL("[radio] connect failed");
    delete file; file = nullptr;
    setState(Failed);
    return false;
  }
  file->RegisterMetadataCB(mdCallback, nullptr);
  buff = new AudioFileSourceBuffer(file, streamBuf, STREAM_BUF);
  uint8_t dummy;
  buff->read(&dummy, 0);          // запускает первичное наполнение буфера
  return true;
}

static void task(void*) {
  uint32_t bufStart = 0;
  bool     starting = false;

  for (;;) {
    M5.delay(1);

    int req = _request.exchange(-1);
    if (req >= 0 && req < STATION_COUNT) {
      starting = openStation(req);
      bufStart = millis();
    }

    if (_stopReq.exchange(false)) {
      stopAll();
      setState(Stopped);
      starting = false;
    }

    State st = (State)_state.load();

    if (st == Buffering && buff) {
      buff->loop();
      uint32_t fill = buff->getFillLevel();
      _fill.store((uint32_t)((uint64_t)fill * 1000 / STREAM_BUF));

      uint32_t waited = millis() - bufStart;
      if (fill >= STREAM_BUF * 3 / 4 || (waited > 6000 && fill > 2048)) {
        decoder = new AudioGeneratorMP3(codecBuf, sizeof(codecBuf));
        if (decoder && decoder->begin(buff, outp)) {
          setState(Playing);
          LOGL("[radio] playing");
        } else {
          LOGL("[radio] decoder failed");
          stopAll();
          setState(Failed);
        }
      } else if (waited > 15000) {
        LOGL("[radio] buffer timeout");
        stopAll();
        setState(Failed);
      }
    }
    else if (st == Playing && decoder && buff) {
      buff->loop();
      _fill.store((uint32_t)((uint64_t)buff->getFillLevel() * 1000 / STREAM_BUF));

      // если буфер опустел — даём сети шанс догнать поток
      if (buff->getFillLevel() < 4096) {
        uint32_t t0 = millis();
        while (buff->getFillLevel() < STREAM_BUF / 2 && millis() - t0 < 3000) {
          buff->loop();
          M5.delay(5);
          if (_request.load() >= 0 || _stopReq.load()) break;
        }
      }
      if (!decoder->loop()) {
        LOGL("[radio] stream ended");
        buff->loop();
        if (buff->getFillLevel() == 0) {
          stopAll();
          setState(Failed);
        }
      }
    }
    else {
      M5.delay(20);
    }
  }
}

void begin() {
  if (!streamBuf) {
    size_t sz = STREAM_BUF;
#if HAS_BLUETOOTH
    if (ESP.getPsramSize() > 0) sz = 128 * 1024;
#endif
    streamBuf = (uint8_t*)malloc(sz);
    if (!streamBuf) { LOGL("[radio] no memory for stream buffer"); }
  }
  outM5.setup();
  outM5.setMonitor(monitor, nullptr);
  xTaskCreatePinnedToCore(task, "radio", 8192, nullptr, 1, nullptr, 0);
}

// Режимы вывода: 0 встроенный динамик, 1 Bluetooth, 2 внешний I2S DAC.
void setMode(int mode, int vol) {
  stopAll();
#if HAS_BLUETOOTH
  if (mode == 1) {
    M5.Speaker.end();
    outBT.setVolume(vol);
    outp = &outBT;
    _mode = 1;
    LOGF("[radio] output mode Bluetooth\n");
    return;
  }
#endif
  if (mode == 2) {
    M5.Speaker.end();
    outI2S.SetPinout(12, 0, 2);   // BCK, LRC, DIN (как у M5Stack Module Audio)
    outI2S.SetOutputModeMono(true);
    outI2S.SetGain(vol / 255.0f);
    outp = &outI2S;
    _mode = 2;
  } else {
    outI2S.stop();
    M5.Speaker.begin();
    M5.Speaker.setVolume(vol);
    outp = &outM5;
    _mode = 0;
  }
#if HAS_BLUETOOTH
  // Если возвращаемся из Bluetooth на плате без PSRAM, восстанавливаем Wi-Fi
  if (ESP.getPsramSize() == 0 && mode != 1) {
    if (WiFi.getMode() == WIFI_OFF && wifiSsid.length()) {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(true);
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
  }
#endif
  LOGF("[radio] output mode %d\n", _mode);
}

int mode() { return _mode; }

#if HAS_BLUETOOTH
void btSetVolume(int vol);
#endif

void setVolume(int vol) {
#if HAS_BLUETOOTH
  if (outp == &outBT) {
    outBT.setVolume(vol);
    btSetVolume(vol);
  } else
#endif
  if (outp == &outI2S) outI2S.SetGain(vol / 255.0f);
  else M5.Speaker.setVolume(vol);
}

void play(int idx) {
  if (idx < 0 || idx >= STATION_COUNT) return;
  setState(Buffering);
  _fill.store(0);
  _request.store(idx);
}

void stop() {
  _stopReq.store(true);
  setState(Stopped);
}

void next(int dir) {
  int idx = _playing.load();
  if (idx < 0) idx = 0;
  idx = (idx + dir + STATION_COUNT) % STATION_COUNT;
  play(idx);
}

State  state()  { return (State)_state.load(); }
int    playing(){ return _playing.load(); }
uint32_t fill() { return _fill.load(); }
int    level()  { return _level.load(); }

void getTitle(char* out, size_t n) {
  portENTER_CRITICAL(&titleMux);
  strncpy(out, titleBuf, n - 1);
  out[n - 1] = 0;
  portEXIT_CRITICAL(&titleMux);
}

} // namespace radio

// -----------------------------------------------------------------------------
//  Настройки
// -----------------------------------------------------------------------------
#if HAS_BLUETOOTH
static BluetoothA2DPSource a2dp;

namespace radio {
void btSetVolume(int vol) {
  if (a2dp.is_connected()) a2dp.set_volume(vol * 127 / 255);
}
}

static bool     btInited    = false;
static int      btState     = 0;   // 0 выкл, 1 поиск, 2 подключение, 3 подключено, 4 ошибка
static uint32_t btConnStart = 0;

struct BtDev { char name[32]; esp_bd_addr_t addr; int rssi; };
static const int BT_MAX = 12;
static BtDev btDevs[BT_MAX];
static int   btDevCount = 0;
static int   btDevSel   = 0;

static char btMac[18]  = "";
static char btName[32] = "";

static volatile float btSineFreq = 440.0f;

static int32_t btGetData(uint8_t* data, int32_t len) {
  if (ESP.getPsramSize() > 0) {
    // На платах с PSRAM (Core2, Fire) стримим интернет-радио
    return radio::outBT.read(data, len);
  } else {
    // На M5Stack Basic (без PSRAM) генерируем стерео синусоиду
    int16_t* samples = (int16_t*)data;
    size_t count = len / 4;
    static float phase = 0.0f;
    float step = 2.0f * 3.14159265f * btSineFreq / 44100.0f;
    float amp = (volume / 255.0f) * 14000.0f;
    for (size_t i = 0; i < count; ++i) {
      int16_t v = (int16_t)(sinf(phase) * amp);
      samples[i * 2]     = v;
      samples[i * 2 + 1] = v;
      phase += step;
      if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
    }
    return len;
  }
}

static bool btSsidCb(const char* ssid, esp_bd_addr_t addr, int rssi) {
  if (!ssid || !ssid[0]) return false;
  for (int i = 0; i < btDevCount; ++i) {
    if (!memcmp(btDevs[i].addr, addr, 6)) {
      if (rssi > btDevs[i].rssi) btDevs[i].rssi = rssi;
      return false;
    }
  }
  if (btDevCount >= BT_MAX) return false;
  BtDev& d = btDevs[btDevCount++];
  strncpy(d.name, ssid, sizeof(d.name) - 1);
  d.name[sizeof(d.name) - 1] = 0;
  memcpy(d.addr, addr, 6);
  d.rssi = rssi;
  LOGF("[bt] found %s rssi=%d\n", d.name, rssi);
  return false;
}

static void btDiscMode(esp_bt_gap_discovery_state_t m) {
  if (m == ESP_BT_GAP_DISCOVERY_STOPPED && btState == 1) {
    btState = 0;
    needFullDraw = true;
  }
}

static void btInit() {
  if (btInited) return;
  WiFi.setSleep(true); // необходимо для coex
  a2dp.set_local_name("M5 Radio");
  a2dp.set_data_callback(btGetData);
  a2dp.set_ssp_enabled(true);
  a2dp.set_ssid_callback(btSsidCb);
  a2dp.set_discovery_mode_callback(btDiscMode);
  a2dp.start();
  btInited = true;
  btState = 1;
  LOGF("[bt] started, heap=%u psram=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
}

static void btStartScan() {
  radio::stop();
  // Выключаем Wi-Fi, чтобы освободить ~80 КБ heap для Bluedroid на платах без PSRAM!
  if (ESP.getPsramSize() == 0) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);
  }
  btInit();
  btDevCount = 0;
  btDevSel = 0;
  btState = 1;
  esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  LOGF("[bt] scan start\n");
}

static void btMacToAddr(const char* s, esp_bd_addr_t a) {
  unsigned v[6];
  sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  for (int i = 0; i < 6; ++i) a[i] = (uint8_t)v[i];
}

static void btConnectSaved() {
  if (!btMac[0]) return;
  btInit();
  esp_bd_addr_t addr;
  btMacToAddr(btMac, addr);
  a2dp.cancel_discovery();
  a2dp.connect_to(addr);
  btConnStart = millis();
  btState = 2;
  LOGF("[bt] connect saved %s\n", btMac);
}

static void btConnectIndex(int i) {
  if (i < 0 || i >= btDevCount) return;
  a2dp.cancel_discovery();
  a2dp.connect_to(btDevs[i].addr);
  snprintf(btMac, sizeof(btMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           btDevs[i].addr[0], btDevs[i].addr[1], btDevs[i].addr[2],
           btDevs[i].addr[3], btDevs[i].addr[4], btDevs[i].addr[5]);
  strncpy(btName, btDevs[i].name, sizeof(btName) - 1);
  btName[sizeof(btName) - 1] = 0;
  prefs.putString("btmac", btMac);
  prefs.putString("btname", btName);
  btConnStart = millis();
  btState = 2;
  LOGF("[bt] connecting to %s\n", btMac);
}

static void btPoll() {
  if (!btInited) return;
  static bool wasConnected = false;
  bool connected = a2dp.is_connected();
  if (connected && !wasConnected) {
    btState = 3;
    needFullDraw = true;
    LOGF("[bt] connected: %s\n", btName);
  }
  if (!connected && wasConnected) {
    btState = 0;
    needFullDraw = true;
    LOGF("[bt] disconnected\n");
  }
  wasConnected = connected;
  if (btState == 2 && millis() - btConnStart > 20000) {
    btState = 4;
    needFullDraw = true;
    LOGF("[bt] connect timeout\n");
  }
}
#endif

static void loadPrefs() {
  prefs.begin("m5radio", false);
  wifiSsid   = prefs.getString("ssid", "");
  wifiPass   = prefs.getString("pass", "");
  volume     = prefs.getInt("vol", 180);
  outMode    = prefs.getInt("out", 0);
  stationSel = prefs.getInt("st", 0);
  if (stationSel < 0 || stationSel >= STATION_COUNT) stationSel = 0;
  if (volume < 0) volume = 0;
  if (volume > 255) volume = 255;
}

static void saveWifi() {
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
}

static void saveStation() { prefs.putInt("st", stationSel); }
static void saveVolume()  { prefs.putInt("vol", volume); }

static void applyVolume() { radio::setVolume(volume); }

// -----------------------------------------------------------------------------
//  Мелкие помощники рисования
// -----------------------------------------------------------------------------
static int pctVolume() { return volume * 100 / 255; }

static void text(const char* s, int x, int y, uint16_t col,
                 const lgfx::IFont* f = nullptr,
                 textdatum_t datum = textdatum_t::top_left) {
  if (f) L->setFont(f);
  L->setTextColor(col);
  L->setTextDatum(datum);
  L->drawString(s, x, y);
  L->setTextDatum(textdatum_t::top_left);
}

static void drawWifiBars(int x, int y, int rssi, uint16_t col) {
  // 4 полоски, растущие вверх; y — низ
  for (int i = 0; i < 4; ++i) {
    int h = 4 + i * 3;
    bool on = rssi > (-84 + i * 8);
    L->fillRect(x + i * 5, y - h, 3, h, on ? col : C_LINE);
  }
}

static void drawLock(int x, int y, uint16_t col) {
  L->drawRoundRect(x, y + 6, 11, 8, 2, col);
  L->drawArc(x + 5, y + 6, 2, 4, 180, 360, col);
}

static void drawPlayIcon(int x, int y, uint16_t col) {
  L->fillTriangle(x, y, x, y + 11, x + 10, y + 5, col);
}

static void drawStopIcon(int x, int y, uint16_t col) {
  L->fillRoundRect(x, y, 10, 10, 2, col);
}

static void drawSpeaker(int x, int y, uint16_t col) {
  L->fillRect(x, y + 3, 4, 6, col);
  L->fillTriangle(x + 4, y + 3, x + 4, y + 9, x + 9, y + 1, col);
  L->drawArc(x + 9, y + 6, 2, 3, -55, 55, col);
}

static void drawSpinner(int cx, int cy, int r, uint16_t col) {
  float a = (float)((millis() / 3) % 360);
  L->drawArc(cx, cy, r - 2, r, a, a + 260, col);
}

// Обрезает строку по ширине, добавляя "..".
static String fitText(const char* s, int maxW, const lgfx::IFont* f) {
  L->setFont(f);
  if (L->textWidth(s) <= maxW) return String(s);
  String r(s);
  while (r.length() && L->textWidth((r + "..").c_str()) > maxW) r.remove(r.length() - 1);
  return r + "..";
}

// -----------------------------------------------------------------------------
//  Верхняя строка
// -----------------------------------------------------------------------------
static void drawTopBar() {
  L->fillRect(0, 0, 320, 28, C_BG);
  L->fillRect(0, 27, 320, 1, C_LINE);
  L->fillRoundRect(9, 9, 9, 9, 2, C_ACCENT);
  L->setFont(F_SM);
  text("M5 RADIO", 24, 6, C_TEXT, F_SM);

  // Wi-Fi
  bool conn = (WiFi.status() == WL_CONNECTED);
  if (conn) {
    drawWifiBars(218, 20, WiFi.RSSI(), C_ACCENT);
  } else {
    text("OFFLINE", 150, 6, C_RED, F_SM);
  }

  // Громкость
  drawSpeaker(248, 9, C_DIM);
  char b[12];
  snprintf(b, sizeof(b), "%d%%", pctVolume());
  text(b, 314, 6, C_DIM, F_SM, textdatum_t::top_right);
}

// -----------------------------------------------------------------------------
//  Экран плеера
// -----------------------------------------------------------------------------
static String shownTitle;
static int    titleScroll = 0;
static uint32_t titleScrollAt = 0;

static void drawNowPlaying() {
  L->setClipRect(8, 138, 304, 34);
  L->fillRect(8, 138, 304, 34, C_BG);   // без очистки бегущая строка «смазывается»
  uint8_t st = radio::state();

  if (st == radio::Buffering) {
    L->setFont(F_MD);
    L->setTextColor(C_ACCENT);
    L->drawString("подключение", 14, 142);
    char b[24];
    snprintf(b, sizeof(b), "%u%%", (unsigned)(radio::fill() / 10));
    L->setTextColor(C_DIM);
    L->drawString(b, 300, 142);
    L->fillRect(14, 167, 292, 3, C_LINE);
    L->fillRect(14, 167, 292 * radio::fill() / 1000, 3, C_ACCENT);
  }
  else if (st == radio::Failed) {
    L->setFont(F_MD);
    L->setTextColor(C_RED);
    L->drawString("нет потока", 14, 142);
    text("B — повторить", 296, 148, C_DIM, F_SM, textdatum_t::top_right);
  }
  else if (st == radio::Stopped) {
    L->setFont(F_MD);
    L->setTextColor(C_DIM);
    L->drawString("стоп", 14, 142);
    text("B — играть", 296, 148, C_DIM, F_SM, textdatum_t::top_right);
  }
  else {
    char title[192];
    radio::getTitle(title, sizeof(title));
    if (!title[0]) strcpy(title, "прямой эфир");

    if (shownTitle != title) { shownTitle = title; titleScroll = 0; titleScrollAt = millis(); }

    L->setFont(F_LG);
    int tw = L->textWidth(title);
    int off = 0;
    if (tw > 292) {
      if (millis() - titleScrollAt > 70) {
        titleScroll += 1;
        titleScrollAt = millis();
        if (titleScroll > tw - 292 + 60) titleScroll = 0;
      }
      off = titleScroll;
    } else {
      titleScroll = 0;
    }
    L->setTextColor(C_TEXT);
    L->drawString(title, 14 - off, 140);
  }
  L->clearClipRect();
}

static void drawVu() {
  const int x0 = 12, base = 196, bw = 10, gap = 2, bars = 24, hmax = 14;
  int lv = radio::level();
  uint32_t t = millis();
  for (int i = 0; i < bars; ++i) {
    int x = x0 + i * (bw + gap);
    L->fillRect(x, base - hmax, bw, hmax, C_LINE);
    float center = 1.0f - fabsf((i - (bars - 1) / 2.0f) / ((bars - 1) / 2.0f));
    int h = (int)((hmax - 1) * (lv / 100.0f) * (0.30f + 0.70f * center));
    if (lv > 2) h += (int)((t / 90 + i * 7) % 3);   // лёгкое живое дрожание
    if (h > hmax) h = hmax;
    if (h > 0) {
      uint16_t c = (i < bars * 0.55f) ? C_GREEN : (i < bars * 0.8f ? C_AMBER : C_RED);
      L->fillRect(x, base - h, bw, h, c);
    }
  }
  // линия буфера
  L->fillRect(12, 199, 296, 2, C_LINE);
  uint32_t f = radio::fill();
  if (f > 1000) f = 1000;
  L->fillRect(12, 199, 296 * f / 1000, 2, radio::state() == radio::Playing ? C_PANEL2 : C_ACCENT);
}

static void drawHints() {
  text("A/C — станция", 10, 204, C_DIM, F_SM);
  text("B — плей/стоп", 135, 204, C_DIM, F_SM);
  text("держ. B — меню", 10, 220, C_DIM, F_SM);
  text("держ. A/C — звук", 160, 220, C_DIM, F_SM);
}

static void drawPlayer() {
  L->fillScreen(C_BG);
  drawTopBar();

  int idx = stationSel;
  const Station& st = STATIONS[idx];

  // карточка станции
  L->fillRoundRect(8, 30, 304, 92, 12, C_PANEL);
  L->fillRoundRect(8, 30, 5, 92, 2, C_ACCENT);

  const lgfx::IFont* nf = (L->textWidth(st.name, F_XL) <= 280) ? F_XL : F_LG;
  String nm = fitText(st.name, 280, nf);
  L->setFont(nf);
  L->setTextColor(C_TEXT);
  L->drawString(nm.c_str(), 22, 40);

  L->setFont(F_MD);
  L->setTextColor(C_DIM);
  L->drawString(st.genre, 22, 88);

  // индикатор состояния
  uint8_t rs = radio::state();
  if (rs == radio::Playing) drawPlayIcon(272, 104, C_GREEN);
  else if (rs == radio::Buffering) drawSpinner(277, 110, 7, C_ACCENT);
  else drawStopIcon(272, 104, C_DIM);

  L->setFont(F_SM);
  L->setTextColor(C_PINK);
  L->drawString("СЕЙЧАС ИГРАЕТ", 12, 124);

  drawNowPlaying();
  drawVu();
  drawHints();
}

static void updatePlayerDyn() {
  // верхняя строка раз в секунду (RSSI)
  drawTopBar();
  drawNowPlaying();
  drawVu();
}

// -----------------------------------------------------------------------------
//  Список станций
// -----------------------------------------------------------------------------
static void drawStations() {
  L->fillScreen(C_BG);
  drawTopBar();
  text("СТАНЦИИ", 12, 32, C_TEXT, F_LG);
  char b[16];
  snprintf(b, sizeof(b), "%d", STATION_COUNT);
  text(b, 308, 40, C_DIM, F_MD, textdatum_t::top_right);

  const int rows = 5, rowH = 30, y0 = 64;
  int top = listSel - 2;
  if (top < 0) top = 0;
  if (top > STATION_COUNT - rows) top = STATION_COUNT - rows;
  if (top < 0) top = 0;

  for (int r = 0; r < rows; ++r) {
    int i = top + r;
    if (i >= STATION_COUNT) break;
    int y = y0 + r * rowH;
    bool sel = (i == listSel);
    if (sel) {
      L->fillRoundRect(8, y, 304, rowH - 3, 8, C_PANEL2);
      L->fillRoundRect(8, y, 4, rowH - 3, 2, C_ACCENT);
    }
    char n[8];
    snprintf(n, sizeof(n), "%02d", i + 1);
    text(n, 18, y + 7, sel ? C_ACCENT : C_DIM, F_SM);

    String name = fitText(STATIONS[i].name, 250, F_MD);
    text(name.c_str(), 42, y + 4, sel ? C_TEXT : C_DIM, F_MD);
  }

  text("A/C выбор", 12, 218, C_DIM, F_SM);
  text("B играть", 106, 218, C_DIM, F_SM);
  text("держ. B назад", 195, 218, C_DIM, F_SM);
}

// -----------------------------------------------------------------------------
//  Меню
// -----------------------------------------------------------------------------
static void drawMenu() {
  L->fillScreen(C_BG);
  drawTopBar();
  text("МЕНЮ", 12, 32, C_TEXT, F_LG);

  const char* modeNames[] = {"динамик", "Bluetooth", "I2S"};
  for (int i = 0; i < MENU_COUNT; ++i) {
    int y = 64 + i * 32;
    bool sel = (i == menuSel);
    if (sel) {
      L->fillRoundRect(8, y, 304, 28, 8, C_PANEL2);
      L->fillRoundRect(8, y, 4, 28, 2, C_ACCENT);
    }
    text(MENU_ITEMS[i], 26, y + 5, sel ? C_TEXT : C_DIM, F_MD);
    if (i == 0) text("список радиостанций", 306, y + 9, C_LINE, F_SM, textdatum_t::top_right);
    if (i == 1) text(modeNames[outMode], 306, y + 9, sel ? C_ACCENT : C_LINE, F_SM, textdatum_t::top_right);
    if (i == 2) {
      String s = wifiSsid.length() ? wifiSsid : String("не настроено");
      s = fitText(s.c_str(), 110, F_SM);
      text(s.c_str(), 306, y + 9, sel ? C_ACCENT : C_LINE, F_SM, textdatum_t::top_right);
    }
    if (i == 3) text("IP и память", 306, y + 9, C_LINE, F_SM, textdatum_t::top_right);
  }

  text("A/C выбор", 12, 218, C_DIM, F_SM);
  text("B открыть", 108, 218, C_DIM, F_SM);
  text("держ. B назад", 195, 218, C_DIM, F_SM);
}

// -----------------------------------------------------------------------------
//  Выбор выхода звука
// -----------------------------------------------------------------------------
static void drawOutput() {
  L->fillScreen(C_BG);
  drawTopBar();
  text("ВЫХОД ЗВУКА", 12, 32, C_TEXT, F_LG);

  for (int i = 0; i < OUT_COUNT; ++i) {
    int y = 64 + i * 32;
    bool sel = (i == outSel);
    if (sel) {
      L->fillRoundRect(8, y, 304, 28, 8, C_PANEL2);
      L->fillRoundRect(8, y, 4, 28, 2, C_ACCENT);
    }
    text(OUT_ITEMS[i], 26, y + 5, sel ? C_TEXT : C_DIM, F_MD);
    if (i < 3 && outMode == i) text("текущий", 306, y + 9, C_ACCENT, F_SM, textdatum_t::top_right);
  }

  text("A/C выбор", 12, 218, C_DIM, F_SM);
  text("B выбор", 112, 218, C_DIM, F_SM);
  text("держ. B назад", 200, 218, C_DIM, F_SM);
}

// -----------------------------------------------------------------------------
//  Bluetooth: на ESP32 без PSRAM одновременная работа Wi-Fi (радио) и
//  Classic Bluetooth A2DP физически невозможна из-за нехватки SRAM (320 КБ):
//  стек Wi-Fi оставляет ~60 КБ, а Bluedroid требует >110 КБ.
//  Поддерживается на платах с PSRAM (M5Stack Core2, Fire).
// -----------------------------------------------------------------------------
static void drawBluetooth() {
  L->fillScreen(C_BG);
  drawTopBar();
  text("BLUETOOTH", 12, 32, C_TEXT, F_LG);

#if HAS_BLUETOOTH
  {
    const char* st;
    uint16_t stc = C_DIM;
    if (btState == 3) { st = "подключено"; stc = C_GREEN; }
    else if (btState == 2) { st = "подключение..."; stc = C_ACCENT; }
    else if (btState == 1) { st = "поиск устройств..."; stc = C_ACCENT; }
    else if (btState == 4) { st = "ошибка подключения"; stc = C_RED; }
    else st = "не подключено";
    text(st, 12, 64, stc, F_MD);
    if (btName[0]) {
      String n = fitText(btName, 140, F_SM);
      text(n.c_str(), 308, 68, C_DIM, F_SM, textdatum_t::top_right);
    }

    const int rows = btDevCount + 1;
    const int y0 = 92, rowH = 26, vis = 4;
    int top = btDevSel - 2;
    if (top < 0) top = 0;
    if (top > rows - vis) top = rows - vis;
    if (top < 0) top = 0;

    for (int r = 0; r < vis; ++r) {
      int i = top + r;
      if (i >= rows) break;
      int y = y0 + r * rowH;
      bool sel = (i == btDevSel);
      if (sel) {
        L->fillRoundRect(8, y, 304, rowH - 3, 6, C_PANEL2);
        L->fillRoundRect(8, y, 4, rowH - 3, 2, C_ACCENT);
      }
      if (i == btDevCount) {
        text("Поиск устройств заново", 22, y + 4, sel ? C_ACCENT : C_DIM, F_MD);
      } else {
        String name = fitText(btDevs[i].name, 200, F_MD);
        text(name.c_str(), 22, y + 4, sel ? C_TEXT : C_DIM, F_MD);
        char b[16];
        snprintf(b, sizeof(b), "%d", btDevs[i].rssi);
        text(b, 306, y + 6, sel ? C_ACCENT : C_LINE, F_SM, textdatum_t::top_right);
      }
    }

    text("A/C выбор", 12, 218, C_DIM, F_SM);
    text("B выбрать", 108, 218, C_DIM, F_SM);
    text("держ. B назад", 195, 218, C_DIM, F_SM);
    return;
  }
#endif

  text("Недоступно на этой плате", 12, 64, C_RED, F_MD);
  text("Для работы Wi-Fi и Bluetooth", 12, 90, C_DIM, F_SM);
  text("нужен ESP32 с PSRAM (Core2/Fire).", 12, 106, C_DIM, F_SM);

  text("На M5Stack Basic всего 320 КБ RAM:", 12, 128, C_TEXT, F_SM);
  text("Wi-Fi оставляет ~60 КБ, а BT", 12, 144, C_TEXT, F_SM);
  text("требует от 110 КБ (не хватает памяти).", 12, 160, C_TEXT, F_SM);

  text("Для чистого звука подключите", 12, 184, C_ACCENT, F_SM);
  text("I2S DAC (MAX98357A) к шине.", 12, 200, C_ACCENT, F_SM);

  text("B — назад", 12, 222, C_DIM, F_SM);
}



// -----------------------------------------------------------------------------
//  Информация
// -----------------------------------------------------------------------------
static void drawInfo() {
  L->fillScreen(C_BG);
  drawTopBar();
  text("ИНФОРМАЦИЯ", 12, 32, C_TEXT, F_LG);

  char b[64];
  int y = 72;
  auto line = [&](const char* label, const String& value, uint16_t col) {
    text(label, 14, y + 6, C_DIM, F_SM);
    text(value.c_str(), 148, y, col, F_MD);
    y += 24;
  };

  line("Wi-Fi", wifiSsid.length() ? wifiSsid : String("—"), C_TEXT);
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(b, sizeof(b), "%d dBm", WiFi.RSSI());
    line("Сигнал", b, WiFi.RSSI() > -70 ? C_GREEN : C_AMBER);
    line("IP", WiFi.localIP().toString(), C_TEXT);
  } else {
    line("Сеть", "нет подключения", C_RED);
  }
  snprintf(b, sizeof(b), "%u КБ", (unsigned)(ESP.getFreeHeap() / 1024));
  line("Свободно RAM", b, C_TEXT);
  {
    const char* names[] = {"встроенный", "Bluetooth", "I2S DAC"};
    line("Выход", names[outMode < 3 ? outMode : 0], C_TEXT);
  }
  snprintf(b, sizeof(b), "%d", STATION_COUNT);
  line("Станций", b, C_TEXT);

  text("B — назад", 12, 218, C_DIM, F_SM);
}

// -----------------------------------------------------------------------------
//  Wi-Fi: сканирование
// -----------------------------------------------------------------------------
static int rssiToBars(int rssi) {
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  if (rssi > -85) return 1;
  return 0;
}

static void sortNets() {
  for (int i = 1; i < netCount; ++i) {
    NetInfo tmp = nets[i];
    int j = i - 1;
    while (j >= 0 && nets[j].rssi < tmp.rssi) { nets[j + 1] = nets[j]; --j; }
    nets[j + 1] = tmp;
  }
}

static void buildNets() {
  netCount = 0;
  int found = WiFi.scanComplete();
  if (found < 0) return;
  for (int i = 0; i < found; ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    int rssi = WiFi.RSSI(i);
    bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    int existing = -1;
    for (int k = 0; k < netCount; ++k) {
      if (ssid.equals(nets[k].ssid)) { existing = k; break; }
    }
    if (existing >= 0) {
      if (rssi > nets[existing].rssi) nets[existing].rssi = rssi;
      continue;
    }
    if (netCount >= MAX_NET) continue;
    NetInfo& n = nets[netCount++];
    strncpy(n.ssid, ssid.c_str(), sizeof(n.ssid) - 1);
    n.ssid[sizeof(n.ssid) - 1] = 0;
    n.rssi = rssi;
    n.open = open;
  }
  WiFi.scanDelete();
  sortNets();
  if (netSel >= netCount) netSel = netCount ? netCount - 1 : 0;
  LOGF("[wifi] scan done: %d networks\n", netCount);
  for (int i = 0; i < netCount && i < 3; ++i)
    LOGF("  %s  %d dBm%s\n", nets[i].ssid, nets[i].rssi, nets[i].open ? " (open)" : "");
}

static void drawWifiScan() {
  L->fillScreen(C_BG);
  drawTopBar();
  text("WI-FI СЕТИ", 12, 32, C_TEXT, F_LG);

  if (scanning) {
    drawSpinner(298, 52, 10, C_ACCENT);
    text("поиск сетей...", 12, 70, C_ACCENT, F_MD);
    text("удерж. B — назад", 12, 218, C_DIM, F_SM);
    return;
  }
  if (scanError) {
    text("не удалось просканировать", 12, 74, C_RED, F_MD);
    text("B — повторить, удерж. B — назад", 12, 214, C_DIM, F_SM);
    return;
  }
  if (netCount == 0) {
    text("сети не найдены", 12, 74, C_DIM, F_MD);
    text("B — повторить, удерж. B — назад", 12, 214, C_DIM, F_SM);
    return;
  }

  const int rows = 5, rowH = 30, y0 = 64;
  int top = netSel - 2;
  if (top < 0) top = 0;
  if (top > netCount - rows) top = netCount - rows;
  if (top < 0) top = 0;

  for (int r = 0; r < rows; ++r) {
    int i = top + r;
    if (i >= netCount) break;
    int y = y0 + r * rowH;
    bool sel = (i == netSel);
    if (sel) {
      L->fillRoundRect(8, y, 304, rowH - 3, 8, C_PANEL2);
      L->fillRoundRect(8, y, 4, rowH - 3, 2, C_ACCENT);
    }
    String name = fitText(nets[i].ssid, 200, F_MD);
    text(name.c_str(), 22, y + 4, sel ? C_TEXT : C_DIM, F_MD);

    int bars = rssiToBars(nets[i].rssi);
    for (int b = 0; b < 4; ++b) {
      int h = 4 + b * 3;
      L->fillRect(276 + b * 5, y + 22 - h, 3, h, b < bars ? (sel ? C_ACCENT : C_DIM) : C_LINE);
    }
    if (!nets[i].open) drawLock(258, y + 6, sel ? C_ACCENT : C_DIM);
  }

  char b[32];
  snprintf(b, sizeof(b), "сетей: %d", netCount);
  text(b, 12, 218, C_DIM, F_SM);
  text("B выбрать", 90, 218, C_DIM, F_SM);
  text("держ. B назад", 198, 218, C_DIM, F_SM);
}

static void startWifiScan();

static void updateWifiScanDyn() {
  if (scanning) {
    int r = WiFi.scanComplete();
    if (r >= 0) {
      scanning = false;
      buildNets();
      needFullDraw = true;
    } else if (millis() - scanStart > 20000) {
      scanning = false;
      scanError = true;
      needFullDraw = true;
    } else {
      // перерисовать только индикатор
      L->fillRect(272, 38, 44, 34, C_BG);
      L->fillRect(8, 66, 210, 24, C_BG);
      drawSpinner(298, 52, 10, C_ACCENT);
      text("поиск сетей...", 12, 70, C_ACCENT, F_MD);
    }
  }
}

// -----------------------------------------------------------------------------
//  Wi-Fi: ввод пароля
// -----------------------------------------------------------------------------
static int kbRowLen(int page, int row) {
  if (row == 4) return 4;
  return (int)strlen(KB_LAYOUT[page][row]);
}

static char kbChar() { return KB_LAYOUT[kbPage][kbRow][kbCol]; }

static void kbMoveVertical(int dir) {
  int oldLen = kbRowLen(kbPage, kbRow);
  int newRow = (kbRow + dir + 5) % 5;
  int newLen = kbRowLen(kbPage, newRow);
  int col = kbCol;
  if (newLen > 1 && oldLen > 1) col = col * (newLen - 1) / (oldLen - 1);
  if (col >= newLen) col = newLen - 1;
  kbRow = newRow;
  kbCol = col;
}

static void drawPassword() {
  L->fillScreen(C_BG);
  drawTopBar();

  String head = "WI-FI: " + passSsid;
  head = fitText(head.c_str(), 296, F_SM);
  text(head.c_str(), 12, 32, C_DIM, F_SM);

  // поле пароля
  L->fillRoundRect(8, 50, 304, 32, 8, C_PANEL);
  L->drawRoundRect(8, 50, 304, 32, 8, C_LINE);

  L->setFont(F_MD);
  String shown;
  if (passInput.length() == 0) {
    L->setTextColor(C_LINE);
    L->drawString("введите пароль", 26, 56);
  } else {
    bool recent = (millis() - lastCharAt) < 900;
    for (int i = 0; i < (int)passInput.length(); ++i) {
      char c = passInput[i];
      bool last = (i == (int)passInput.length() - 1);
      if (last && recent) shown += c;
      else shown += '*';
    }
    L->setClipRect(16, 52, 282, 28);
    L->setTextColor(C_TEXT);
    L->drawString(shown.c_str(), 20, 56);
    L->clearClipRect();
  }
  // курсор
  int cx = 20 + (passInput.length() ? L->textWidth(shown) : 0) + 2;
  if (cx < 300) L->fillRect(cx, 53, 2, 24, C_ACCENT);
  // длина
  char b[16];
  snprintf(b, sizeof(b), "%d", (int)passInput.length());
  text(b, 304, 56, C_DIM, F_SM, textdatum_t::top_right);

  // клавиатура
  for (int row = 0; row < 4; ++row) {
    int len = kbRowLen(kbPage, row);
    for (int col = 0; col < len; ++col) {
      int x = 10 + col * 30;
      int y = 86 + row * 23;
      bool sel = (kbRow == row && kbCol == col);
      L->fillRoundRect(x, y, 28, 21, 4, sel ? C_ACCENT : C_PANEL);
      char c[2] = { KB_LAYOUT[kbPage][row][col], 0 };
      text(c, x + 14, y + 3, sel ? C_BG : C_TEXT, F_SM, textdatum_t::top_center);
    }
  }

  // нижний ряд: страница, DEL, OK, назад
  const char* pageLabels[3] = {"ABC", "abc", "123"};
  const char* labels[4] = { pageLabels[kbPage], "DEL", "OK", "<" };
  for (int i = 0; i < 4; ++i) {
    int x = 10 + i * 76;
    int y = 180;
    bool sel = (kbRow == 4 && kbCol == i);
    L->fillRoundRect(x, y, 72, 24, 6, sel ? C_ACCENT : C_PANEL2);
    text(labels[i], x + 36, y + 4, sel ? C_BG : C_TEXT, F_SM, textdatum_t::top_center);
  }

  text("A/C выбор", 12, 207, C_DIM, F_SM);
  text("держ. A/C ряд", 110, 207, C_DIM, F_SM);
  text("B ввод", 240, 207, C_DIM, F_SM);
  text("держ. B стереть", 12, 223, C_DIM, F_SM);
  text("OK подключить", 175, 223, C_DIM, F_SM);
}

// -----------------------------------------------------------------------------
//  Подключение к Wi-Fi
// -----------------------------------------------------------------------------
static void startConnect(const String& ssid, const String& pass) {
  connSsid   = ssid;
  connPass   = pass;
  connStart  = millis();
  connResult = 0;
  connTried  = true;
  radio::stop();
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true); // необходимо для устойчивого coex Wi-Fi + BT
  WiFi.begin(connSsid.c_str(), connPass.c_str());
  LOGF("[wifi] connecting to %s\n", connSsid.c_str());
  screen = Screen::WifiConnecting;
  needFullDraw = true;
}

static void drawConnecting() {
  L->fillScreen(C_BG);
  drawTopBar();

  if (connResult == 0) {
    drawSpinner(160, 96, 24, C_ACCENT);
    text("ПОДКЛЮЧЕНИЕ", 160, 128, C_TEXT, F_LG, textdatum_t::top_center);
    String s = fitText(connSsid.c_str(), 280, F_MD);
    text(s.c_str(), 160, 164, C_ACCENT, F_MD, textdatum_t::top_center);
    int dots = (millis() / 400) % 4;
    char d[5] = "....";
    d[dots] = 0;
    text(d, 160, 192, C_DIM, F_MD, textdatum_t::top_center);
    text("удерж. B — отмена", 160, 222, C_DIM, F_SM, textdatum_t::top_center);
  } else {
    L->fillCircle(160, 84, 24, C_PANEL2);
    text("!", 160, 66, C_RED, F_XL, textdatum_t::top_center);
    text("НЕ УДАЛОСЬ", 160, 122, C_RED, F_LG, textdatum_t::top_center);
    text("проверьте пароль и сеть", 160, 160, C_DIM, F_SM, textdatum_t::top_center);
    text("B — повторить", 160, 188, C_DIM, F_SM, textdatum_t::top_center);
    text("удерж. B — к списку сетей", 160, 204, C_DIM, F_SM, textdatum_t::top_center);
  }
}

// -----------------------------------------------------------------------------
//  Навигация
// -----------------------------------------------------------------------------
static void drawScreen() {
  switch (screen) {
    case Screen::Splash:      break;
    case Screen::Player:      drawPlayer(); break;
    case Screen::Stations:    drawStations(); break;
    case Screen::Menu:        drawMenu(); break;
    case Screen::WifiScan:    drawWifiScan(); break;
    case Screen::Password:    drawPassword(); break;
    case Screen::WifiConnecting: drawConnecting(); break;
    case Screen::Info:        drawInfo(); break;
    case Screen::Output:      drawOutput(); break;
    case Screen::Bluetooth:   drawBluetooth(); break;
  }
  needFullDraw = false;
}

// Снимок текущего экрана: рисуем его во временный 8-битный спрайт и отдаём
// по COM-порту как RGB332 (320*240 = 76800 байт). Так интерфейс можно
// проверять без камеры.
static M5Canvas* shotCanvas = nullptr;
static uint32_t shotLast = 0;

static void takeShot() {
  static int shotBpp = 8;
  M5Canvas* canvas = shotCanvas;

  if (!canvas) {
    canvas = new M5Canvas(&M5.Display);
    canvas->setColorDepth(8);
    if (!canvas->createSprite(320, 240)) {
      canvas->setColorDepth(1);
      if (!canvas->createSprite(320, 240)) {
        Serial.println("SHOT FAIL");
        delete canvas;
        canvas = nullptr;
        return;
      }
      shotBpp = 1;
    }
    shotCanvas = canvas;
  }

  shotLast = millis();
  logQuiet = true;
  lgfx::LGFXBase* prev = L;
  L = canvas;
  needFullDraw = false;
  drawScreen();
  L = prev;

  const size_t bytes = shotBpp == 8 ? 320u * 240u : (320u * 240u) / 8u;
  Serial.printf("SHOT %d %d %d\n", 320, 240, shotBpp);
  Serial.write((const uint8_t*)canvas->getBuffer(), bytes);
  Serial.print("\nEND\n");
  Serial.flush();

  needFullDraw = true;
  logQuiet = false;
}

static void changeVolume(int dir) {
  int old = volume;
  volume += dir * 10;
  if (volume < 0) volume = 0;
  if (volume > 255) volume = 255;
  if (volume != old) {
    applyVolume();
    saveVolume();
    if (screen == Screen::Player || screen == Screen::Stations) drawTopBar();
  }
}

static void playStation(int idx) {
  stationSel = (idx + STATION_COUNT) % STATION_COUNT;
  saveStation();
  wantPlaying = true;
  radio::play(stationSel);
  shownTitle = "";
  titleScroll = 0;
  LOGF("[ui] station %d %s\n", stationSel, STATIONS[stationSel].name);
}

// -----------------------------------------------------------------------------
//  Управление по COM-порту (отладка/автотесты)//    A / B / C      — короткое нажатие кнопки
//    AH / BH / CH   — удержание
//    V+ / V-        — громкость
//    SHOT           — дамп экрана (RGB332) в порт
//    INFO           — состояние
//    HELP           — список команд
// -----------------------------------------------------------------------------
static void playStation(int idx);
static void goPlayer();

static void serialTick() {
  static char cmd[24];
  static uint8_t len = 0;

  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len) {
        cmd[len] = 0;
        len = 0;
        Serial.printf("CMD %s\n", cmd);
        if (!strcasecmp(cmd, "A")) synthClick[0] = 1;
        else if (!strcasecmp(cmd, "B")) synthClick[1] = 1;
        else if (!strcasecmp(cmd, "C")) synthClick[2] = 1;
        else if (!strcasecmp(cmd, "AH")) synthHold[0] = 1;
        else if (!strcasecmp(cmd, "BH")) synthHold[1] = 1;
        else if (!strcasecmp(cmd, "CH")) synthHold[2] = 1;
        else if (!strcasecmp(cmd, "V+")) changeVolume(+1);
        else if (!strcasecmp(cmd, "V-")) changeVolume(-1);
        else if (!strncasecmp(cmd, "ST ", 3)) {
          int n = atoi(cmd + 3);
          if (n >= 0 && n < STATION_COUNT) { playStation(n); goPlayer(); }
        }
        else if (!strncasecmp(cmd, "VOL ", 4)) {
          int v = atoi(cmd + 4);
          if (v < 0) v = 0; if (v > 255) v = 255;
          volume = v; applyVolume(); saveVolume(); drawTopBar();
        }
        else if (!strcasecmp(cmd, "SHOT")) takeShot();
                else if (!strcasecmp(cmd, "OUT DAC")) {
          outMode = 0; prefs.putInt("out", 0);
          radio::setMode(0, volume);
          if (wantPlaying) radio::play(stationSel);
        }
#if HAS_BLUETOOTH
        else if (!strcasecmp(cmd, "OUT BT")) {
          outMode = 1; prefs.putInt("out", 1);
          radio::setMode(1, volume);
          btConnectSaved();
          if (wantPlaying) radio::play(stationSel);
        }
        else if (!strcasecmp(cmd, "BTSCAN")) {
          btStartScan();
          screen = Screen::Bluetooth; needFullDraw = true;
        }
        else if (!strcasecmp(cmd, "BTLIST")) {
          Serial.printf("BT devices (%d):\n", btDevCount);
          for (int i = 0; i < btDevCount; ++i)
            Serial.printf("  [%d] %s (%d dBm)\n", i, btDevs[i].name, btDevs[i].rssi);
        }
        else if (!strncasecmp(cmd, "BTCON ", 6)) {
          int i = atoi(cmd + 6);
          btConnectIndex(i);
          outMode = 1; prefs.putInt("out", 1);
          radio::setMode(1, volume);
          goPlayer();
        }
#endif
        else if (!strcasecmp(cmd, "OUT I2S")) {
          outMode = 2; prefs.putInt("out", 2);
          radio::setMode(2, volume);
          if (wantPlaying) radio::play(stationSel);
        }
        else if (!strcasecmp(cmd, "INFO")) {
          Serial.printf("INFO screen=%d radio=%d station=%d wifi=%d out=%s heap=%u\n",
                        (int)screen, (int)radio::state(), stationSel,
                        (int)WiFi.status(), radio::mode() == 1 ? "bt" : (radio::mode() == 2 ? "i2s" : "dac"),
                        (unsigned)ESP.getFreeHeap());
        }
        else {
          Serial.println("HELP: A B C AH BH CH V+ V- SHOT INFO");
        }
      }
    } else if (len < sizeof(cmd) - 1) {
      cmd[len++] = ch;
    }
  }
}

static void goPlayer() {  screen = Screen::Player;
  needFullDraw = true;
  if (wantPlaying && WiFi.status() == WL_CONNECTED) {
    uint8_t st = radio::state();
    if (st != radio::Playing && st != radio::Buffering) radio::play(stationSel);
  }
}

static void openMenu() {
  menuSel = 0;
  screen = Screen::Menu;
  needFullDraw = true;
}

static void startWifiScan() {
  radio::stop();
  scanning = true;
  scanError = false;
  scanStart = millis();
  netCount = 0;
  netSel = 0;
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  WiFi.scanNetworks(true, false);
  LOGL("[ui] wifi scan start");
  screen = Screen::WifiScan;
  needFullDraw = true;
}

static void openPassword(int netIndex) {
  if (netIndex < 0 || netIndex >= netCount) return;
  if (nets[netIndex].open) {          // открытая сеть — подключаемся сразу
    startConnect(nets[netIndex].ssid, "");
    return;
  }
  passSsid  = nets[netIndex].ssid;
  passInput = "";
  kbPage = 0; kbRow = 0; kbCol = 0;
  lastCharAt = 0;
  screen = Screen::Password;
  needFullDraw = true;
}

// -----------------------------------------------------------------------------
//  Кнопки
// -----------------------------------------------------------------------------
struct Press { bool click; bool hold; };
static bool holdLatch[3] = {false, false, false};

static Press pollButton(m5::Button_Class& b, int i) {
  Press p{false, false};
  if (b.wasReleased()) holdLatch[i] = false;
  if (b.wasClicked()) p.click = true;
  if (b.wasHold() && !holdLatch[i]) { p.hold = true; holdLatch[i] = true; }
  if (synthClick[i]) { p.click = true; synthClick[i] = 0; }
  if (synthHold[i])  { p.hold  = true; synthHold[i]  = 0; }
  return p;
}

static void volumeRepeatTick() {
  m5::Button_Class* btns[2] = {&M5.BtnA, &M5.BtnC};
  for (int i = 0; i < 2; ++i) {
    if (btns[i]->isPressed() && btns[i]->pressedFor(550)) {
      uint32_t now = millis();
      if (volRepeat[i] == 0 || now - volRepeat[i] >= 130) {
        volRepeat[i] = now;
        changeVolume(i == 0 ? -1 : +1);
      }
    } else {
      volRepeat[i] = 0;
    }
  }
}

static void handleSplash() {
  if (millis() - splashStart > 1800) {
#if HAS_BLUETOOTH
    if (outMode == 1 && btMac[0]) btConnectSaved();
#endif
    if (wifiSsid.length()) startConnect(wifiSsid, wifiPass);
    else startWifiScan();
  }
}

static void handlePlayer(Press a, Press b, Press c) {
  volumeRepeatTick();
  if (a.click) { playStation(stationSel - 1); needFullDraw = true; }
  if (c.click) { playStation(stationSel + 1); needFullDraw = true; }
  if (b.click) {
    uint8_t st = radio::state();
    if (st == radio::Playing || st == radio::Buffering) {
      wantPlaying = false;
      radio::stop();
    } else {
      wantPlaying = true;
      radio::play(stationSel);
    }
    drawNowPlaying();
  }
  if (b.hold) openMenu();
}

static void handleStations(Press a, Press b, Press c) {
  if (a.click) { listSel = (listSel + STATION_COUNT - 1) % STATION_COUNT; needFullDraw = true; }
  if (c.click) { listSel = (listSel + 1) % STATION_COUNT; needFullDraw = true; }
  if (b.click) { playStation(listSel); goPlayer(); }
  if (b.hold) goPlayer();
}

static void handleMenu(Press a, Press b, Press c) {
  if (a.click) { menuSel = (menuSel + MENU_COUNT - 1) % MENU_COUNT; needFullDraw = true; }
  if (c.click) { menuSel = (menuSel + 1) % MENU_COUNT; needFullDraw = true; }
  if (b.hold) { goPlayer(); return; }
  if (!b.click) return;
  switch (menuSel) {
    case 0:
      listSel = (radio::playing() >= 0) ? radio::playing() : stationSel;
      screen = Screen::Stations;
      needFullDraw = true;
      break;
    case 1: outSel = outMode; screen = Screen::Output; needFullDraw = true; break;
    case 2: startWifiScan(); break;
    case 3: screen = Screen::Info; needFullDraw = true; break;
    default: goPlayer(); break;
  }
}

static void handleBluetooth(Press a, Press b, Press c) {
#if HAS_BLUETOOTH
  {
    int rows = btDevCount + 1;
    if (a.click) { btDevSel = (btDevSel + rows - 1) % rows; needFullDraw = true; }
    if (c.click) { btDevSel = (btDevSel + 1) % rows; needFullDraw = true; }
    if (b.hold) { screen = Screen::Output; needFullDraw = true; return; }
    if (!b.click) return;
    if (btDevSel == btDevCount) {
      btStartScan();
      needFullDraw = true;
    } else {
      btConnectIndex(btDevSel);
      outMode = 1;
      prefs.putInt("out", 1);
      radio::setMode(1, volume);
      goPlayer();
    }
    return;
  }
#endif
  (void)a; (void)c;
  if (b.click || b.hold) { screen = Screen::Output; needFullDraw = true; }
}

static void handleOutput(Press a, Press b, Press c) {
  if (a.click) { outSel = (outSel + OUT_COUNT - 1) % OUT_COUNT; needFullDraw = true; }
  if (c.click) { outSel = (outSel + 1) % OUT_COUNT; needFullDraw = true; }
  if (b.hold) { screen = Screen::Menu; needFullDraw = true; return; }
  if (!b.click) return;
  if (outSel == 0) {
    outMode = 0; prefs.putInt("out", 0); radio::setMode(0, volume); goPlayer();
  } else if (outSel == 1) {
    screen = Screen::Bluetooth;
#if HAS_BLUETOOTH
    if (!btInited || (!btDevCount && !a2dp.is_connected())) btStartScan();
#endif
    needFullDraw = true;
  } else if (outSel == 2) {
    outMode = 2; prefs.putInt("out", 2); radio::setMode(2, volume); goPlayer();
  } else {
    screen = Screen::Menu; needFullDraw = true;
  }
}



static void handleWifiScan(Press a, Press b, Press c) {
  if (scanning) {
    if (b.hold) goPlayer();
    return;
  }  if (scanError || netCount == 0) {
    if (b.click) startWifiScan();
    if (b.hold) goPlayer();
    return;
  }
  if (a.click) { netSel = (netSel + netCount - 1) % netCount; needFullDraw = true; }
  if (c.click) { netSel = (netSel + 1) % netCount; needFullDraw = true; }
  if (b.click) { openPassword(netSel); }
  if (b.hold) { goPlayer(); }
}

static void handlePassword(Press a, Press b, Press c) {
  if (a.click) {
    if (--kbCol < 0) {
      kbRow = (kbRow + 4) % 5;
      kbCol = kbRowLen(kbPage, kbRow) - 1;
    }
    needFullDraw = true;
  }
  if (c.click) {
    if (++kbCol >= kbRowLen(kbPage, kbRow)) {
      kbRow = (kbRow + 1) % 5;
      kbCol = 0;
    }
    needFullDraw = true;
  }
  if (a.hold) { kbMoveVertical(-1); needFullDraw = true; }
  if (c.hold) { kbMoveVertical(+1); needFullDraw = true; }
  if (b.hold) {
    if (passInput.length()) passInput.remove(passInput.length() - 1);
    needFullDraw = true;
  }
  if (b.click) {
    if (kbRow == 4) {
      if (kbCol == 0) {
        kbPage = (kbPage + 1) % 3;
        if (kbCol >= kbRowLen(kbPage, kbRow)) kbCol = kbRowLen(kbPage, kbRow) - 1;
        needFullDraw = true;
      }
      else if (kbCol == 1) { if (passInput.length()) passInput.remove(passInput.length() - 1); needFullDraw = true; }
      else if (kbCol == 2) { startConnect(passSsid, passInput); }
      else { screen = Screen::WifiScan; needFullDraw = true; }
    } else {
      passInput += kbChar();
      lastCharAt = millis();
      needFullDraw = true;
    }
  }
}
static void handleConnecting(Press a, Press b, Press c) {
  (void)a; (void)c;
  if (b.hold) { startWifiScan(); return; }
  if (connResult == -1 && b.click) startConnect(connSsid, connPass);
}

static void handleInfo(Press a, Press b, Press c) {
  (void)a; (void)c;
  if (b.click || b.hold) { screen = Screen::Menu; needFullDraw = true; }
}

// -----------------------------------------------------------------------------
//  Wi-Fi: фоновые события
// -----------------------------------------------------------------------------
static void wifiTick() {
  if (screen == Screen::WifiConnecting && connResult == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      connResult = 1;
      wifiSsid = connSsid;
      wifiPass = connPass;
      saveWifi();
      LOGF("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
      wantPlaying = true;
      playStation(stationSel);
      goPlayer();
    } else if (millis() - connStart > 20000) {
      connResult = -1;
      LOGL("[wifi] connect timeout");
      needFullDraw = true;
    }
  }

  // потеря связи во время игры
  static uint32_t lastDropCheck = 0;
  if (millis() - lastDropCheck > 1000) {
    lastDropCheck = millis();
    static bool wasConnected = false;
    bool nowConnected = (WiFi.status() == WL_CONNECTED);
    if (wasConnected && !nowConnected) {
      LOGL("[wifi] connection lost");
      if (radio::state() == radio::Playing || radio::state() == radio::Buffering) radio::stop();
    }
    wasConnected = nowConnected;
  }

  // авто-переподключение
  static uint32_t lastRetry = 0;
  if (wifiSsid.length() && WiFi.status() != WL_CONNECTED &&
      outMode != 1 && screen != Screen::Bluetooth &&
      screen != Screen::WifiConnecting && screen != Screen::WifiScan &&
      screen != Screen::Password && !scanning &&
      millis() - lastRetry > 15000) {
    lastRetry = millis();
    LOGL("[wifi] retry");
    WiFi.disconnect();
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  }
}

// -----------------------------------------------------------------------------
//  Splash
// -----------------------------------------------------------------------------
static void drawSplash() {
  L->fillScreen(C_BG);
  L->fillRoundRect(70, 56, 180, 68, 16, C_PANEL);
  L->fillRoundRect(70, 56, 6, 68, 3, C_ACCENT);
  text("M5", 92, 64, C_ACCENT, F_XL);
  text("RADIO", 156, 72, C_TEXT, F_LG);
  text("ИНТЕРНЕТ-РАДИО", 160, 136, C_DIM, F_SM, textdatum_t::top_center);

  // бегущая полоска загрузки
  int p = (millis() - splashStart) * 100 / 1800;
  if (p > 100) p = 100;
  L->fillRoundRect(70, 176, 180, 4, 2, C_LINE);
  L->fillRoundRect(70, 176, 180 * p / 100, 4, 2, C_ACCENT);

  text("M5Stack Basic · 320x240", 160, 206, C_LINE, F_SM, textdatum_t::top_center);
}

static void updateSplashDyn() {
  int p = (millis() - splashStart) * 100 / 1800;
  if (p > 100) p = 100;
  L->fillRect(68, 174, 184, 8, C_BG);
  L->fillRoundRect(70, 176, 180, 4, 2, C_LINE);
  L->fillRoundRect(70, 176, 180 * p / 100, 4, 2, C_ACCENT);
}

static void updateConnectingDyn() {
  if (connResult != 0) return;
  L->fillRect(132, 68, 56, 56, C_BG);
  drawSpinner(160, 96, 24, C_ACCENT);
  L->fillRect(130, 190, 60, 22, C_BG);
  int dots = (millis() / 400) % 4;
  char d[5] = "....";
  d[dots] = 0;
  text(d, 160, 192, C_DIM, F_MD, textdatum_t::top_center);
}

// -----------------------------------------------------------------------------
//  setup / loop
// -----------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  delay(150);
  LOGL("\n=== M5 Stack Radio ===");
  LOGF("board=%d  heap=%u  psram=%u\n", (int)M5.getBoard(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getPsramSize());

  M5.Display.setBrightness(200);
  L->setTextDatum(textdatum_t::top_left);
  L->setTextWrap(false);

  loadPrefs();
  LOGF("[prefs] ssid='%s' vol=%d station=%d\n", wifiSsid.c_str(), volume, stationSel);

  // динамик: для DAC-платы (M5Stack Basic) M5Unified сам удваивает частоту,
  // перезаписывать sample_rate нельзя — иначе появляется сильный шип.
  M5.Speaker.begin();

  M5.BtnA.setHoldThresh(550);
  M5.BtnB.setHoldThresh(550);
  M5.BtnC.setHoldThresh(550);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true); // coex
  WiFi.disconnect();

  radio::begin();
  if (outMode) radio::setMode(outMode, volume);
  applyVolume();

  splashStart = millis();
  screen = Screen::Splash;
  drawSplash();
  needFullDraw = false;
}

void loop() {
  M5.update();
  serialTick();

  Press a = pollButton(M5.BtnA, 0);
  Press b = pollButton(M5.BtnB, 1);
  Press c = pollButton(M5.BtnC, 2);

  switch (screen) {
    case Screen::Splash:         handleSplash(); break;
    case Screen::Player:         handlePlayer(a, b, c); break;
    case Screen::Stations:       handleStations(a, b, c); break;
    case Screen::Menu:           handleMenu(a, b, c); break;
    case Screen::WifiScan:       handleWifiScan(a, b, c); break;
    case Screen::Password:       handlePassword(a, b, c); break;
    case Screen::WifiConnecting: handleConnecting(a, b, c); break;
    case Screen::Info:           handleInfo(a, b, c); break;
    case Screen::Output:         handleOutput(a, b, c); break;
    case Screen::Bluetooth:      handleBluetooth(a, b, c); break;
  }

  wifiTick();
#if HAS_BLUETOOTH
  btPoll();
#endif

  if (screen == Screen::WifiScan) updateWifiScanDyn();

  if (needFullDraw) drawScreen();

  // динамика экранов
  uint32_t now = millis();
  if (screen == Screen::Splash) {
    if (now - lastDyn > 60) { lastDyn = now; updateSplashDyn(); }
  }
  else if (screen == Screen::Player) {
    if (now - lastDyn > 80) { lastDyn = now; updatePlayerDyn(); }
  }
  else if (screen == Screen::Password) {
    // через 0.9 с после ввода скрываем последний символ
    static uint32_t lastPwd = 0;
    if (lastCharAt && now - lastCharAt >= 900 && now - lastCharAt < 1100 && now - lastPwd > 100) {
      lastPwd = now;
      drawPassword();
    }
  }
  else if (screen == Screen::WifiConnecting) {
    static uint32_t lastAnim = 0;
    if (now - lastAnim > 100) { lastAnim = now; updateConnectingDyn(); }
  }

  // отладочный спрайт занимает 77 КБ — освобождаем, когда он долго не нужен
  if (shotCanvas && now - shotLast > 60000) {
    shotCanvas->deleteSprite();
    delete shotCanvas;
    shotCanvas = nullptr;
  }

  static uint32_t hb = 0;
  if (now - hb > 5000) {
    hb = now;
    LOGF("[hb] screen=%d radio=%d wifi=%d heap=%u\n", (int)screen,
                  (int)radio::state(), (int)WiFi.status(), (unsigned)ESP.getFreeHeap());
  }

  delay(5);
}
