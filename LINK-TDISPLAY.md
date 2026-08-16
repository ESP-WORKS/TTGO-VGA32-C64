# Ponte T-Display no MCUME esp64 (C64)

Integração do bridge de controle Bluetooth da TTGO T-Display neste emulador,
seguindo `TTGO-T-DISPLAY-Implement.md`. O firmware da T-Display **não muda**.

---

## Ligação

| Sinal | T-Display | esp64 |
|---|---|---|
| Estado do controle (TX → RX) | GPIO 27 | **GPIO 34** |
| Nome do jogo (RX ← TX) | GPIO 26 | **GPIO 26** |
| Terra | GND | GND |

115200 8N1, cruzado, terra em comum.

---

## Desvios em relação ao manual

O manual descreve um projeto Arduino rodando numa TTGO VGA32 com FabGL. Este
`esp64` é o build **ILI9341/ESP-IDF** do MCUME, então três coisas mudaram:

**1. Sem `Serial2`.** Este projeto é ESP-IDF puro (make legado, `esp_event_loop.h`,
`i2s_write_bytes` — IDF 3.x). Não existe core Arduino nem `HardwareSerial`.
`port_link.cpp` usa `driver/uart.h` em `UART_NUM_2`, com `uart_set_pin()`
remapeando para 34/26. Os pinos default da UART2 (16/17) ficam livres — importante,
porque **GPIO17 é o CS do TFT** neste projeto.

**2. GPIO 34 estava ocupado.** Aqui não é PS/2 mouse: `iopins.h` define
`PIN_KEY_USER2 = 34`. Com `HAS_TDISPLAY_LINK` ligado, USER2 deixa de ser lido e
deixa de ser configurado como GPIO de entrada. Consequências:

- O botão físico de *swap* de joysticks (USER2) some. O swap continua acessível
  pelo ícone de joystick no menu e por USER1.
- O combo de reboot USER1+USER2 no `input_task` nunca dispara. O reboot por
  **USER4 (GPIO36)** continua funcionando.

**3. GPIO 26 estava ocupado pelo áudio.** `AudioPlaySystem.cpp` chamava
`i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN)`, que é DAC2 = **GPIO26** — colisão
direta com o TX do link. Trocado para `I2S_DAC_CHANNEL_RIGHT_EN` (DAC1 = GPIO25).
O mixer já duplica o mono em L e R, então o áudio é idêntico; só muda o pino.
GPIO25 é, aliás, o pino de áudio padrão da VGA32.

---

## Tradução dos bits

Os valores do fio são constantes do SMS Plus. As máscaras deste emulador são as
mesmas citadas no §6.1 do manual como exemplo de "não batem com nada":

```
MASK_JOY2_RIGHT 0x0001  LEFT 0x0002  UP 0x0004  DOWN 0x0008  BTN 0x0010
```

A tradução é explícita em `link_get_mask()`.

### BUTTON2 (B/Y)

O joystick do C64 tem **um** botão de fogo, então BUTTON2 não cabe no
joystick. Foi mapeado para **ESPAÇO** no teclado emulado
(`LINK_BUTTON2_AS_SPACE`, em `port_link.h` — coloque 0 para descartar).

Detalhe importante: `setKey()` do core é um *pulso* de 20 ms e não consegue
representar uma tecla **segurada**. Por isso o ESPAÇO é injetado direto na
matriz em `cia1PORTA()` / `cia1PORTB()`, em paralelo com `kbdData`, via
`link_heldScancode()`. Assim o jogo enxerga a tecla enquanto o botão estiver
pressionado.

---

## Os quatro call sites

| Onde | O quê |
|---|---|
| `go.cpp` `setup()` | `link_init()` antes do `tft.begin()` e do mount do SD |
| `go.cpp` `input_task()` | `link_poll()` no topo do laço (core 0) |
| `go.cpp` `main_step()` | `link_poll()` no ramo do menu — sem isso o controle parece morto no seletor |
| `emuapi.cpp` `emu_ReadKeys()` | `j1 \|= link_get_mask()`, OR-merge antes da lógica de swap |
| `go.cpp` `ACTION_RUNTFT` | `link_send_game_name(filename)` antes de `emu_Init()` |
| `emuapi.cpp` `toggleMenu(true)` | `link_send_game_name("")` para limpar o "now playing" |

O OR entrar em `j1` *antes* do bloco de swap faz o gamepad respeitar o SWAP=0/1
do menu de graça.

---

## Botão Home/Heart

`LINK_SOFT_RESET` levanta `g_menuRequest` (flag `volatile`), consumida no
`main_step()` do laço principal — nunca dentro do handler de input, conforme o
§6.3. A ação é reabrir o seletor de ROMs, não resetar o ESP32.

Dois cuidados que isso exigiu:

- `input_task` agora só é criada **uma vez** (`inputTaskStarted`). Sem isso,
  voltar ao menu e escolher outro jogo criaria uma segunda task de input.
- `g_menuRequest` é zerada logo depois de `emu_Init()`, para um botão segurado
  durante o load não abrir o menu no instante em que o jogo começa.

Como o `input_task` continua viva enquanto o seletor está aberto, `link_poll()`
pode rodar nos dois cores ao mesmo tempo. O decodificador de bytes está
protegido por `portMUX` em `port_link.cpp`.

---

## Bring-up

1. Confirme a fiação cruzada e o terra comum.
2. `LINK_TRACE 1` em `port_link.h`, recompile, e pressione cada botão olhando o
   serial: `LINK pad=0x20 sys=0x00` = A/X, `pad=0x10` = B/Y, `sys=0x04` = Home.
3. Volte `LINK_TRACE` para 0.

Se nada aparecer: TX/RX não cruzados ou sem terra. Se funcionar no jogo mas não
no seletor: o `link_poll()` do ramo do menu sumiu. Se as direções saírem
trocadas: a tradução em `link_get_mask()` foi alterada.

Para desligar tudo e voltar ao comportamento original, basta comentar
`#define HAS_TDISPLAY_LINK 1` em `emuapi.h` — o USER2 volta ao GPIO34. O áudio
continua no DAC1; reverta `AudioPlaySystem.cpp` se quiser o GPIO26 de volta.
