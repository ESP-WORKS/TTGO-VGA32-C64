#ifndef PS2KBD_H
#define PS2KBD_H

#include <stdint.h>

// 1 = a tecla vai direto na matriz do C64 (funciona em jogo).
// 0 = caminho antigo via setKey(), um pulso de 20 ms (so' serve para digitar).
// Com 1, a fila ASCII nao e' consumida -- senao cada tecla contaria duas vezes.
#define PS2_HELD_KEYS 1

// Teclado PS/2 da TTGO VGA32 via FabGL (CLK=33, DAT=32).
// Este header NAO inclui fabgl.h de proposito: fabgl.h faz
// "using fabgl::Color;" e nao pode vazar para o resto do emulador.

#ifdef __cplusplus
extern "C" {
#endif

void ps2kbd_begin(void);

// Pinos efetivos em uso pelo teclado PS/2 (default 33/32, ou lidos do
// bootl.rc no SD, quando presentes e diferentes do default).
int ps2kbd_get_clk_pin(void);
int ps2kbd_get_dat_pin(void);

// Devolve o proximo caractere ASCII pressionado, ou 0 se nao houver.
// Consumido por emu_ReadI2CKeyboard(), que o core do C64 ja usa.
int  ps2kbd_read_ascii(void);

// Setas e Enter como MASK_JOY2_*, para o menu (que le emu_ReadKeys e nao o
// teclado). Sem isto o seletor de ROMs nao responde ao PS/2.
unsigned short ps2kbd_get_mask(void);

// Eventos de navegacao (setas/ENTER) acumulados como MASK_JOY2_*, drenados a
// cada chamada. O menu usa isto em vez de ps2kbd_get_mask() para que segurar
// a seta repita no ritmo do auto-repeat, em vez de andar so' 1 por toque.
unsigned short ps2kbd_get_events(void);

// ASCII da tecla segurada agora (0 = nenhuma). Injetada direto na matriz do
// C64, o que faz o teclado funcionar em jogo e nao so' para digitar.
int ps2kbd_get_held_ascii(void);

#ifdef __cplusplus
}
#endif

#endif