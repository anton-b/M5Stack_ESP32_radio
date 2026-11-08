// Список радиостанций.
// Все потоки — обычный HTTP MP3: ESP32 без PSRAM не тянет HTTPS и тяжёлые кодеки,
// зато MP3 128 кбит/с декодируется программно и стабильно.
#pragma once

struct Station {
  const char* name;
  const char* genre;
  const char* url;
};

static const Station STATIONS[] = {
  // --- Россия ---
  {"Европа Плюс",     "Популярная музыка", "http://ep128.hostingradio.ru:8030/ep128"},
  {"Ретро FM",        "Хиты 70-90-х",      "http://retroserver.streamr.ru:8043/retro256.mp3"},
  {"DFM",             "Танцевальная",      "http://dfm.hostingradio.ru/dfm128.mp3"},
  {"Наше Радио",      "Русский рок",       "http://nashe1.hostingradio.ru:80/nashe-256"},
  {"Монте Карло",     "Лёгкий поп",        "http://montecarlo.hostingradio.ru/montecarlo128.mp3"},
  {"Русское Радио",   "Поп-музыка",        "http://rusradio.hostingradio.ru/rusradio128.mp3"},
  {"Радио Маяк",      "Разговорное",       "http://icecast.vgtrk.cdnvideo.ru/mayakfm_mp3_128kbps"},
  {"Вести FM",        "Новости",           "http://icecast.vgtrk.cdnvideo.ru/vestifm_mp3_128kbps"},
  {"Jazz FM",         "Джаз",              "http://nashe1.hostingradio.ru/jazz-128.mp3"},
  // --- Мир ---
  {"Radio Paradise",  "Eclectic / USA",    "http://stream.radioparadise.com/mp3-128"},
  {"FIP",             "Эклектика / Франция","http://icecast.radiofrance.fr/fip-midfi.mp3"},
  {"KEXP",            "Indie / Seattle",   "http://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
  {"181.FM Beatles",  "Классика рока",     "http://listen.181fm.com/181-beatles_128k.mp3"},
  {"Classic FM",      "Классика / UK",     "http://media-ice.musicradio.com:80/ClassicFMMP3"},
};

static const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);
