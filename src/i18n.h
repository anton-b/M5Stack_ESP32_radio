// i18n.h — Двуязычная локализация (RU / EN)
#pragma once
#include <cstdint>

enum Lang : uint8_t {
  LANG_RU = 0,
  LANG_EN = 1,
  LANG_COUNT = 2
};

extern int currentLang;

// Макрос / инлайн-хелпер выбора строки по активному языку
inline const char* TR(const char* ru, const char* en) {
  return (currentLang == LANG_EN) ? en : ru;
}
