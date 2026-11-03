#ifndef BOARD_LED_H
#define BOARD_LED_H

// Pico/Pico 2 use a GPIO LED; wireless boards use the CYW43 LED.
void boardLedInit();
void boardLedPut(bool on);

#endif
