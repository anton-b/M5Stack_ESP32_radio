// AudioOutputBT.h
// Вывод PCM в Bluetooth A2DP (наушники/колонка).
//
// Декодер MP3 выдаёт стерео с частотой 44100/48000/22050 и т.п. A2DP source
// ждёт стерео 16 бит 44100 Гц — приводим к ней линейной интерполяцией и
// складываем в кольцевой буфер. Из буфера читает задача A2DP.
#pragma once

#include <AudioOutput.h>
#include <cstdint>
#include <cstring>

class AudioOutputBT : public AudioOutput {
public:
  // ~23 мс стерео-потока 44100 Гц (экономит RAM под Wi-Fi + BT)
  static constexpr size_t RING = 4096;

  AudioOutputBT() { hertz = 44100; }

  bool begin() override { hertz = 44100; return true; }

  void setVolume(int vol) { _gain = vol / 255.0f; }

  bool ConsumeSample(int16_t sample[2]) override {
    if (hertz && hertz != 44100) {
      if (hertz != _lastRate) {
        _lastRate = hertz;
        _phase = 0.0f;
        _prevL = sample[0];
        _prevR = sample[1];
      }
      resampleFrame(sample[0], sample[1]);
    } else {
      push(sample[0], sample[1]);
    }
    return true;
  }

  void flush() override {}
  bool stop() override { clear(); return true; }

  // Вызывается из задачи A2DP: всегда возвращает len байт (нулями в конце).
  int32_t read(uint8_t* dst, int32_t len) {
    int32_t n = 0;
    while (n < len) {
      if (used() == 0) break;
      dst[n++] = _ring[_r];
      _r = (_r + 1) % RING;
    }
    if (n < len) memset(dst + n, 0, len - n);
    return len;
  }

  void clear() {
    _r = _w = 0;
    _phase = 0.0f;
    _prevL = _prevR = 0;
  }

private:
  uint8_t _ring[RING];
  volatile size_t _w = 0, _r = 0;
  float _gain = 1.0f;

  int32_t _prevL = 0, _prevR = 0;
  float _phase = 0.0f;
  int32_t _lastRate = 44100;

  inline size_t used() const { return (_w - _r + RING) % RING; }
  inline size_t freeSpace() const { return (_r - _w - 1 + RING) % RING; }

  inline void push(int16_t l, int16_t r) {
    if (freeSpace() < 4) {
      // Не блокируем декодер: выбрасываем самые старые 4 байта.
      _r = (_r + 4) % RING;
    }
    int16_t vl = (int16_t)(l * _gain);
    int16_t vr = (int16_t)(r * _gain);
    _ring[_w] = (uint8_t)(vl & 0xFF);        _w = (_w + 1) % RING;
    _ring[_w] = (uint8_t)((vl >> 8) & 0xFF); _w = (_w + 1) % RING;
    _ring[_w] = (uint8_t)(vr & 0xFF);        _w = (_w + 1) % RING;
    _ring[_w] = (uint8_t)((vr >> 8) & 0xFF); _w = (_w + 1) % RING;
  }

  void resampleFrame(int16_t l, int16_t r) {
    // k — сколько выходных отсчётов нужно на один входной.
    const float k = 44100.0f / (float)hertz;
    const float step = 1.0f / k;
    while (_phase < 1.0f) {
      int32_t ol = _prevL + (int32_t)((l - _prevL) * _phase);
      int32_t orr = _prevR + (int32_t)((r - _prevR) * _phase);
      push((int16_t)ol, (int16_t)orr);
      _phase += step;
    }
    _phase -= 1.0f;
    _prevL = l;
    _prevR = r;
  }
};
