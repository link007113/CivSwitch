#include "BoardLed.h"

#include "pico/stdlib.h"

#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif

namespace {
bool initialized = false;
bool ready = false;
}

void boardLedInit() {
  if (initialized) return;
  initialized = true;
#ifdef CYW43_WL_GPIO_LED_PIN
  // LED failure must not prevent USB macro playback.
  ready = cyw43_arch_init() == 0;
#elif defined(PICO_DEFAULT_LED_PIN)
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  ready = true;
#endif
  boardLedPut(false);
}

void boardLedPut(bool on) {
  if (!ready) return;
#ifdef CYW43_WL_GPIO_LED_PIN
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#elif defined(PICO_DEFAULT_LED_PIN)
  gpio_put(PICO_DEFAULT_LED_PIN, on);
#endif
}
