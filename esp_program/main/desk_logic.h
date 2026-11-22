// Pure desk-control decisions, free of Arduino headers so they can be
// unit-tested on a computer (see esp_program/tests/run_tests.sh).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

constexpr float kMinHeightCm = 64.0f;
constexpr float kMaxHeightCm = 114.0f;
constexpr float kDeadBandCm = 0.5f;  // closer than this: don't drive the motor
// A request handled this soon after a move ended arrived while the desk was
// moving (the WebServer queues it); it is stale and must not start a move.
constexpr uint32_t kStaleRequestWindowMs = 250;

enum class Direction { None, Up, Down };

struct MoveResult {
  bool ok;
  float heightCm;     // valid when ok
  const char* error;  // set when !ok
};

inline float clampHeight(float cm) {
  if (cm < kMinHeightCm) return kMinHeightCm;
  if (cm > kMaxHeightCm) return kMaxHeightCm;
  return cm;
}

// Parses a whole string as a finite number. Rejects empty strings, trailing
// garbage ("12abc") and nan/inf, so a bad request never moves the desk.
inline bool parseCm(const char* text, float* out) {
  if (text == nullptr) return false;
  char* end = nullptr;
  float value = std::strtof(text, &end);
  if (end == text || *end != '\0' || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

// Target for a relative move. Never drives against the requested direction:
// if the desk is already outside 64-114 cm, clamping could turn "+1" into a
// move down, so stay put instead.
inline float nudgeTarget(float currentCm, float deltaCm) {
  float target = clampHeight(currentCm + deltaCm);
  if ((deltaCm > 0 && target < currentCm) || (deltaCm < 0 && target > currentCm)) {
    return currentCm;
  }
  return target;
}

// nowMs/lastMoveEndMs are millis() values; unsigned subtraction handles wrap.
inline bool isStaleRequest(uint32_t nowMs, uint32_t lastMoveEndMs, bool hasMoved) {
  return hasMoved && (uint32_t)(nowMs - lastMoveEndMs) < kStaleRequestWindowMs;
}

inline Direction chooseDirection(float currentCm, float targetCm) {
  float diff = targetCm - currentCm;
  if (std::fabs(diff) <= kDeadBandCm) return Direction::None;
  return diff > 0 ? Direction::Up : Direction::Down;
}

inline bool reachedTarget(Direction dir, float currentCm, float targetCm) {
  switch (dir) {
    case Direction::Up:
      return currentCm >= targetCm;
    case Direction::Down:
      return currentCm <= targetCm;
    default:
      return true;
  }
}

inline int formatHeightJson(char* buf, size_t len, float heightCm) {
  return std::snprintf(buf, len, "{\"height\":%.1f}", heightCm);
}

inline int formatErrorJson(char* buf, size_t len, const char* message) {
  return std::snprintf(buf, len, "{\"error\":\"%s\"}", message);
}
