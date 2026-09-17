#include <Arduino.h>
#include <fabgl.h>
#include <stdio.h>
#include "ps2kbd.h"

#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif

// 1 = imprime cada tecla recebida no serial. Use para confirmar se o teclado
// esta chegando antes de procurar problema no mapeamento.
#define PS2_TRACE 1   // TEMPORARIO: veja no serial o vk/ascii de cada tecla; volte a 0 depois

// Teclado PS/2 da TTGO VGA32 via FabGL (preset KeyboardPort0 = CLK 33 / DAT 32).
//
// Duas saidas, porque o emulador consome teclado de dois jeitos diferentes:
//
//  - ps2kbd_read_ascii(): fila de ASCII, consumida por emu_ReadI2CKeyboard(),
//    que o c64_Input() usa para indexar ascii2scan[]. So' roda com jogo ativo.
//  - ps2kbd_get_mask():   estado de NIVEL dos atalhos (F9/F10/F11) e do
//    joystick por teclado (F12). Lido por emu_ReadKeys().
//  - ps2kbd_get_events(): eventos de navegacao (setas/ENTER) acumulados,
//    drenados a cada chamada. E' o que o menu consome.
//
// QUEM CHAMA: a partir de agosto/2026, SO' a emuthread (core 0). O loop() do
// Arduino e a input_task nao encostam mais no teclado -- duas tasks em cores
// diferentes faziam read-modify-write em s_mask/s_events sem lock.
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
#define M_JOY1_RIGHT 0x0100
#define M_JOY1_LEFT  0x0200
#define M_JOY1_UP    0x0400
#define M_JOY1_DOWN  0x0800
#define M_JOY1_BTN   0x1000
#define M_KEY_USER1  0x0020   // dispara a macro LOAD""+RUN no c64_Input()
#define M_KEY_MENU   0x4000   // F6: reabre o menu durante o jogo
#define M_KEY_RESET  0x8000   // F5: reseta o C64 emulado

static fabgl::PS2Controller ps2;
static bool     kbdReady = false;
static int      s_kbd_clk = 33;  // pino CLK efetivo em uso (default ou do bootl.rc)
static int      s_kbd_dat = 32;  // pino DAT efetivo em uso (default ou do bootl.rc)
static uint16_t s_mask   = 0;   // ESTADO das teclas (nivel): para o jogo
static uint8_t  s_held   = 0;   // ASCII da tecla atualmente SEGURADA

// ---- Navegacao do menu: FILA de eventos, nao acumulador ------------------
//
// s_events era um OR: cada 'down' ligava um bit, e o menu drenava a mascara
// inteira de uma vez. Dois problemas praticos:
//
//  1) PERDA. O menu le a cada 20 ms. Tres toques rapidos na seta dentro da
//     mesma janela ligavam o MESMO bit -- viravam UM passo. Voce apertava
//     tres vezes e a lista andava um item.
//  2) REPETICAO LENTA. Segurar a seta dependia do typematic do proprio
//     teclado: ~500 ms parado e depois ~11 repeticoes por segundo. E' isso
//     que da a sensacao de "buferizado" -- aperta, nada, e de repente anda.
//
// Agora cada transicao solto->apertado entra numa FILA (nada se perde, e a
// ordem e' respeitada) e a repeticao de segurar e' gerada AQUI, com tempos
// que nos controlamos. As repeticoes que o proprio teclado manda sao
// ignoradas, senao a velocidade dobraria.
#define NAV_REPEAT_DELAY_MS 280   // espera antes de comecar a repetir
#define NAV_REPEAT_RATE_MS   45   // intervalo entre repeticoes (~22/s)

#define EVQ_SIZE 12
static uint16_t evQ[EVQ_SIZE];
static int      evHead = 0, evTail = 0;
static uint16_t s_navLevel = 0;   // setas/ENTER apertadas AGORA
static uint32_t s_navNextMs = 0;  // quando disparar a proxima repeticao

static inline void evPush(uint16_t m) {
  int n = (evHead + 1) % EVQ_SIZE;
  if (n != evTail) { evQ[evHead] = m; evHead = n; }   // cheia: descarta
}
static inline uint16_t evPop(void) {
  if (evHead == evTail) return 0;
  uint16_t m = evQ[evTail];
  evTail = (evTail + 1) % EVQ_SIZE;
  return m;
}

// Modo joystick, alternado por F2. Ligado: Q/A/O/P/SPACE viram joystick
// (layout Sinclair classico) e sao SUPRIMIDAS do caminho de teclado --
// nao entram em s_held nem na fila ASCII, senao no jogo elas mandariam os
// dois sinais ao mesmo tempo. Desligado: as mesmas teclas digitam normal.
// Existe porque Q/A/O/P sao letras -- sem o toggle, digitar no BASIC viraria
// comando de joystick.
// 0 = OFF, 1 = teclado mapeia porta 1, 2 = teclado mapeia porta 2
static int s_joyMode = 0;

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
    // ---- ATALHOS DO EMULADOR: F9..F12, NUNCA F1..F8 --------------------
    // F1..F8 sao teclas DE VERDADE do C64 (linha 0 da matriz do teclado:
    // F1=$3a, F3=$3c, F5=$3e, F7=$40 em codigo USB; F2/F4/F6/F8 sao as
    // mesmas com SHIFT). Praticamente todo jogo usa alguma delas na tela de
    // titulo -- "press F1 to start", selecao de 1/2 jogadores, etc.
    // Enquanto F1 e F5 eram atalhos NOSSOS, elas eram consumidas aqui e
    // NUNCA chegavam na matriz: o jogo simplesmente nao via a tecla.
    //
    // Por isso os atalhos do emulador vivem de F9 a F12, que o C64 nao tem:
    //   F9  = abre o menu de ROMs           (M_KEY_MENU,  tratado em go.cpp)
    //   F10 = reseta o C64 (volta ao BASIC) (M_KEY_RESET, tratado em go.cpp)
    //   F11 = macro LOAD"" + RUN            (M_KEY_USER1, tratado em c64_Input)
    //   F12 = liga/desliga o modo joystick  (tratado direto no poll abaixo)
    //
    // A macro do F11: o c64_Input() digita LOAD"" + Enter, espera 2 s e
    // digita RUN. O patchLOAD() ve o nome vazio (RAM[0xB7]==0) e usa o
    // menuSelection(), ou seja o arquivo escolhido no menu. So' funciona com
    // .PRG: o patch le 2 bytes de endereco e despeja o resto na RAM.
    case fabgl::VK_F9:     return M_KEY_MENU;
    case fabgl::VK_F10:    return M_KEY_RESET;
    case fabgl::VK_F11:    return M_KEY_USER1;
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
// Contador de ciclos do nucleo: uma instrucao, sem dependencia de header.
static inline uint32_t ps2_ccount(void)
{
  uint32_t r;
  asm volatile ("rsr %0, ccount" : "=r"(r));
  return r;
}

static void ps2kbd_poll(void)
{
  if (!kbdReady) return;

  // LIMITADOR DE FREQUENCIA. Nao e' otimizacao prematura: cia1PORTA() e
  // cia1PORTB() em c64.cpp chamam heldScancode() E emu_ReadKeys(), e cada um
  // cai aqui. Como o 6502 le essas portas o tempo todo, isto era executado
  // centenas de milhares de vezes por segundo, entrando na secao critica da
  // FabGL a cada vez. 1 ms de granularidade e' de sobra para um teclado
  // (o menu le a 20 ms e o jogo a 20 ms).
  // O corte usa o contador de ciclos do nucleo (get_ccount), nao micros().
  // micros() e' esp_timer_get_time(): leitura de contador de 64 bits com
  // sequencia de latch, dezenas de ciclos -- e ela acontecia ANTES do corte,
  // ou seja, era paga em todas as centenas de milhares de chamadas por
  // segundo descritas acima. O ccount e' uma unica instrucao (RSR).
  // A 240 MHz, 1 ms = 240000 ciclos. O contador da' a volta a cada ~17,9 s,
  // mas a subtracao em uint32 continua correta na volta.
  static uint32_t lastPollCyc = 0;
  uint32_t nowCyc = ps2_ccount();
  if ((uint32_t)(nowCyc - lastPollCyc) < 240000u) return;
  lastPollCyc = nowCyc;

  fabgl::Keyboard *kb = ps2.keyboard();
  if (!kb) return;

#if PS2_TRACE
  // Status periodico: prova se o poll esta rodando e o que o teclado reporta.
  static uint32_t lastStat = 0;
  if (millis() - lastStat > 2000) {
    lastStat = millis();
  //  Serial.printf("[PS2] poll vivo | disponivel=%d vk=%d\n",
  //                (int)kb->isKeyboardAvailable(),
  //                (int)kb->virtualKeyAvailable());
  }
#endif

  while (kb->virtualKeyAvailable()) {
    bool down = false;
    fabgl::VirtualKey vk = kb->getNextVirtualKey(&down);

#if PS2_TRACE
//    Serial.printf("[PS2] vk=%d down=%d ascii=%d\n",
//                  (int)vk, (int)down, (int)kb->virtualKeyToASCII(vk));
#endif

    // F12 cicla o modo joystick: OFF -> J1 -> J2 -> OFF.
    // J1 = Q/A/O/P/SPACE viram joystick na porta 1 do C64.
    // J2 = idem, porta 2. OFF = teclado normal.
    if (vk == fabgl::VK_F12) {
      if (down) {
        s_joyMode = (s_joyMode + 1) % 3;
        // Libera qualquer direcao que tenha ficado presa quando o modo mudou.
        s_mask &= ~(M_JOY2_UP | M_JOY2_DOWN | M_JOY2_LEFT |
                    M_JOY2_RIGHT | M_JOY2_BTN |
                    M_JOY1_UP | M_JOY1_DOWN | M_JOY1_LEFT |
                    M_JOY1_RIGHT | M_JOY1_BTN);
        s_navLevel = 0;
        s_navNextMs = 0;
        const char *label[] = {"OFF", "J1 (porta 1)", "J2 (porta 2)"};
        Serial.printf("[joy] modo joystick %s (Q/A/O/P/SPACE)\n", label[s_joyMode]);
      }
      continue;
    }

    // No modo joystick, Q/A/O/P/SPACE alimentam s_mask como joystick e
    // NAO seguem para o caminho de teclado -- senao no jogo mandariam
    // direcao e letra ao mesmo tempo.
    if (s_joyMode != 0) {
      uint16_t jm = joyMaskOf(vk);
      // Em J1 mode, desloca para M_JOY1_* (byte alto).
      if (s_joyMode == 1 && jm) jm <<= 8;
      if (jm) {
        if (down) s_mask |= jm;
        else      s_mask &= ~jm;
        continue;
      }
    }

    uint16_t m = maskOf(vk);
    if (m) {
      // Dois destinos, com criterio:
      //
      //  - NAVEGACAO (setas + ENTER) vai SO' para s_events, o acumulador de
      //    eventos que o menu consome. No jogo essas teclas viram cursor
      //    (via heldScancode em c64.cpp), nao joystick -- tratamos o teclado
      //    como o de um C64 real.
      //
      //  - ATALHOS (F9/F10/F11) vao SO' para s_mask, o estado de nivel que
      //    emu_ReadKeys() devolve e o go.cpp checa por borda uma vez por
      //    quadro.
      //
      // Os atalhos NAO entram mais em s_events. Antes entravam, e o menu
      // recebia um M_KEY_MENU parado la' dentro na primeira leitura -- lixo
      // que o handleMenu() nao espera.
      const uint16_t navBits = M_JOY2_UP | M_JOY2_DOWN | M_JOY2_LEFT |
                               M_JOY2_RIGHT | M_JOY2_BTN;
      if (m & navBits) {
        // Navegacao: so' a TRANSICAO conta. A FabGL reentrega 'down' a cada
        // repeticao do teclado; se aceitassemos, teriamos duas fontes de
        // repeticao (a do teclado e a nossa) e o cursor voaria.
        if (down) {
          if ((s_navLevel & m) == 0) {
            s_navLevel |= m;
            evPush(m);
            s_navNextMs = millis() + NAV_REPEAT_DELAY_MS;
          }
        } else {
          s_navLevel &= ~m;
          if (!s_navLevel) s_navNextMs = 0;
        }
      } else {
        // Atalho (F9/F10/F11): nivel puro, lido por emu_ReadKeys().
        if (down) s_mask |= m;
        else      s_mask &= ~m;
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
        // F1..F8 do C64. A tabela ascii2scan[] reserva os codigos 133..140
        // para elas (133=F1 -> 0x3a, ..., 140=F8 -> 0x41 em codigo USB), e o
        // keymatrixmap[] resolve isso para a linha 0 da matriz. Sem estas
        // linhas o s_held ficava 0 e a tecla nunca chegava ao jogo, que era
        // o motivo de "aperto F1 e o jogo nao comeca".
        case fabgl::VK_F1:        c = 133; break;
        case fabgl::VK_F2:        c = 134; break;   // = SHIFT+F1 no C64
        case fabgl::VK_F3:        c = 135; break;
        case fabgl::VK_F4:        c = 136; break;   // = SHIFT+F3 no C64
        case fabgl::VK_F5:        c = 137; break;
        case fabgl::VK_F6:        c = 138; break;   // = SHIFT+F5 no C64
        case fabgl::VK_F7:        c = 139; break;
        case fabgl::VK_F8:        c = 140; break;   // = SHIFT+F7 no C64
        // F9..F12 sao atalhos nossos: nunca viram tecla do C64.
        case fabgl::VK_F9:
        case fabgl::VK_F10:
        case fabgl::VK_F11:
        case fabgl::VK_F12:       c = 0;   break;
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

  // Le o bootl.rc do SD (gerado pelo bootloader) para obter os pinos do
  // teclado PS/2. Formato: "kbddat=32\nkbdclk=33\nmagicb=36\n"
  // Se o arquivo nao existir, usa os defaults (CLK=33, DAT=32).
  int kbd_clk = -1, kbd_dat = -1;
  {
    FILE *f = fopen(SD_MOUNT_POINT "/bootl.rc", "r");
    if (f) {
      char line[32];
      while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, "kbdclk=%d", &val) == 1) kbd_clk = val;
        if (sscanf(line, "kbddat=%d", &val) == 1) kbd_dat = val;
      }
      fclose(f);
      printf("[bootl.rc] CLK=%d DAT=%d\n", kbd_clk, kbd_dat);
    } else {
      printf("[bootl.rc] nao encontrado, usando defaults (CLK=33 DAT=32)\n");
    }
  }
  // Pinos efetivos: do bootl.rc quando presentes, senao os defaults da placa.
  s_kbd_clk = (kbd_clk < 0) ? 33 : kbd_clk;
  s_kbd_dat = (kbd_dat < 0) ? 32 : kbd_dat;

  // CreateVirtualKeysQueue, nao GenerateVirtualKeys: e' este modo que faz a
  // FabGL manter a FILA de teclas. Com GenerateVirtualKeys ela converte o
  // scancode mas nao enfileira nada, entao virtualKeyAvailable() fica sempre
  // falso e getNextVirtualKey() nunca tem o que devolver -- exatamente o
  // sintoma de "teclado detectado mas nenhuma tecla chega".
  if (s_kbd_clk == 33 && s_kbd_dat == 32) {
    // Preset padrao -- FabGL configura os pinos automaticamente.
    ps2.begin(PS2Preset::KeyboardPort0, KbdMode::CreateVirtualKeysQueue);
  } else {
    // Pinos personalizados lidos do bootl.rc.
    ps2.begin((gpio_num_t)s_kbd_clk, (gpio_num_t)s_kbd_dat);
    ps2.setKeyboard(new fabgl::Keyboard);
    ps2.keyboard()->begin((gpio_num_t)s_kbd_clk, (gpio_num_t)s_kbd_dat,
                           true, true);
  }
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
  //Serial.printf("[PS2] objeto=%s  dispositivo=%s\n",
  //              kb ? "ok" : "nulo",
  //              (kb && kb->isKeyboardAvailable()) ? "DETECTADO" : "nao responde");
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

// UM evento de navegacao por chamada (0 = nenhum). O menu chama isto a cada
// iteracao, entao a cadencia de rolagem e' a do laco do menu, nao a do
// teclado. Nada se perde: toques rapidos ficam na fila e saem um por vez.
//
// Quando a fila esvazia e a tecla continua apertada, geramos a repeticao
// aqui -- NAV_REPEAT_DELAY_MS ate' comecar, depois um passo a cada
// NAV_REPEAT_RATE_MS. Para esvaziar a fila (ao abrir/fechar o menu) basta
// chamar em laco ate' devolver 0.
uint16_t ps2kbd_get_events(void)
{
  ps2kbd_poll();

  uint16_t e = evPop();
  if (e) return e;

  // ENTER de proposito NAO repete: segurar carregaria o jogo varias vezes.
  uint16_t rep = s_navLevel & ~M_JOY2_BTN;
  if (rep && s_navNextMs && (int32_t)(millis() - s_navNextMs) >= 0) {
    s_navNextMs = millis() + NAV_REPEAT_RATE_MS;
    return rep;
  }
  return 0;
}


// Pinos efetivos do teclado PS/2 em uso (default 33/32, ou os lidos do
// bootl.rc). Usados pelo rodape para mostrar a configuracao ativa.
int ps2kbd_get_clk_pin(void) { return s_kbd_clk; }
int ps2kbd_get_dat_pin(void) { return s_kbd_dat; }

// Modo joystick atual: 0=OFF, 1=J1 (porta 1), 2=J2 (porta 2).
// Usado pelo go.cpp para desenhar o indicador visual.
int ps2kbd_get_joy_mode(void) { return s_joyMode; }

// Tecla atualmente segurada, em ASCII. O c64.cpp injeta isto direto na matriz
// do teclado em cia1PORTA/PORTB. Sem isso so' existe o caminho setKey(), que
// e' um pulso de 20 ms -- suficiente para digitar no BASIC, inutil em jogo,
// onde a tecla precisa ficar pressionada enquanto o dedo estiver nela.
int ps2kbd_get_held_ascii(void)
{
  ps2kbd_poll();
  return s_held;
}