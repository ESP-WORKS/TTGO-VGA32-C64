#include <Arduino.h>
#include <fabgl.h>
#include "ps2kbd.h"

// 1 = imprime cada tecla recebida no serial. Use para confirmar se o teclado
// esta chegando antes de procurar problema no mapeamento.
#define PS2_TRACE 1   // TEMPORARIO: veja no serial o vk/ascii de cada tecla; volte a 0 depois

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
#define M_KEY_MENU   0x4000   // F6: reabre o menu durante o jogo
#define M_KEY_RESET  0x8000   // F5: reseta o C64 emulado

static fabgl::PS2Controller ps2;
static bool     kbdReady = false;
static uint16_t s_mask   = 0;   // ESTADO das teclas (nivel): para o jogo
static uint16_t s_events = 0;   // EVENTOS 'down' acumulados: para o menu
static uint8_t  s_held   = 0;   // ASCII da tecla atualmente SEGURADA

// Modo joystick, alternado por F2. Ligado: Q/A/O/P/SPACE viram joystick
// (layout Sinclair classico) e sao SUPRIMIDAS do caminho de teclado --
// nao entram em s_held nem na fila ASCII, senao no jogo elas mandariam os
// dois sinais ao mesmo tempo. Desligado: as mesmas teclas digitam normal.
// Existe porque Q/A/O/P sao letras -- sem o toggle, digitar no BASIC viraria
// comando de joystick.
static bool s_joyMode = false;

// Por que dois acumuladores:
//   - O jogo quer NIVEL: seta segurada = direcao mantida. Isso e' s_mask.
//   - O menu quer EVENTO: cada pressionar = um passo, e segurar deve repetir
//     no ritmo do auto-repeat do teclado. Isso e' s_events, que junta cada
//     'down' (inclusive os repeats que a FabGL gera) e e' esvaziado por
//     ps2kbd_get_events(). Antes o menu usava o s_mask de nivel com
//     deteccao de borda, e como o nivel ficava preso em 1 enquanto a tecla
//     estava pressionada, a borda so' acontecia uma vez -- dai "apertar 5x
//     para andar 1".

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

// Teclas que servem para NAVEGAR (setas + ENTER). No menu elas movem a
// selecao; no jogo NAO devem virar joystick -- viram cursor, pelo caminho do
// heldScancode() em c64.cpp (codigos 17/29/145/157). Por isso elas entram
// so' em s_events (consumido pelo menu) e nunca em s_mask (consumido pelo
// jogo). Ver ps2kbd_poll().
//
// Decisao de projeto (agosto/2026): tratamos o teclado como o de um C64
// real. No C64 as setas movem o CURSOR, nao um joystick -- o joystick era um
// periferico separado. Entao nenhuma tecla vira direcao de joystick. Se um
// dia quiser jogar com o teclado, e' aqui que se religa (mapear WASD ou as
// setas de volta para MASK_JOY2_* em s_mask).
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
    // F6 reabre o menu durante o jogo (o C64 sozinho nao teria essa tecla;
    // e' so' um atalho nosso). Consumido em go.cpp, uma vez por quadro.
    // F5 reseta o C64 emulado (volta ao BASIC), como o reset de um C64 real.
    // Nao reinicia o ESP32 -- isso continua no USER4 (GPIO36). Consumido em
    // go.cpp, uma vez por quadro.
    case fabgl::VK_F5:     return M_KEY_RESET;
    case fabgl::VK_F6:     return M_KEY_MENU;
    case fabgl::VK_KP_ENTER:return M_JOY2_BTN;
    default:               return 0;
  }
}

// Teclas que viram JOYSTICK quando s_joyMode esta ligado. Layout Sinclair:
//   Q = cima     A = baixo     O = esquerda     P = direita     SPACE = fire
// Separada de maskOf() de proposito: maskOf() serve a navegacao do menu
// (s_events), esta serve ao jogo (s_mask, por nivel). O bit final e' o
// mesmo (M_JOY2_*), e cai em cia1PORTA/PORTB via emu_ReadKeys() -- porta 1
// ou 2 conforme o SWAP (F1).
static uint16_t joyMaskOf(fabgl::VirtualKey vk) {
  switch (vk) {
    case fabgl::VK_q: case fabgl::VK_Q:  return M_JOY2_UP;
    case fabgl::VK_a: case fabgl::VK_A:  return M_JOY2_DOWN;
    case fabgl::VK_o: case fabgl::VK_O:  return M_JOY2_RIGHT;
    case fabgl::VK_p: case fabgl::VK_P:  return M_JOY2_LEFT;
    case fabgl::VK_SPACE:                return M_JOY2_BTN;
    default: return 0;
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

    // F2 alterna o modo joystick (Q/A/O/P/SPACE = joystick vs. teclado).
    // So' na borda de descida, e nunca chega ao C64 -- e' hotkey nosso.
    if (vk == fabgl::VK_F2) {
      if (down) {
        s_joyMode = !s_joyMode;
        // Libera qualquer direcao que tenha ficado presa quando o modo mudou.
        s_mask &= ~(M_JOY2_UP | M_JOY2_DOWN | M_JOY2_LEFT |
                    M_JOY2_RIGHT | M_JOY2_BTN);
        Serial.printf("[joy] modo joystick %s (Q/A/O/P/SPACE)\n",
                      s_joyMode ? "ON" : "OFF");
      }
      continue;
    }

    // No modo joystick, Q/A/O/P/SPACE alimentam s_mask como joystick e
    // NAO seguem para o caminho de teclado -- senao no jogo mandariam
    // direcao e letra ao mesmo tempo.
    if (s_joyMode) {
      uint16_t jm = joyMaskOf(vk);
      if (jm) {
        if (down) s_mask |= jm;
        else      s_mask &= ~jm;
        continue;
      }
    }

    uint16_t m = maskOf(vk);
    if (m) {
      // Dois destinos, com criterio: as teclas de NAVEGACAO (setas + ENTER)
      // vao SO' para s_events (consumido pelo menu). No jogo elas viram
      // cursor, nao joystick -- tratamos o teclado como o de um C64 real.
      //
      // Ja' as teclas de FUNCAO (F1/F5/F6) precisam ir para s_mask, porque
      // sao hotkeys checados durante o jogo por emu_ReadKeys() (F1=LOAD""+
      // RUN, F5=reset, F6=menu). Sem isso as F* silenciosamente pararam de
      // funcionar quando separei as setas do s_mask na rodada passada.
      const uint16_t navBits = M_JOY2_UP | M_JOY2_DOWN | M_JOY2_LEFT |
                               M_JOY2_RIGHT | M_JOY2_BTN;
      if (down) {
        s_events |= m;                          // menu ve tudo (nav e F*)
        if ((m & navBits) == 0) s_mask |= m;    // jogo so' recebe hotkeys
      } else {
        if ((m & navBits) == 0) s_mask &= ~m;   // libera hotkey ao soltar
      }
    }
    int c = kb->virtualKeyToASCII(vk);
    {
      // Varias teclas nao tem (ou tem o ASCII "errado" para o C64). Mapeamos
      // pelo VirtualKey, que e' inequivoco, para os codigos que a tabela
      // ascii2scan[] do c64.cpp entende:
      //   - setas -> codigos de cursor do C64 (17/29/145/157)
      //   - RETURN -> 13 (a FabGL as vezes devolve 10/'\n', que a tabela nao
      //     tem, e ai o Enter "sumia" ou caia noutra tecla)
      //   - BACKSPACE -> 20, o DEL/INST do C64 (ascii2scan[20]=0x49); a FabGL
      //     devolve 8, que na tabela e' 0 = nada
      switch (vk) {
        case fabgl::VK_UP:        c = 145; break;
        case fabgl::VK_DOWN:      c = 17;  break;
        case fabgl::VK_LEFT:      c = 157; break;
        case fabgl::VK_RIGHT:     c = 29;  break;
        case fabgl::VK_RETURN:
        case fabgl::VK_KP_ENTER:  c = 13;  break;
        case fabgl::VK_BACKSPACE: c = 20;  break;   // DEL/INST do C64
        case fabgl::VK_DELETE:    c = 20;  break;
        case fabgl::VK_HOME:      c = 19;  break;   // CLR/HOME
        // ESC do PS/2 = RUN/STOP do C64. Sem isto nao existe RUN/STOP
        // funcional (a tecla nao esta no PS/2 padrao). E' a tecla que
        // interrompe programas BASIC e sai de muitos loaders.
        case fabgl::VK_ESCAPE:    c = 3;   break;
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
    // Layout US DE PROPOSITO, mesmo com teclado fisico ABNT2. O C64 real e'
    // um teclado americano: nao tem c-cedilha nem acentos, e simbolos como "
    // # $ ( ) saem com shift+numero no arranjo US. Mapear o ABNT2 fielmente
    // deixaria o usuario sem teclas que o C64 tem e com teclas que o C64 nao
    // tem. A troca: alguns simbolos saem numa tecla fisica diferente da
    // serigrafia do seu teclado (ex.: shift+2 = " no US, nao @). E' o
    // comportamento correto para um emulador de C64.
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
  // Devolve o estado de nivel de:
  //   - HOTKEYS: F1 (SWAP + LOAD""+RUN), F5 (reset C64), F6 (menu).
  //   - JOYSTICK: Q/A/O/P/SPACE quando o modo joystick esta ligado (F2).
  // As setas de proposito NAO entram aqui -- para navegacao de menu usar
  // ps2kbd_get_events(). Ver ps2kbd_poll() para a separacao completa.
  return s_mask;
}

// Eventos de navegacao acumulados desde a ultima chamada -- e ZERA. Cada
// pressionar (e cada auto-repeat) conta uma vez. E' o que o menu deve usar
// para as setas/ENTER, em vez do s_mask de nivel.
uint16_t ps2kbd_get_events(void)
{
  ps2kbd_poll();
  uint16_t e = s_events;
  s_events = 0;
  return e;
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