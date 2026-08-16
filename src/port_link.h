#ifndef PORT_LINK_H
#define PORT_LINK_H

/*
 * TTGO T-Display controller bridge - MCUME esp64 (C64) port.
 *
 * Wire protocol and rationale: see TTGO-T-DISPLAY-Implement.md.
 * The T-Display firmware is used unmodified; everything here is the
 * receiving side.
 *
 * NOTE: this project is plain ESP-IDF (no Arduino core), so the reference
 * implementation's Serial2 is replaced by the IDF UART driver on UART_NUM_2.
 */

#include <stdint.h>
#include <stdbool.h>

/* 1 = print every decoded pad/sys pair, to identify a controller's buttons. */
#define LINK_TRACE 0

/* 1 = LINK_BUTTON2 (B/Y) acts as a held SPACE on the emulated C64 keyboard.
   0 = LINK_BUTTON2 is discarded (the C64 joystick has a single fire button). */
#define LINK_BUTTON2_AS_SPACE 1

#ifdef __cplusplus
extern "C" {
#endif

/* Set by link_poll() when the Home/Heart button goes down.
   Consumed (and cleared) by the main loop in go.cpp - never act on it from
   inside an input handler. */
extern volatile bool g_menuRequest;

void     link_init(void);                       /* UART2 up; once at boot     */
void     link_poll(void);                       /* drain RX; never blocks     */
uint16_t link_get_mask(void);                   /* controller -> MASK_JOY2_*  */
uint16_t link_get_stkey(void);                  /* BUTTON2 -> ASCII, or 0     */
void     link_send_game_name(const char *path); /* "" or NULL clears it       */

#ifdef __cplusplus
}
#endif

#endif
