// =============================================================================
//  M5Stack Basic: Автономный тест Bluetooth A2DP Source + Генератор синусоиды
// =============================================================================
//  Wi-Fi выключен — вся память (240+ КБ) свободна для Bluedroid.
//  Генерирует стерео синусоиду 44.1 кГц 16 бит в Bluetooth-наушники/колонку.
//  Экранный интерфейс на 3 кнопках: поиск, список устройств, подключение.
// =============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <BluetoothA2DPSource.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "../src/fonts/font8.h"
#include "../src/fonts/font11.h"
#include "../src/fonts/font16b.h"
#include "../src/fonts/font22b.h"

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

static const lgfx::IFont* F_SM = &Rf8;
static const lgfx::IFont* F_MD = &Rf11;
static const lgfx::IFont* F_LG = &Rf16b;
static const lgfx::IFont* F_XL = &Rf22b;

static lgfx::LGFXBase* L = &M5.Display;

// -----------------------------------------------------------------------------
//  Генератор синусоиды
// -----------------------------------------------------------------------------
static volatile float currentFreq = 440.0f;
static volatile int   sineVolume  = 80;   // 0..100 %

static int32_t btSineCallback(uint8_t* data, int32_t len) {
  int16_t* samples = (int16_t*)data;
  size_t count = len / 4;   // 4 байта на стерео сэмпл (L:16 бит, R:16 бит)
  static float phase = 0.0f;
  float step = 2.0f * 3.14159265f * currentFreq / 44100.0f;
  float amp = (sineVolume / 100.0f) * 16000.0f;

  for (size_t i = 0; i < count; ++i) {
    int16_t v = (int16_t)(sinf(phase) * amp);
    samples[i * 2]     = v; // Left
    samples[i * 2 + 1] = v; // Right
    phase += step;
    if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
  }
  return len;
}

// -----------------------------------------------------------------------------
//  Bluetooth A2DP Source
// -----------------------------------------------------------------------------
static BluetoothA2DPSource a2dp;
static bool     btInited    = false;
static int      btState     = 0;   // 0 стоп, 1 поиск, 2 подключение, 3 играет, 4 ошибка
static uint32_t btConnStart = 0;

struct BtDev { char name[32]; esp_bd_addr_t addr; int rssi; };
static const int BT_MAX = 15;
static BtDev btDevs[BT_MAX];
static int   btDevCount = 0;
static int   btDevSel   = 0;

static char btTargetName[32] = "";
static bool needRedraw = true;

static bool btSsidCallback(const char* ssid, esp_bd_addr_t addr, int rssi) {
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
  needRedraw = true;
  Serial.printf("[bt] found: '%s' (%02X:%02X:%02X:%02X:%02X:%02X) rssi=%d\n",
                d.name, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], rssi);
  return false; // не подключаемся автоматически к первому попавшемуся
}

static void btDiscCallback(esp_bt_gap_discovery_state_t state) {
  if (state == ESP_BT_GAP_DISCOVERY_STOPPED && btState == 1) {
    btState = (btDevCount > 0) ? 0 : 4;
    needRedraw = true;
    Serial.printf("[bt] discovery stopped, found %d devices\n", btDevCount);
  }
}

static void btInit() {
  if (btInited) return;
  a2dp.set_local_name("M5 Stack Test");
  a2dp.set_data_callback(btSineCallback);
  a2dp.set_ssp_enabled(true);
  a2dp.set_ssid_callback(btSsidCallback);
  a2dp.set_discovery_mode_callback(btDiscCallback);
  a2dp.start();
  btInited = true;
  Serial.printf("[bt] A2DP Source started! Free heap: %u\n", (unsigned)ESP.getFreeHeap());
}

static void btStartScan() {
  btInit();
  btDevCount = 0;
  btDevSel   = 0;
  btState    = 1;
  needRedraw = true;
  esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  Serial.println("[bt] scanning for devices (10s)...");
}

static void btConnect(int idx) {
  if (idx < 0 || idx >= btDevCount) return;
  a2dp.cancel_discovery();
  a2dp.connect_to(btDevs[idx].addr);
  strncpy(btTargetName, btDevs[idx].name, sizeof(btTargetName) - 1);
  btTargetName[sizeof(btTargetName) - 1] = 0;
  btConnStart = millis();
  btState = 2; // подключение
  needRedraw = true;
  Serial.printf("[bt] connecting to: %s\n", btTargetName);
}

// -----------------------------------------------------------------------------
//  Графика и отрисовка
// -----------------------------------------------------------------------------
static void text(const char* s, int x, int y, uint16_t col,
                 const lgfx::IFont* f = nullptr,
                 textdatum_t datum = textdatum_t::top_left) {
  if (f) L->setFont(f);
  L->setTextColor(col);
  L->setTextDatum(datum);
  L->drawString(s, x, y);
  L->setTextDatum(textdatum_t::top_left);
}

static String fitText(const char* s, int maxW, const lgfx::IFont* f) {
  L->setFont(f);
  if (L->textWidth(s) <= maxW) return String(s);
  String r(s);
  while (r.length() && L->textWidth((r + "..").c_str()) > maxW) r.remove(r.length() - 1);
  return r + "..";
}

static void drawTopBar() {
  L->fillRect(0, 0, 320, 28, C_BG);
  L->fillRect(0, 27, 320, 1, C_LINE);
  L->fillRoundRect(9, 9, 9, 9, 2, C_ACCENT);
  text("M5 BLUETOOTH TEST", 24, 6, C_TEXT, F_SM);

  char b[32];
  snprintf(b, sizeof(b), "RAM %uK", (unsigned)(ESP.getFreeHeap() / 1024));
  text(b, 240, 6, C_DIM, F_SM);
}

static void drawSineWave(int x0, int y0, int w, int h) {
  L->fillRect(x0, y0, w, h, C_PANEL);
  L->drawRect(x0, y0, w, h, C_LINE);
  int midY = y0 + h / 2;
  float t = millis() / 100.0f;
  for (int x = 0; x < w - 1; ++x) {
    float y1 = sinf(t + (x * 0.15f)) * (h / 2 - 4);
    float y2 = sinf(t + ((x + 1) * 0.15f)) * (h / 2 - 4);
    L->drawLine(x0 + x, midY + (int)y1, x0 + x + 1, midY + (int)y2, C_GREEN);
  }
}

static void drawUI() {
  L->fillScreen(C_BG);
  drawTopBar();

  text("BT SINE GENERATOR", 12, 32, C_TEXT, F_LG);

  // Статус
  if (btState == 3) {
    L->fillCircle(18, 66, 5, C_GREEN);
    text("ИГРАЕТ СИНУСОИДА", 28, 58, C_GREEN, F_MD);
    char fstr[32];
    snprintf(fstr, sizeof(fstr), "%.0f Гц · %d%%", currentFreq, sineVolume);
    text(fstr, 308, 60, C_TEXT, F_SM, textdatum_t::top_right);
    drawSineWave(12, 78, 296, 36);
  } else if (btState == 2) {
    L->fillCircle(18, 66, 5, C_ACCENT);
    text("ПОДКЛЮЧЕНИЕ...", 28, 58, C_ACCENT, F_MD);
    text(fitText(btTargetName, 120, F_SM).c_str(), 308, 60, C_DIM, F_SM, textdatum_t::top_right);
  } else if (btState == 1) {
    L->fillCircle(18, 66, 5, C_AMBER);
    text("ПОИСК...", 28, 58, C_AMBER, F_MD);
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "найдено: %d", btDevCount);
    text(cnt, 308, 60, C_DIM, F_SM, textdatum_t::top_right);
  } else if (btState == 4) {
    L->fillCircle(18, 66, 5, C_RED);
    text("НЕ НАЙДЕНО / СБОЙ", 28, 58, C_RED, F_MD);
  } else {
    L->fillCircle(18, 66, 5, C_DIM);
    text("НЕ ПОДКЛЮЧЕНО", 28, 58, C_DIM, F_MD);
  }

  // Список устройств (если играет, сдвигаем ниже)
  const int y0 = (btState == 3) ? 120 : 84;
  const int vis = (btState == 3) ? 3 : 4;
  const int rowH = 26;
  const int totalRows = btDevCount + 1; // +1 для пункта "Поиск заново"

  int top = btDevSel - (vis / 2);
  if (top < 0) top = 0;
  if (top > totalRows - vis) top = totalRows - vis;
  if (top < 0) top = 0;

  for (int r = 0; r < vis; ++r) {
    int i = top + r;
    if (i >= totalRows) break;
    int y = y0 + r * rowH;
    bool sel = (i == btDevSel);
    if (sel) {
      L->fillRoundRect(8, y, 304, rowH - 3, 6, C_PANEL2);
      L->fillRoundRect(8, y, 4, rowH - 3, 2, C_ACCENT);
    }
    if (i == btDevCount) {
      text(">> Поиск заново <<", 22, y + 4, sel ? C_ACCENT : C_DIM, F_MD);
    } else {
      String name = fitText(btDevs[i].name, 210, F_MD);
      text(name.c_str(), 22, y + 4, sel ? C_TEXT : C_DIM, F_MD);
      char b[16];
      snprintf(b, sizeof(b), "%d dBm", btDevs[i].rssi);
      text(b, 306, y + 6, sel ? C_ACCENT : C_LINE, F_SM, textdatum_t::top_right);
    }
  }

  // Подсказки
  text("A/C выбор", 12, 218, C_DIM, F_SM);
  text("B выбор", 112, 218, C_DIM, F_SM);
  text("держ. B скан", 200, 218, C_DIM, F_SM);
  needRedraw = false;
}

// -----------------------------------------------------------------------------
//  Снимок экрана по Serial (RGB332)
// -----------------------------------------------------------------------------
static void takeShot() {
  M5Canvas canvas(&M5.Display);
  canvas.setColorDepth(8);
  if (!canvas.createSprite(320, 240)) {
    Serial.println("SHOT FAIL");
    return;
  }
  lgfx::LGFXBase* prev = L;
  L = &canvas;
  drawUI();
  L = prev;

  Serial.printf("SHOT 320 240 8\n");
  Serial.write((const uint8_t*)canvas.getBuffer(), 320 * 240);
  Serial.print("\nEND\n");
  Serial.flush();
  canvas.deleteSprite();
}

// -----------------------------------------------------------------------------
//  Serial команды
// -----------------------------------------------------------------------------
static volatile uint8_t synthClick[3] = {0, 0, 0};
static volatile uint8_t synthHold[3]  = {0, 0, 0};

static void serialTick() {
  static char cmd[32];
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
        else if (!strcasecmp(cmd, "SCAN")) btStartScan();
        else if (!strcasecmp(cmd, "SHOT")) takeShot();
        else if (!strcasecmp(cmd, "LIST")) {
          Serial.printf("BT Devices (%d):\n", btDevCount);
          for (int i = 0; i < btDevCount; ++i)
            Serial.printf("  [%d] %s (%d dBm)\n", i, btDevs[i].name, btDevs[i].rssi);
        }
        else if (!strncasecmp(cmd, "CON ", 4)) {
          btConnect(atoi(cmd + 4));
        }
        else if (!strncasecmp(cmd, "FREQ ", 5)) {
          currentFreq = (float)atof(cmd + 5);
          needRedraw = true;
          Serial.printf("[sine] freq = %.1f Hz\n", currentFreq);
        }
        else if (!strcasecmp(cmd, "INFO")) {
          Serial.printf("INFO state=%d devCount=%d conn=%d heap=%u\n",
                        btState, btDevCount, (int)a2dp.is_connected(), (unsigned)ESP.getFreeHeap());
        }
        else {
          Serial.println("HELP: A B C AH BH CH SCAN LIST CON <i> FREQ <hz> SHOT INFO");
        }
      }
    } else if (len < sizeof(cmd) - 1) {
      cmd[len++] = ch;
    }
  }
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

// -----------------------------------------------------------------------------
//  setup / loop
// -----------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  delay(200);

  Serial.println("\n=== M5Stack Bluetooth Sine Test ===");
  Serial.printf("Board: %d  RAM free: %u bytes\n", (int)M5.getBoard(), (unsigned)ESP.getFreeHeap());

  // Полностью выключаем Wi-Fi, чтобы не отнимал ни байта памяти
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin("Gzone", "");

  delay(100);

  M5.Display.setBrightness(200);
  M5.BtnA.setHoldThresh(500);
  M5.BtnB.setHoldThresh(500);
  M5.BtnC.setHoldThresh(500);

  drawUI();

  // Запуск Bluetooth и поиска
  btStartScan();
}

void loop() {
  M5.update();
  serialTick();

  // Опрос состояния A2DP
  if (btInited) {
    static bool wasConnected = false;
    bool connected = a2dp.is_connected();
    if (connected && !wasConnected) {
      btState = 3; // играет
      needRedraw = true;
      Serial.printf("[bt] Connected! Sine wave playing at %.0f Hz\n", currentFreq);
    }
    if (!connected && wasConnected) {
      btState = 0;
      needRedraw = true;
      Serial.println("[bt] Disconnected.");
    }
    wasConnected = connected;

    if (btState == 2 && millis() - btConnStart > 20000) {
      btState = 4;
      needRedraw = true;
      Serial.println("[bt] Connection timeout.");
    }
  }

  Press a = pollButton(M5.BtnA, 0);
  Press b = pollButton(M5.BtnB, 1);
  Press c = pollButton(M5.BtnC, 2);

  int totalRows = btDevCount + 1;
  if (a.click) {
    btDevSel = (btDevSel + totalRows - 1) % totalRows;
    needRedraw = true;
  }
  if (c.click) {
    btDevSel = (btDevSel + 1) % totalRows;
    needRedraw = true;
  }
  if (b.click) {
    if (btDevSel == btDevCount) {
      btStartScan();
    } else {
      btConnect(btDevSel);
    }
  }
  if (b.hold) {
    btStartScan();
  }
  // Переключение частоты синусоиды удержанием A / C
  if (a.hold) {
    if (currentFreq > 200.0f) currentFreq -= 110.0f;
    needRedraw = true;
  }
  if (c.hold) {
    if (currentFreq < 2000.0f) currentFreq += 110.0f;
    needRedraw = true;
  }

  // Анимация волны при воспроизведении
  static uint32_t lastAnim = 0;
  if (btState == 3 && millis() - lastAnim > 80) {
    lastAnim = millis();
    drawSineWave(12, 78, 296, 36);
  }

  if (needRedraw) {
    drawUI();
  }

  delay(5);
}
