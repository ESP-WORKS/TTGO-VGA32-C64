# esp64 C64 -> TTGO VGA32

Vídeo trocado de ILI9341 para VGA bare-metal (`vga_6bit`, I2S1, zero-cópia).
Framework migrado de ESP-IDF 5.5 para Arduino.

## Como o vídeo funciona

O VIC-II escreve os 320 pixels da linha em `tft.getLineBuffer(y)` (vic.cpp:1392).
Isso não mudou. O que mudou:

- `PALETTE()` agora gera o byte de 6 bits do VGA (`VGA_RGB6`) em vez de RGB565
  byte-swapped para SPI. `tpixel` continua `uint16_t`, então **nenhum** dos
  modos gráficos mode0..mode7 nem os sprites precisaram ser tocados.
- `getLineBuffer()` devolve um scratch linear. O framebuffer do vga_6bit tem
  os bytes fora de ordem (pixel x mora em `[x^2]`), e o VIC percorre a linha
  com `*p++` — os dois não se conversam direto.
- A linha é convertida quando a próxima é pedida, montando palavras de 32 bits:
  1 store por 4 pixels. O autor do vga_6bit avisa que escrita byte a byte no
  ESP32 é lenta.

Geometria bate exata: o VIC já produz 320x240 (200 + 2x20 de borda). Sem escala.

## PSRAM

O framebuffer **não pode** ficar em PSRAM: o DMA do I2S no ESP32 só lê DRAM
interna. São 76800 bytes com `MALLOC_CAP_DMA`, buffer único. Duplo buffer
custaria 153600 e não sobra junto com os 64 KB de RAM do C64 e as ROMs.
PSRAM está habilitada (`-DBOARD_HAS_PSRAM`) para o resto.

## Pinos (TTGO VGA32 v1.4)

| Função | GPIO |
|---|---|
| VGA R1/R0 | 22 / 21 |
| VGA G1/G0 | 19 / 18 |
| VGA B1/B0 | 5 / 4 |
| HSync / VSync | 23 / 15 |
| SD CS/CLK/MISO/MOSI | 13 / 14 / 2 / 12 |
| Link T-Display RX/TX | 34 / 26 |

GPIO 4 e 5 eram o teclado I2C do hardware original — `HAS_I2CKBD` foi
desligado. GPIO 15 era o CS do SD. Os pinos do SD foram para os da VGA32.

## Entrada

Sem touch (a VGA32 não tem) e sem teclado I2C. Os métodos de touch viraram
stub em `video_vga.h`. A navegação do menu depende do gamepad da T-Display
pela ponte UART que já estava pronta.

## Não verificado

Nada disto foi compilado nem testado. Os pontos mais prováveis de quebrar,
em ordem: o áudio (`AudioPlaySystem.cpp` usa `dac_continuous`, que só existe
na IDF 5.x — no Arduino/IDF 4.4 precisa voltar para `i2s_set_dac_mode`), o
mount do SD, e a ordem do `vga_pins[]`.
