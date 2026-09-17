#include <Arduino.h>
#include <string.h>
#include <fabgl.h>

#include "video_vga.h"
#include "font8x8.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

// ---------------------------------------------------------------------------
// VGADirectController: registramos um callback de scanline e a FabGL o chama
// em ISR para cada linha. Nao ha canvas nem fila de primitivas.
//
// O framebuffer guarda 1 byte por pixel ja no formato RAW da FabGL (RGB222 +
// bits de sync) e ja "swizzled" (indice ^ 2), que e' a ordem que o I2S espera.
// Assim o callback e' um memcpy direto, sem conversao dentro da ISR.
//
// Framebuffer em RAM INTERNA de proposito: o callback roda em ISR e ler PSRAM
// ali e' lento e arriscado. Quem permitiria framebuffer em PSRAM seria o
// VGA16Controller, que converte por scanline -- fica como proximo passo se a
// RAM interna apertar.
// ---------------------------------------------------------------------------

static fabgl::VGADirectController vgaCtrl;

static uint8_t *_fb      = nullptr;   // 320*240, raw FabGL, swizzled
static uint8_t  _rawLUT[64];          // indice RGB222 (6 bits) -> pixel raw
static bool     vgaReady = false;

// Scratch linear onde o VIC-II escreve a linha corrente.
static uint16_t lineScratch[VGA_XRES] __attribute__((aligned(4)));
static int      pendingLine = -1;

// Mapa dx(0..279) -> src_x(0..319) da compressao horizontal, pre-calculado em
// begin(). Antes essa conta era feita por pixel dentro da flushLine:
//   src_x = (dx * VGA_XRES) / VGA_CONTENT_XRES
// 280 divisoes por linha x 15600 linhas/s. Agora e' uma leitura de tabela.
static uint16_t srcMap[VGA_CONTENT_XRES];

// Contadores de desempenho.
static unsigned long      s_frames  = 0;   // quadros do VIC-II
static unsigned long long s_flushUs = 0;   // tempo total convertendo linhas

// Indicador "DISK" estilo Apple ][. Ativado por vga_show_disk() a partir do
// patches.cpp quando um LOAD e' interceptado. Fica visivel ate' 'deadline'
// (millis()). Guardamos o millis do ultimo tick observado para nao chamar
// millis() dentro do IRAM_ATTR (o flushLine gira 15600 vezes/s).
#define DISK_MAX_LEN 24
static char     s_diskMsg[DISK_MAX_LEN + 1] = {0};
static uint32_t s_diskDeadline = 0;
static uint32_t s_diskNow      = 0;   // atualizado uma vez por quadro
// Linha e coluna do texto no framebuffer VGA. 8x8 no canto inferior esquerdo,
// dentro da borda azul lateral (nao competir com o conteudo do C64).
#define DISK_TEXT_Y (VGA_YRES - 12)
#define DISK_TEXT_X 4


void vga_show_disk(const char *msg, int ms)
{
  if (!msg) { s_diskMsg[0] = 0; s_diskDeadline = 0; return; }
  int n = 0;
  while (msg[n] && n < DISK_MAX_LEN) { s_diskMsg[n] = msg[n]; n++; }
  s_diskMsg[n] = 0;
  s_diskDeadline = millis() + (uint32_t)ms;
}


static void IRAM_ATTR drawScanline(void *arg, uint8_t *dest, int scanLine)
{
  memcpy(dest, _fb + scanLine * VGA_XRES, VGA_XRES);
}


// ---------------------------------------------------------------------------
// Converte o scratch para a linha do framebuffer aplicando o swizzle x^2.
// Com compensação horizontal: reduz conteúdo de 320 para 280 pixels (20px bordas laterais).
// ---------------------------------------------------------------------------
static void IRAM_ATTR flushLine(int y)
{
  if (!vgaReady || (unsigned)y >= VGA_YRES) return;

#if VGA_PROFILE
  int64_t t0 = esp_timer_get_time();
#endif

  const uint8_t  *lut = _rawLUT;
  uint8_t *fb_line = _fb + y * VGA_XRES;
  // Cor da borda vem do VIC ($D020). ATENCAO: o VIC monta cpu.vic.palette[]
  // ja em formato RAW de 6 bits (PALETTE = VGA_RGB6 em vic.cpp), NAO RGB565
  // -- entao usamos direto como indice do LUT, sem passar pelo vga_rgb565to6.
  // Antes eu chamava vga_rgb565to6() aqui e a cor saia lixo (borda azul
  // padrao caia para preto, etc).
  uint8_t borderRaw = lut[vic_get_border_color() & 0x3F];

  // Bordas: como a cor e' uniforme, o swizzle x^2 nao importa -- todos os
  // bytes do intervalo recebem o mesmo valor. memset em vez de laco.
  memset(fb_line, borderRaw, VGA_BORDER_WIDTH);
  memset(fb_line + VGA_XRES - VGA_BORDER_WIDTH, borderRaw, VGA_BORDER_WIDTH);

  // Centro: 320 -> 280, quatro pixels por store de 32 bits.
  //
  // O swizzle x^2 embaralha DENTRO de cada grupo de 4 bytes alinhado: os
  // pixels 0,1,2,3 vao para as posicoes 2,3,0,1. Como VGA_BORDER_WIDTH (20)
  // e VGA_CONTENT_XRES (280) sao ambos multiplos de 4, o centro cai exatamente
  // em palavras alinhadas e da' para montar cada uma na ordem certa e gravar
  // de uma vez. ESP32 e' little-endian: byte 0 da palavra = menor endereco.
  {
    uint32_t *fb32 = (uint32_t *)fb_line + (VGA_BORDER_WIDTH / 4);
    const uint16_t *sm = srcMap;
    for (int w = 0; w < VGA_CONTENT_XRES / 4; w++) {
      uint32_t p0 = lut[lineScratch[sm[0]] & 0x3F];
      uint32_t p1 = lut[lineScratch[sm[1]] & 0x3F];
      uint32_t p2 = lut[lineScratch[sm[2]] & 0x3F];
      uint32_t p3 = lut[lineScratch[sm[3]] & 0x3F];
      sm += 4;
      fb32[w] = p2 | (p3 << 8) | (p0 << 16) | (p1 << 24);
    }
  }

  // Overlay do indicador "DISK" -- desenhado por cima da borda inferior. So'
  // afeta as 8 linhas do texto, entao 0 custo fora desse intervalo. Nao
  // chamamos millis() aqui: usamos o s_diskNow atualizado uma vez por quadro
  // em getLineBuffer(0).
  if (s_diskMsg[0] && s_diskNow < s_diskDeadline &&
      y >= DISK_TEXT_Y && y < DISK_TEXT_Y + 8) {
    int row = y - DISK_TEXT_Y;
    // Amarelo brilhante sobre preto -- alto contraste, estilo Apple ][.
    uint8_t fg = lut[vga_rgb565to6(RGBVAL16(0xFF, 0xFF, 0x00)) & 0x3F];
    uint8_t bg = lut[vga_rgb565to6(RGBVAL16(0x00, 0x00, 0x00)) & 0x3F];
    int px = DISK_TEXT_X;
    for (int i = 0; s_diskMsg[i] && px + 8 <= VGA_XRES; i++) {
      unsigned char ch = (unsigned char)s_diskMsg[i];
      if (ch >= 128) ch = '?';
      unsigned char bits = font8x8[ch][row];
      for (int col = 0; col < 8; col++) {
        int x = px + col;
        fb_line[(x ^ 2)] = (bits & (1 << col)) ? fg : bg;
      }
      px += 8;
    }
  }

#if VGA_PROFILE
  s_flushUs += (unsigned long long)(esp_timer_get_time() - t0);
#endif
}


void vga_get_stats(unsigned long *frames, unsigned long long *flush_us)
{
  if (frames)   { *frames   = s_frames;  s_frames  = 0; }
  if (flush_us) { *flush_us = s_flushUs; s_flushUs = 0; }
}

static inline void putPixel(int x, int y, uint8_t c6)
{
  if ((unsigned)x >= VGA_XRES || (unsigned)y >= VGA_YRES || !vgaReady) return;
  _fb[y * VGA_XRES + (x ^ 2)] = _rawLUT[c6 & 0x3F];
}


void VGA_Video::begin(void)
{
  if (vgaReady) return;

  _fb = (uint8_t *)heap_caps_malloc(VGA_XRES * VGA_YRES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!_fb) {
    Serial.println("[VGA] ERRO: sem RAM interna para o framebuffer");
    Serial.flush();
    return;
  }
  Serial.printf("[VGA] %dx%d, framebuffer %d bytes em %p\n",
                VGA_XRES, VGA_YRES, VGA_XRES * VGA_YRES, _fb);
  Serial.flush();

  vgaCtrl.begin();                     // pinos padrao da TTGO VGA32
  vgaCtrl.setDrawScanlineCallback(drawScanline);
#if VGA_MODE_320x200
  vgaCtrl.setResolution(VGA_320x200_70Hz);
#else
  vgaCtrl.setResolution(QVGA_320x240_60Hz);
#endif

  for (int i = 0; i < 64; i++)
    _rawLUT[i] = vgaCtrl.createRawPixel(RGB222((i >> 4) & 3, (i >> 2) & 3, i & 3));

  // Fundo inicial todo preto. As bordas laterais viram a cor do VIC ($D020)
  // na primeira scanline convertida -- nao adianta pintar aqui, ainda nem
  // resetamos o VIC. Antes tinhamos borda azul fixa no init.
  memset(_fb, _rawLUT[0], VGA_XRES * VGA_YRES);

  memset(lineScratch, 0, sizeof(lineScratch));

  // Mapa da compressao horizontal, calculado uma vez.
  for (int dx = 0; dx < VGA_CONTENT_XRES; dx++) {
    int src_x = (dx * VGA_XRES) / VGA_CONTENT_XRES;
    if (src_x >= VGA_XRES) src_x = VGA_XRES - 1;
    srcMap[dx] = (uint16_t)src_x;
  }

  vgaReady = true;

  Serial.printf("[VGA] pronto. Compensacao horizontal: 320->280px, borda do VIC ($D020).\n");
  Serial.printf("[VGA] heap interno livre=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  Serial.flush();
}

void VGA_Video::start(void) {}
void VGA_Video::stop(void)  {}

void VGA_Video::refresh(void)
{
  // A tela le o framebuffer continuamente: nada a trocar. Mas a ultima linha
  // do quadro ainda pode estar no scratch.
  if (pendingLine >= 0) { flushLine(pendingLine); pendingLine = -1; }
}

void VGA_Video::refreshPrepare(void) {}
void VGA_Video::refreshFinish(void)  { refresh(); }


uint16_t * VGA_Video::getLineBuffer(int j)
{
  // O VIC-II percorre as linhas em ordem; voltar para 0 marca fim de quadro.
  if (j == 0) { s_frames++; s_diskNow = millis(); }

  if (pendingLine >= 0 && pendingLine != j) flushLine(pendingLine);
  pendingLine = j;
  return lineScratch;
}


void VGA_Video::fillScreen(uint16_t color)
{
  if (!vgaReady) return;
  memset(_fb, _rawLUT[vga_rgb565to6(color) & 0x3F], VGA_XRES * VGA_YRES);
}


void VGA_Video::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
  uint8_t c6 = vga_rgb565to6(color);
  for (int16_t j = 0; j < h; j++)
    for (int16_t i = 0; i < w; i++)
      putPixel(x + i, y + j, c6);
}


void VGA_Video::drawSprite(int16_t x, int16_t y, const uint16_t *bitmap)
{
  drawSprite(x, y, bitmap, 0, 0, 0, 0);
}

// bitmap[0] = largura, bitmap[1] = altura, depois os pixels RGB565.
void VGA_Video::drawSprite(int16_t x, int16_t y, const uint16_t *bitmap,
                           uint16_t arx, uint16_t ary, uint16_t arw, uint16_t arh)
{
  if (!bitmap) return;
  int bw = bitmap[0];
  int bh = bitmap[1];
  if (arw == 0) { arx = 0; ary = 0; arw = bw; arh = bh; }

  const uint16_t *src = bitmap + 2;
  for (int j = 0; j < (int)arh; j++) {
    int sy = ary + j;
    if (sy >= bh) break;
    for (int i = 0; i < (int)arw; i++) {
      int sx = arx + i;
      if (sx >= bw) break;
      putPixel(x + sx, y + sy, vga_rgb565to6(src[sy * bw + sx]));
    }
  }
}


void VGA_Video::drawText(int16_t x, int16_t y, const char * text,
                         uint16_t fgcolor, uint16_t bgcolor, bool doublesize)
{
  if (!text) return;
  uint8_t fg = vga_rgb565to6(fgcolor);
  uint8_t bg = vga_rgb565to6(bgcolor);

  // ATENCAO: no MCUME "doublesize" dobra SO' A ALTURA. O caractere e' 8x16,
  // nao 16x16, e o avanco horizontal e' sempre 8 (ver drawText original do
  // ILI9341_t3DMA: "x += 8" fora do if). O layout do menu depende disso:
  // TEXT_WIDTH=8 / TEXT_HEIGHT=16, e o TITLE tem 40 chars = 320 px exatos.
  for (const char *p = text; *p; p++, x += 8) {
    unsigned char ch = (unsigned char)*p;
    if (ch >= 128) ch = 0;
    const unsigned char *glyph = &font8x8[ch][0];

    int l = y;
    for (int row = 0; row < 8; row++) {
      unsigned char bits = glyph[row];
      if (doublesize) {
        for (int col = 0; col < 8; col++)
          putPixel(x + col, l, (bits & (1 << col)) ? fg : bg);
        l++;
      }
      for (int col = 0; col < 8; col++)
        putPixel(x + col, l, (bits & (1 << col)) ? fg : bg);
      l++;
    }
  }
}


// Caminho generico do MCUME. O core do C64 nao usa (escreve via getLineBuffer).
void VGA_Video::writeLine(int width, int height, int line,
                          uint8_t *buffer, uint16_t *palette16)
{
  if (!vgaReady || (unsigned)line >= VGA_YRES) return;
  int n = (width > VGA_XRES) ? VGA_XRES : width;
  for (int x = 0; x < n; x++) {
    uint16_t c = palette16 ? palette16[buffer[x]] : (uint16_t)buffer[x];
    putPixel(x, line, vga_rgb565to6(c));
  }
}

void VGA_Video::writeScreen(int width, int height, int stride,
                            uint8_t *buffer, uint16_t *palette16)
{
  for (int y = 0; y < height; y++)
    writeLine(width, height, y, buffer + y * stride, palette16);
}