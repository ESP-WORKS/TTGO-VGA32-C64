#ifndef VIDEO_VGA_H
#define VIDEO_VGA_H

// -------------------------------------------------------------------------
// Camada de video VGA para TTGO VGA32, substituindo o ILI9341_t3DMA.
//
// A API publica e' identica a' do antigo driver ILI9341 de proposito: emuapi.cpp faz
// ~40 chamadas de desenho e nao precisou ser reescrito. Metodos de touch
// viraram stub (a VGA32 nao tem touch; a navegacao vai pelo gamepad da
// T-Display).
//
// Formato do framebuffer (vga_6bit / bitluni):
//   1 byte por pixel = [VSync][HSync][R1 R0 G1 G0 B1 B0]
//   Os bytes de uma linha ficam FORA DE ORDEM: o pixel x mora em [x^2].
//
// Por causa desse swizzle o VIC-II nao pode escrever direto no framebuffer
// (ele percorre a linha com *p++). Entao getLineBuffer() devolve um scratch
// linear e a linha e' convertida quando a proxima e' pedida -- montando
// palavras de 32 bits (4 pixels por store), como o autor do vga_6bit
// recomenda.
// -------------------------------------------------------------------------

#include <stdint.h>

// 1 = 320x200 (padrao). O C64 so' desenha 200 linhas uteis: as 20 de borda em
//     cima e embaixo eram decorativas. Cortar economiza 17% da conversao de
//     linha E 17% dos memcpy de scanline na ISR da FabGL, alem de 12,8 KB de
//     RAM interna. O BORDER do vic.cpp vira 0 sozinho, porque e' calculado a
//     partir de VGA_YRES.
//     Bonus: o pixel do modo 320x200 nao e' quadrado (0,833), o que compensa
//     em parte o esticao horizontal de monitores widescreen.
// 0 = 320x240 com as bordas.
#define VGA_MODE_320x200 0

#define VGA_XRES   320
#if VGA_MODE_320x200
#define VGA_YRES   200
#else
#define VGA_YRES   240
#endif

// Mantido: emuapi.cpp e vic_palette.h usam RGBVAL16 como tipo de cor.
#define RGBVAL16(r,g,b)  ( (((r>>3)&0x1f)<<11) | (((g>>2)&0x3f)<<5) | (((b>>3)&0x1f)<<0) )

// RGB888 -> 6 bits (RRGGBB), sem os bits de sync.
#define VGA_RGB6(r,g,b)  ( ((((r)>>6)&3)<<4) | ((((g)>>6)&3)<<2) | (((b)>>6)&3) )

// RGB565 -> 6 bits. Usado onde o codigo do menu ja passa RGBVAL16.
static inline uint8_t vga_rgb565to6(uint16_t c) {
  return (uint8_t)( (((c >> 14) & 3) << 4) | (((c >> 9) & 3) << 2) | ((c >> 3) & 3) );
}

class VGA_Video
{
  public:
    VGA_Video(void) {}

    void begin(void);
    void start(void);
    void stop(void);
    void refresh(void);
    void refreshPrepare(void);
    void refreshFinish(void);
    void flipscreen(bool flip) { flipped = flip; }
    bool isflipped(void)       { return flipped; }

    // O VIC-II escreve aqui. Devolve scratch linear de VGA_XRES uint16_t.
    uint16_t * getLineBuffer(int j);

    void fillScreen(uint16_t color);
    void writeScreen(int width, int height, int stride, uint8_t *buffer, uint16_t *palette16);
    void writeLine(int width, int height, int line, uint8_t *buffer, uint16_t *palette16);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawSprite(int16_t x, int16_t y, const uint16_t *bitmap);
    void drawSprite(int16_t x, int16_t y, const uint16_t *bitmap,
                    uint16_t croparx, uint16_t cropary, uint16_t croparw, uint16_t croparh);
    void drawText(int16_t x, int16_t y, const char * text,
                  uint16_t fgcolor, uint16_t bgcolor, bool doublesize);

    void fillScreenNoDma(uint16_t color) { fillScreen(color); }
    void drawSpriteNoDma(int16_t x, int16_t y, const uint16_t *bitmap) { drawSprite(x,y,bitmap); }
    void drawSpriteNoDma(int16_t x, int16_t y, const uint16_t *bitmap,
                         uint16_t croparx, uint16_t cropary, uint16_t croparw, uint16_t croparh)
                         { drawSprite(x,y,bitmap,croparx,cropary,croparw,croparh); }
    void drawRectNoDma(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
                         { drawRect(x,y,w,h,color); }
    void drawTextNoDma(int16_t x, int16_t y, const char * text,
                       uint16_t fgcolor, uint16_t bgcolor, bool doublesize)
                         { drawText(x,y,text,fgcolor,bgcolor,doublesize); }

    // --- Touch: a VGA32 nao tem. Stubs para nao mexer no emuapi.cpp. ---
    void touchBegin(void) {}
    bool isTouching(void) { return false; }
    void readRo (uint16_t *x, uint16_t *y, uint16_t *z) { *x=0; *y=0; *z=0; }
    void readRaw(uint16_t *x, uint16_t *y, uint16_t *z) { *x=0; *y=0; *z=0; }
    void readCal(uint16_t *x, uint16_t *y, uint16_t *z) { *x=0; *y=0; *z=0; }
    void callibrateTouch(uint16_t xMin, uint16_t yMin, uint16_t xMax, uint16_t yMax) {}

  protected:
    bool flipped = false;
};

// --- Instrumentacao de desempenho ---------------------------------------
// VGA_PROFILE=1 mede o tempo gasto convertendo linhas. Custa duas leituras de
// esp_timer por linha (15600/s), entao desligue quando nao estiver medindo.
// Agora em 0: as duas leituras de esp_timer por linha eram 31200 chamadas por
// segundo e ja pesavam mais que o que mediam. Ponha 1 para voltar a medir
// "conv" -- com 0 ele aparece como 0.0%, mas linhas/s e quadros/s continuam.
#define VGA_PROFILE 0

#ifdef __cplusplus
extern "C" {
#endif
// Devolve e ZERA os contadores acumulados desde a ultima chamada.
void vga_get_stats(unsigned long *frames, unsigned long long *flush_us);
#ifdef __cplusplus
}
#endif

#endif // VIDEO_VGA_H