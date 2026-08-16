#include <Arduino.h>
#include <fabgl.h>
#include "ps2kbd.h"

// 1 = imprime cada tecla recebida no serial. Use para confirmar se o teclado
// esta chegando antes de procurar problema no mapeamento.
#define PS2_TRACE 0

// Teclado PS/2 da TTGO VGA32 via FabGL (preset KeyboardPort0 = CLK 33 / DAT 32).
//
// Duas saidas, porque o emulador consome teclado de dois jeitos diferentes:
//
//  - ps2kbd_read_ascii(): fila de ASCII, consumida por emu_ReadI2CKeyboard(),
//    que o c64_Input() usa para indexar ascii2scan[]. So' roda com jogo ativo.
//  - ps2kbd_get_mask():   estado das setas e do Enter como MASK_JOY2_*, para
//    o menu funcionar. O seletor de ROMs le emu_ReadKeys(), nao o teclado.
//
// As setas e o Enter alimentam os dois caminhos. Em jogo isso faz o Enter
// contar tambem como fire, o que e' inofensivo: o direcional de verdade vem
// da ponte T-Display.

// Espelha emuapi.h sem inclui-lo (evita puxar o resto do emulador aqui).
#define M_JOY2_RIGHT 0x0001
#define M_JOY2_LEFT  0x0002
#define M_JOY2_UP    0x0004
#define M_JOY2_DOWN  0x0008
#define M_JOY2_BTN   0x0010
#define M_KEY_USER1  0x0020   // dispara a macro LOAD""+RUN no c64_Input()

static fabgl::PS2Controller ps2;
static bool     kbdReady = false;
static uint16_t s_mask   = 0;
static uint8_t  s_held   = 0;   // ASCII da tecla atualmente SEGURADA

#define ASCII_QUEUE_SIZE 16
static uint8_t asciiQ[ASCII_QUEUE_SIZE];
static int     qHead = 0, qTail = 0;

static inline void qPush(uint8_t c) {
  int n = (qHead + 1) % ASCII_QUEUE_SIZE;
  if (n != qTail) { asciiQ[qHead] = c; qHead = n; }   // cheia: descarta
}
static inline int qPop(void) {
  if (qHead == qTail) return 0;
  int c = asciiQ[qTail];
  qTail = (qTail + 1) % ASCII_QUEUE_SIZE;
  return c;
}

static uint16_t maskOf(fabgl::VirtualKey vk) {
  switch (vk) {
    case fabgl::VK_UP:     return M_JOY2_UP;
    case fabgl::VK_DOWN:   return M_JOY2_DOWN;
    case fabgl::VK_LEFT:   return M_JOY2_LEFT;
    case fabgl::VK_RIGHT:  return M_JOY2_RIGHT;
    case fabgl::VK_RETURN: return M_JOY2_BTN;
    // F1 = o antigo botao USER1 (GPIO35) do hardware do MCUME. O c64_Input()
    // ja reage a ele digitando LOAD"" + Enter, esperando 2 s e digitando RUN.
    // O patchLOAD() ve o nome vazio (RAM[0xB7]==0) e usa menuSelection(),
    // ou seja, o arquivo escolhido no menu. Note que so' funciona com .PRG:
    // o patch le 2 bytes de endereco e despeja o resto na RAM.
    case fabgl::VK_F1:     return M_KEY_USER1;
    case fabgl::VK_KP_ENTER:return M_JOY2_BTN;
    default:               return 0;
  }
}

// Drena a fila da FabGL. Chamada pelos dois getters, entao o teclado responde
// tanto no menu quanto em jogo.
static void ps2kbd_poll(void)
{
  if (!kbdReady) return;
  fabgl::Keyboard *kb = ps2.keyboard();
  if (!kb) return;

#if PS2_TRACE
  // Status periodico: prova se o poll esta rodando e o que o teclado reporta.
  static uint32_t lastStat = 0;
  if (millis() - lastStat > 2000) {
    lastStat = millis();
    Serial.printf("[PS2] poll vivo | disponivel=%d vk=%d\n",
                  (int)kb->isKeyboardAvailable(),
                  (int)kb->virtualKeyAvailable());
  }
#endif

  while (kb->virtualKeyAvailable()) {
    bool down = false;
    fabgl::VirtualKey vk = kb->getNextVirtualKey(&down);

#if PS2_TRACE
    Serial.printf("[PS2] vk=%d down=%d ascii=%d\n",
                  (int)vk, (int)down, (int)kb->virtualKeyToASCII(vk));
#endif
    uint16_t m = maskOf(vk);
    if (m) {
      if (down) s_mask |= m; else s_mask &= ~m;
    }
    int c = kb->virtualKeyToASCII(vk);
    {
      // As setas nao tem ASCII (virtualKeyToASCII devolve -1), mas o C64 tem
      // codigos proprios de cursor -- e o ascii2scan[] do c64.cpp os mapeia.
      switch (vk) {
        case fabgl::VK_UP:    c = 145; break;
        case fabgl::VK_DOWN:  c = 17;  break;
        case fabgl::VK_LEFT:  c = 157; break;
        case fabgl::VK_RIGHT: c = 29;  break;
        default: break;
      }

      // ascii2scan[] em c64.cpp so' tem MAIUSCULAS: as linhas de 'a'..'z'
      // sao todas zero. O C64 sem shift produz maiuscula, entao a tabela foi
      // feita assim. Sem esta conversao, letra minuscula vira setKey(0) e
      // nada acontece -- que era o motivo do teclado nao funcionar no BASIC.
      if (c >= 'a' && c <= 'z') c -= 32;

      if (c < 0 || c >= 160) c = 0;   // 160 = tamanho de ascii2scan[]
    }

    if (down) {
      s_held = (uint8_t)c;            // tecla segurada -> vai direto na matriz
      if (c) qPush((uint8_t)c);       // fila ASCII (usada so' se PS2_HELD_KEYS=0)
    } else if ((uint8_t)c == s_held) {
      s_held = 0;
    }
  }
}

void ps2kbd_begin(void)
{
  if (kbdReady) return;
  // CreateVirtualKeysQueue, nao GenerateVirtualKeys: e' este modo que faz a
  // FabGL manter a FILA de teclas. Com GenerateVirtualKeys ela converte o
  // scancode mas nao enfileira nada, entao virtualKeyAvailable() fica sempre
  // falso e getNextVirtualKey() nunca tem o que devolver -- exatamente o
  // sintoma de "teclado detectado mas nenhuma tecla chega".
  ps2.begin(PS2Preset::KeyboardPort0, KbdMode::CreateVirtualKeysQueue);
  fabgl::Keyboard *kb = ps2.keyboard();
  if (kb) {
    kb->setLayout(&fabgl::USLayout);
    kbdReady = true;
  }
  // keyboard() devolve o objeto mesmo sem teclado ligado, entao "iniciado"
  // sozinho nao prova nada. isKeyboardAvailable() diz se o dispositivo
  // respondeu ao reset do protocolo.
  Serial.printf("[PS2] objeto=%s  dispositivo=%s\n",
                kb ? "ok" : "nulo",
                (kb && kb->isKeyboardAvailable()) ? "DETECTADO" : "nao responde");
  Serial.flush();
}

int ps2kbd_read_ascii(void)
{
  ps2kbd_poll();
  return qPop();
}

uint16_t ps2kbd_get_mask(void)
{
  ps2kbd_poll();
  return s_mask;
}


// Tecla atualmente segurada, em ASCII. O c64.cpp injeta isto direto na matriz
// do teclado em cia1PORTA/PORTB. Sem isso so' existe o caminho setKey(), que
// e' um pulso de 20 ms -- suficiente para digitar no BASIC, inutil em jogo,
// onde a tecla precisa ficar pressionada enquanto o dedo estiver nela.
int ps2kbd_get_held_ascii(void)
{
  ps2kbd_poll();
  return s_held;
}