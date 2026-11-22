// Host-side unit tests for desk_logic.h. Run with ./run_tests.sh
#include <cstdio>
#include <cstring>

#include "desk_logic.h"

static int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      ++failures;                                                      \
    }                                                                  \
  } while (0)

int main() {
  // clampHeight
  CHECK(clampHeight(50) == 64);
  CHECK(clampHeight(64) == 64);
  CHECK(clampHeight(90) == 90);
  CHECK(clampHeight(200) == 114);

  // parseCm
  float v = 0;
  CHECK(parseCm("90", &v) && v == 90.0f);
  CHECK(parseCm("-1", &v) && v == -1.0f);
  CHECK(parseCm("1.5", &v) && v == 1.5f);
  CHECK(parseCm(" 1", &v) && v == 1.0f);  // '+' arrives URL-decoded as space
  CHECK(!parseCm("abc", &v));
  CHECK(!parseCm("", &v));
  CHECK(!parseCm("12abc", &v));
  CHECK(!parseCm(nullptr, &v));
  CHECK(!parseCm("nan", &v));
  CHECK(!parseCm("inf", &v));

  // chooseDirection
  CHECK(chooseDirection(80, 90) == Direction::Up);
  CHECK(chooseDirection(90, 80) == Direction::Down);
  CHECK(chooseDirection(90, 90) == Direction::None);
  CHECK(chooseDirection(90, 90.5f) == Direction::None);
  CHECK(chooseDirection(90, 89.5f) == Direction::None);
  CHECK(chooseDirection(90, 90.6f) == Direction::Up);

  // reachedTarget
  CHECK(!reachedTarget(Direction::Up, 89, 90));
  CHECK(reachedTarget(Direction::Up, 90, 90));
  CHECK(!reachedTarget(Direction::Down, 91, 90));
  CHECK(reachedTarget(Direction::Down, 90, 90));
  CHECK(reachedTarget(Direction::None, 50, 90));

  // nudge composition stays within limits
  CHECK(clampHeight(113.5f + 1) == 114);
  CHECK(clampHeight(64.2f - 1) == 64);

  // nudgeTarget: never moves against the requested direction
  CHECK(nudgeTarget(90, 1) == 91);
  CHECK(nudgeTarget(113.5f, 1) == 114);
  CHECK(nudgeTarget(116, 1) == 116);   // above range: +1 must not drive down
  CHECK(nudgeTarget(62, -1) == 62);    // below range: -1 must not drive up
  CHECK(nudgeTarget(116, -1) == 114);  // down, back into range: fine
  CHECK(nudgeTarget(62, 1) == 64);     // up, back into range: fine

  // isStaleRequest: requests queued during a move are rejected
  CHECK(isStaleRequest(1100, 1000, true));
  CHECK(!isStaleRequest(1300, 1000, true));
  CHECK(!isStaleRequest(100, 0, false));          // nothing moved yet
  CHECK(isStaleRequest(5, 0xFFFFFFFFUL - 10, true));  // millis() wraparound

  // JSON
  char b[64];
  formatHeightJson(b, sizeof b, 90.14f);
  CHECK(std::strcmp(b, "{\"height\":90.1}") == 0);
  formatErrorJson(b, sizeof b, "sensor lost");
  CHECK(std::strcmp(b, "{\"error\":\"sensor lost\"}") == 0);

  if (failures == 0) {
    std::printf("All tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
