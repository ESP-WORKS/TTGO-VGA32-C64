#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

extern "C" {
  #include "emuapi.h"        /* MASK_JOY2_* */
  #include "iopins.h"        /* LINK_PIN_RX / LINK_PIN_TX */
}
#include "port_link.h"       /* MUST be included here: keeps these definitions
                                extern "C", otherwise the C callers get an
                                "undefined reference to link_init" at link time */

#define LINK_UART       UART_NUM_2
#define LINK_BAUD       115200
#define LINK_RX_BUFSZ   256

#define LINK_MARKER     0xFF
#define LINK_STX        0x02
#define LINK_ETX        0x03

/* pad byte */
#define LINK_UP         0x01
#define LINK_DOWN       0x02
#define LINK_LEFT       0x04
#define LINK_RIGHT      0x08
#define LINK_BUTTON2    0x10
#define LINK_BUTTON1    0x20

/* sys byte */
#define LINK_START      0x01
#define LINK_PAUSE      0x02
#define LINK_SOFT_RESET 0x04
#define LINK_HARD_RESET 0x08

volatile bool g_menuRequest = false;

static bool     s_ready    = false;
static uint8_t  s_pad      = 0;
static uint8_t  s_sys      = 0;
static bool     s_prevMenu = false;
static uint8_t  s_state    = 0;    /* 0 idle, 1 want pad, 2 want sys */
static uint8_t  s_pending  = 0;

/* link_poll() is called from input_task (core 0) and, while the ROM picker is
   up, from the main loop (core 1). Both can be live at once once a game has
   been launched and the picker re-opened, so the decoder state is guarded. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;


void link_init(void)
{
    uart_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.baud_rate           = LINK_BAUD;
    cfg.data_bits           = UART_DATA_8_BITS;
    cfg.parity              = UART_PARITY_DISABLE;
    cfg.stop_bits           = UART_STOP_BITS_1;
    cfg.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
    cfg.rx_flow_ctrl_thresh = 0;

    if (uart_param_config(LINK_UART, &cfg) != ESP_OK) {
        printf("LINK: uart_param_config failed\n");
        return;
    }
    if (uart_set_pin(LINK_UART, LINK_PIN_TX, LINK_PIN_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        printf("LINK: uart_set_pin failed\n");
        return;
    }
    /* tx buffer 0 => uart_write_bytes blocks until the bytes are queued.
       The game name is 43 bytes max, ~4 ms at 115200. */
    if (uart_driver_install(LINK_UART, LINK_RX_BUFSZ, 0, 0, NULL, 0) != ESP_OK) {
        printf("LINK: uart_driver_install failed\n");
        return;
    }

    s_ready = true;
    printf("LINK: T-Display bridge up on UART2 (RX=%d TX=%d)\n",
           (int)LINK_PIN_RX, (int)LINK_PIN_TX);
}


static void link_feed(uint8_t b)
{
    if (b >= 0x80) {                       /* marker, or resync after a loss */
        s_state = (b == LINK_MARKER) ? 1 : 0;
        return;
    }
    switch (s_state) {
        case 1:
            s_pending = b;
            s_state   = 2;
            break;
        case 2:
            s_pad   = s_pending;
            s_sys   = b;
            s_state = 0;
#if LINK_TRACE
            printf("LINK pad=0x%02X sys=0x%02X\n", s_pad, s_sys);
#endif
            break;
        default:
            break;                         /* stray byte, ignore */
    }
}


/* Never blocks. Called from input_task (core 0) while a game runs and from the
   main loop while the ROM picker is up. */
void link_poll(void)
{
    if (!s_ready) return;

    uint8_t buf[64];
    int n;
    while ((n = uart_read_bytes(LINK_UART, buf, sizeof(buf), 0)) > 0) {
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < n; i++) link_feed(buf[i]);
        portEXIT_CRITICAL(&s_mux);
        if (n < (int)sizeof(buf)) break;
    }

    /* Home/Heart -> open the ROM picker. Edge-triggered; the bit is never
       passed on to the emulator core. */
    bool menu = (s_sys & LINK_SOFT_RESET) != 0;
    if (menu && !s_prevMenu) g_menuRequest = true;
    s_prevMenu = menu;
}


/* Wire bits -> this emulator's mask. MCUME esp64 uses
   RIGHT 0x0001 / LEFT 0x0002 / UP 0x0004 / DOWN 0x0008 / BTN 0x0010,
   which matches none of the wire values, so translate explicitly. */
uint16_t link_get_mask(void)
{
    if (!s_ready) return 0;
    uint16_t m = 0;
    if (s_pad & LINK_UP)      m |= MASK_JOY2_UP;
    if (s_pad & LINK_DOWN)    m |= MASK_JOY2_DOWN;
    if (s_pad & LINK_LEFT)    m |= MASK_JOY2_LEFT;
    if (s_pad & LINK_RIGHT)   m |= MASK_JOY2_RIGHT;
    if (s_pad & LINK_BUTTON1) m |= MASK_JOY2_BTN;
    return m;
}


/* The C64 joystick port has a single fire button, so BUTTON2 has nowhere to go
   on the joystick. Returned as an ASCII code and folded into the emulated
   keyboard (see emu_ReadI2CKeyboard / c64_SetLinkHeldKey). 0 = unused. */
uint16_t link_get_stkey(void)
{
    if (!s_ready) return 0;
#if LINK_BUTTON2_AS_SPACE
    return (s_pad & LINK_BUTTON2) ? 0x20 : 0;   /* 0x20 = SPACE */
#else
    return 0;
#endif
}


void link_send_game_name(const char *path)
{
    if (!s_ready) return;

    char name[41];
    name[0] = '\0';

    if (path && *path) {                   /* strip directory and extension */
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        size_t n = strlen(base);
        const char *dot = strrchr(base, '.');
        if (dot && dot != base) n = (size_t)(dot - base);
        if (n > sizeof(name) - 1) n = sizeof(name) - 1;
        memcpy(name, base, n);
        name[n] = '\0';
    }

    size_t len = strlen(name);
    const uint8_t stx = LINK_STX;
    const uint8_t etx = LINK_ETX;

    for (int i = 0; i < 3; i++) {          /* redundancy instead of retry */
        uart_write_bytes(LINK_UART, (const char *)&stx, 1);
        if (len) uart_write_bytes(LINK_UART, name, len);
        uart_write_bytes(LINK_UART, (const char *)&etx, 1);
        if (i < 2) vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}
