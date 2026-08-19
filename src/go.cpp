#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "go.h"

extern "C" {
  #include "emuapi.h"
  #include "iopins.h"
}

#include "esp_event.h"
#include "esp_timer.h"

#include "keyboard_osd.h"
#ifdef HAS_PS2KBD
#include "ps2kbd.h"
#endif
#include "video_vga.h"
#include "esp_system.h"
#ifdef HAS_TDISPLAY_LINK
#include "port_link.h"
#endif
#ifdef HAS_SND
#include "AudioPlaySystem.h"
#endif

#include "c64.h"


VGA_Video video;
#ifdef HAS_SND
AudioPlaySystem audio;
#endif

// Trace de hotkeys: imprime a mascara toda vez que uma borda e' detectada.
// Serve para descobrir QUAL bit chega quando voce aperta F5/F6/F1.
// Deixe em 1 ate' o comportamento estar certo, depois zere.
#define HOTKEY_TRACE 1


// ===========================================================================
// DONO UNICO DO TECLADO
//
// Tudo que le tecla (emu_ReadKeys, emu_DebounceLocalKeys, emu_GetMenuKeys,
// ps2kbd_get_mask, ps2kbd_get_events) termina em ps2kbd_poll(), que faz
// read-modify-write em s_mask e s_events. Essas variaveis NAO sao atomicas e
// NAO tem lock.
//
// Antes existiam quatro leitores em dois cores: loop() (core 1, 50 ms),
// input_task (core 1, 20 ms), o bloco de hotkeys do emu_loop (core 0) e o
// menu em main_step (core 0). Os sintomas eram:
//
//   * setas do menu falhando ou saindo em rajada -- o "le e zera" do
//     ps2kbd_get_events() no core 0 colidia com o "s_events |= m" do
//     ps2kbd_poll() chamado pela input_task no core 1;
//   * F6 disparando reset -- um |= do core 0 reescrevia por cima de um
//     &= ~m do core 1, o bit do F5 piscava 1->0->1 entre duas amostras e a
//     deteccao de borda via isso como uma pressionada nova;
//   * F1 sumindo -- mesma corrida, no mesmo s_mask.
//
// REGRA A PARTIR DAQUI: so' a emuthread (core 0) le o teclado, e le UMA VEZ
// por quadro. A input_task cuida so' de audio e do link. O loop() do Arduino
// nao encosta no PS/2. Nao adicione nenhuma chamada de teclado fora de
// keys_step() / main_step() sem repensar isto.
// ===========================================================================

static uint16_t s_prevKeys = 0;

// Borda de subida (solto -> apertado) desde a ultima chamada. Um unico ponto
// de leitura, usado tanto no jogo quanto no menu -- os dois rodam na mesma
// task, entao nao ha corrida.
static uint16_t keys_edge(void)
{
  uint16_t keys = emu_ReadKeys();
  uint16_t edge = keys & ~s_prevKeys;
  s_prevKeys = keys;
  return edge;
}

// Zera o historico de bordas e descarta o que estiver acumulado. Chamado ao
// entrar e ao sair do menu: sem isto as setas apertadas durante o jogo saem
// todas de uma vez quando o menu abre (a lista pula sozinha), e uma tecla
// ainda segurada na saida do menu gera uma borda falsa no primeiro quadro.
static void keys_resync(void)
{
#ifdef HAS_PS2KBD
  ps2kbd_get_events();          // joga fora setas/ENTER acumulados
#endif
  s_prevKeys = emu_ReadKeys();  // nada que ja' esteja apertado vira borda
}


// Com o ILI9341 esta task bombeava o DMA: video.refresh() bloqueava esperando a
// transferencia. Com VGA o refresh() retorna na hora, entao o laco virava
// espera ocupada em prioridade 1 no core 0 e matava o IDLE0 (watchdog).
// A tela le o framebuffer continuamente; nao ha nada para bombear.
// Mantida so' para nao mexer em quem a referencia.
static void spi_task(void *args)
{
  while(true) {
    video.refresh();
    vTaskDelay(20 / portTICK_PERIOD_MS);
  } 
}

// So' audio e link. NAO LE TECLADO -- ver "DONO UNICO DO TECLADO" acima.
// O emu_DebounceLocalKeys()/emu_Input() que moravam aqui foram para
// keys_step(), na emuthread.
static void input_task(void *args)
{
  while(true) {
#ifdef HAS_TDISPLAY_LINK
    link_poll();   // core 0, a few bytes out of a FIFO: costs nothing
#endif
#ifdef HAS_SND      
    audio.step();
#endif  
    vTaskDelay(20 / portTICK_PERIOD_MS);
  } 
}

static bool inputTaskStarted = false;

// Sequencia de "sair do menu e rodar". Usada em dois lugares: quando o
// usuario escolhe um arquivo e aperta ENTER, e no boot (emu_setup chama isto
// direto com filename="", pulando o menu -- igual a ligar um C64 de verdade,
// que cai direto no BASIC).
static void startGame(char *filename) {
#ifdef HAS_SND      
  audio.begin();
  audio.start();
#endif                 
  toggleMenu(false); 
  video.fillScreenNoDma( RGBVAL16(0x00,0x00,0x00) );
  if (!inputTaskStarted) {
    // Core 1: audio.step() chama i2s_write, que pode bloquear -- fora do
    // core do emulador.
    xTaskCreatePinnedToCore(input_task, "inputthread", 4096, NULL, 2, NULL, 1);
    inputTaskStarted = true;
  }
#ifdef HAS_TDISPLAY_LINK
  link_send_game_name(filename);   // before emu_Init, which may block
#endif
  emu_Init(filename);        
#ifdef HAS_TDISPLAY_LINK
  // A button held during the load must not fire the instant the game starts.
  g_menuRequest = false;
#endif
  // O ENTER que escolheu o arquivo ainda pode estar fisicamente apertado, e
  // o emu_Init pode ter demorado varios segundos (leitura do SD) acumulando
  // eventos. Comeca o jogo com o teclado limpo.
  keys_resync();
}

// Abre o menu por caminho unico, para nao esquecer o resync em nenhum lugar.
//
// O keys_resync() aqui e' o conserto do bug do "F9 reseta o jogo". O
// ps2kbd_poll() acumula cada seta e cada ENTER em s_events enquanto o jogo
// roda, e ninguem drena isso -- so' o menu drena. Entao, ao abrir, a
// primeira leitura de emu_GetMenuKeys() devolvia todos os ENTER que voce
// deu no BASIC de uma vez, com MASK_JOY2_BTN ligado, e o handleMenu()
// respondia ACTION_RUN no mesmo quadro: o menu abria e ja' rodava o arquivo
// selecionado, o que na tela parece um reset. As setas guardadas faziam a
// lista pular sozinha pelo mesmo motivo.
static void openMenu(void)
{
  toggleMenu(true);
  keys_resync();
}

// Sai do menu sem carregar nada e volta para onde o C64 parou. O emu_Step()
// nao roda com o menu aberto, entao o C64 esta apenas congelado -- basta
// fechar. O VIC redesenha o quadro inteiro, entao a tela do menu some
// sozinha.
static void closeMenu(void)
{
  toggleMenu(false);
  keys_resync();
}

// ---------------------------------------------------------------------------
// Leitura de teclas do JOGO. Uma vez por quadro (~50 Hz), so' na emuthread.
// Nao e' chamada com o menu aberto: la' quem manda e' emu_GetMenuKeys().
// ---------------------------------------------------------------------------
static void keys_step(void)
{
  uint16_t edge = keys_edge();
  if (!edge) return;

#if HOTKEY_TRACE
  printf("[hot] edge=%04X  MENU=%04X RESET=%04X USER1=%04X\n",
         edge, (unsigned)MASK_KEY_MENU, (unsigned)MASK_KEY_RESET,
         (unsigned)MASK_KEY_USER1);
  fflush(stdout);
#endif

  // Prioridade e EXCLUSAO MUTUA. Se os dois bits aparecerem na mesma borda,
  // o menu ganha e o reset e' descartado -- nunca os dois no mesmo quadro.
  if (edge & MASK_KEY_MENU) {
    openMenu();
    return;
  }
  if (edge & MASK_KEY_RESET) {
    // F10: reseta o C64 (volta ao BASIC), mantendo o que estiver na RAM.
    emu_Reset();
    return;
  }

  if (edge & MASK_KEY_USER2) {
    printf("%d\n", emu_SwapJoysticks(1));
    emu_SwapJoysticks(0);
    return;
  }

  emu_Input(edge);
}

static void main_step() {
  if (menuActive()) {
#ifdef HAS_TDISPLAY_LINK
    // input_task may not exist yet (or the picker was re-opened while it runs
    // but does not own this branch): without this the pad looks dead in the
    // ROM picker.
    link_poll();
#endif
    // F9 fecha o menu e volta para o jogo, sem carregar nada. Sem isto o
    // unico jeito de sair era escolher um arquivo.
    if (keys_edge() & MASK_KEY_MENU) {
      closeMenu();
      return;
    }
    // emu_GetMenuKeys (nao ...DebounceLocalKeys): as setas do PS/2 vem por
    // EVENTO com auto-repeat, senao segurar a seta so' anda 1 item.
    uint16_t bClick = emu_GetMenuKeys();
    int action = handleMenu(bClick);
    char * filename = menuSelection();
    if (action == ACTION_RUN) {
      startGame(filename);
    }       
    // Estava comentado no original, que rodava com o watchdog desligado na
    // IDF 3.3. Sem isto o laco do menu gira a full speed no core 0, o IDLE0
    // nao roda e o task watchdog derruba a placa.
    vTaskDelay(20 / portTICK_PERIOD_MS); 
  }
  else {         
#ifdef HAS_TDISPLAY_LINK
    if (g_menuRequest) {
      g_menuRequest = false;
      openMenu();   // clears the T-Display "now playing" line
      return;
    }
#endif
    emu_Step();     
  }
}


// ===========================================================================
// TESTE DE ISOLAMENTO
//   2 = so' spi_bus_initialize + spi_bus_add_device, nada antes. Se resetar
//       aqui, o problema esta' no SPI/hardware, nao no resto do projeto.
//   1 = video completo (begin/flipscreen/start/refresh), sem link e sem emu_init
//   0 = setup normal
// Suba de 2 -> 1 -> 0 conforme cada etapa passar.
#define ISOLATION_TEST 0
// ===========================================================================

void emu_setup(void)
{
  printf("Starting emulator\n"); fflush(stdout);

#if ISOLATION_TEST == 0
#ifdef HAS_TDISPLAY_LINK
  link_init();          // before anything that might block (SD mount, etc.)
#endif
#else
  printf("### ISOLATION_TEST=%d: link_init() PULADO\n", ISOLATION_TEST);
  fflush(stdout);
#endif

  printf("setup: video begin()\n");        fflush(stdout);
	video.begin();
  printf("setup: video begin() VOLTOU\n"); fflush(stdout);

#if ISOLATION_TEST >= 2
  printf("### SPI ok. Parando aqui de proposito.\n"); fflush(stdout);
  while (1) { vTaskDelay(1000 / portTICK_PERIOD_MS); printf("."); fflush(stdout); }
#endif

  printf("setup: video flipscreen()\n");   fflush(stdout);
	video.flipscreen(true);  
  printf("setup: video start()\n");        fflush(stdout);
	video.start();
  printf("setup: video refresh()\n");      fflush(stdout);
	video.refresh();

#if ISOLATION_TEST >= 1
  printf("### Video ok. Parando aqui de proposito.\n"); fflush(stdout);
  while (1) { vTaskDelay(1000 / portTICK_PERIOD_MS); printf("."); fflush(stdout); }
#endif

  printf("setup: emu_init()\n");         fflush(stdout);
	emu_init(); 

  // Nao criamos mais a spithread: com VGA ela nao tem funcao e so' disputava
  // CPU com o emulador. O flush da ultima linha do quadro sai no emu_loop.
  //xTaskCreatePinnedToCore(spi_task, "spithread", 4096, NULL, 1, NULL, 0);
  //vTaskPrioritySet(NULL, tskIDLE_PRIORITY+1);     

  // Boot direto no BASIC, igual a um C64 de verdade: pula o menu no
  // power-on. F9 abre o menu depois, a qualquer momento (ver emu_loop).
  static char emptyName[1] = {0};
  printf("setup: startGame() -- pulando o menu, indo direto pro BASIC\n");
  fflush(stdout);
  startGame(emptyName);
}

void emu_loop(void)
{
  // video.refresh() saiu daqui: era chamado 15600 vezes por segundo so' para
  // checar um flag. O getLineBuffer() ja converte a linha anterior quando a
  // proxima e' pedida -- inclusive na volta da linha 239 para a 0.
  main_step();

  // ---- Cadencia de 50 Hz -------------------------------------------------
  // O core nao tem sincronismo proprio: oneRasterLine() roda o mais rapido
  // que a CPU deixar. Sem cadencia o emulador oscilava de 14000 a 18800
  // linhas/s, ou seja de 90% a 120% da velocidade real -- jogo acelerado e
  // com ritmo tremido. Um C64 PAL faz 312 linhas por quadro a 50 Hz, entao
  // dormimos o que sobrar de cada janela de 20 ms.
  {
    static int64_t nextFrame = 0;
    static int     lineCount = 0;

    if (++lineCount >= 312) {
      lineCount = 0;
      int64_t now = esp_timer_get_time();
      if (nextFrame == 0) nextFrame = now;
      nextFrame += 20000;                       // 20 ms = 50 Hz
      int64_t wait = nextFrame - now;
      if (wait > 1000) {
        vTaskDelay((wait / 1000) / portTICK_PERIOD_MS);
      } else if (wait < -200000) {
        // Ficamos mais de 200 ms atrasados (carga de arquivo, por exemplo):
        // nao adianta tentar recuperar o tempo perdido, ressincroniza.
        nextFrame = now;
      }

      // Teclado do jogo: UMA leitura por quadro, nesta task, neste core.
      // Atalhos (F9/F10), troca de joystick e emu_Input saem todos daqui --
      // ver keys_step() e o bloco "DONO UNICO DO TECLADO" no topo.
      if (!menuActive()) {
        keys_step();
      }
    }
  }

  // c64_Step() emula UMA linha de raster, entao main_step() e' chamado umas
  // 15600 vezes por segundo -- delay a cada chamada mataria o desempenho.
  // ---- Instrumentacao de desempenho -------------------------------------
  // Um C64 PAL faz 50 quadros/s e 312 linhas de raster por quadro. c64_Step()
  // emula UMA linha, entao main_step() deveria ser chamado ~15600 vezes/s.
  // "linhas/s" abaixo da esse numero: se estiver bem abaixo de 15600, o
  // emulador nao esta acompanhando o tempo real.
  // "quadros/s" conta quando o VIC volta para a linha 0 (esperado ~50).
  // "conv" e' o custo da conversao de linha para o framebuffer VGA.
  {
    static int64_t  tStat = 0;
    static uint32_t steps = 0;
    steps++;
    int64_t tnow = esp_timer_get_time();
    if (tStat == 0) tStat = tnow;
    if (tnow - tStat >= 5000000) {                 // a cada 5 s
      unsigned long      frames = 0;
      unsigned long long flushUs = 0;
      vga_get_stats(&frames, &flushUs);
      double secs = (double)(tnow - tStat) / 1000000.0;
      printf("[PERF] linhas/s=%.0f (esperado 15600)  quadros/s=%.1f (esperado 50)"
             "  conv=%.1f%% da CPU  heap=%u\n",
             steps / secs, frames / secs,
             100.0 * (double)flushUs / (double)(tnow - tStat),
             (unsigned)esp_get_free_heap_size());
      fflush(stdout);
      steps = 0;
      tStat = tnow;
    }
  }

  // Rede de seguranca do watchdog: se o emulador estiver atrasado, a cadencia
  // acima nunca dorme, e sem ceder CPU o IDLE0 morre de fome.
  static int64_t lastYield = 0;
  int64_t now = esp_timer_get_time();
  if (now - lastYield > 50000) {
    lastYield = now;
    vTaskDelay(1);
  }
} 



// ---------------------------------------------------------------------------
// Entrada Arduino. O emulador roda em task propria com pilha folgada em vez
// da loopTask (8 KB por padrao), e o loop() do Arduino so' dorme.
// ---------------------------------------------------------------------------
static void emu_task(void *arg)
{
  Serial.printf("emuthread no core %d\n", (int)xPortGetCoreID());
  Serial.flush();
  emu_setup();
  while (1) emu_loop();
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== MCUME esp64 (C64) - TTGO VGA32 ===");
  Serial.println("Atalhos: F9=menu  F10=reset C64  F11=LOAD\"\"+RUN  F12=joystick");
  Serial.println("F1..F8 sao teclas do C64 e vao direto para o jogo.");

  // Integracao com o bootloader (fg1998/esp32-bootloader): apagar o otadata
  // faz o ESP32 voltar para a particao factory no proximo boot, em vez de
  // recarregar este emulador. Tem que ser cedo, antes de qualquer periferico.
  {
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
      esp_partition_erase_range(otadata, 0, otadata->size);
      Serial.println("otadata apagado: proximo boot vai para a factory");
    } else {
      Serial.println("AVISO: nao ha particao otadata (veja board_build.partitions)");
    }
    Serial.flush();
  }
  Serial.printf("PSRAM: %s  tamanho=%u  livre=%u\n",
                psramFound() ? "detectada" : "NAO detectada",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  Serial.printf("Heap interno: livre=%u  DMA livre=%u  maior bloco DMA=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  Serial.printf("Compilado: %s %s\n", __DATE__, __TIME__);
  Serial.printf("setup() rodando no core %d (a ISR de video vai para ca)\n",
                (int)xPortGetCoreID());
  Serial.print("Recursos:");
#ifdef HAS_PS2KBD
  Serial.print(" PS2");
#else
  Serial.print(" [SEM PS2]");
#endif
#ifdef NO_ANALOG_JOYSTICK
  Serial.print(" sem-ADC-joy");
#else
  Serial.print(" [ADC-JOY LIGADO: usa GPIO2=MISO do SD!]");
#endif
#ifdef HAS_TDISPLAY_LINK
  Serial.print(" link-TDisplay");
#endif
#ifdef HAS_I2CKBD
  Serial.print(" [I2C-KBD LIGADO: usa GPIO4/5=VGA!]");
#endif
  Serial.println();
  Serial.flush();

  // ORDEM E CORE IMPORTAM AQUI.
  //
  // A FabGL aloca a ISR de scanline no core onde setResolution() e' chamado.
  // Antes o video.begin() acontecia dentro do emu_setup(), ou seja na
  // emuthread (core 0) -- a interrupcao de video, que dispara 15700 vezes por
  // segundo em nivel 3, ficava disputando o mesmo core que o emulador,
  // enquanto o core 1 ficava ocioso.
  //
  // setup() do Arduino roda na loopTask, que por padrao esta no core 1.
  // Inicializando o video aqui, a ISR fica no core 1 e o core 0 sobra para o
  // emulador. O video.begin() do emu_setup() vira no-op (ele tem guarda).
  video.begin();

  xTaskCreatePinnedToCore(emu_task, "emuthread", 16384, NULL, 1, NULL, 0);
}

void loop()
{
  // NAO LE TECLADO. O ps2kbd_get_mask() que estava aqui era o quarto leitor
  // concorrente do PS/2, rodando no core 1, e drenava/corrompia o estado que
  // a emuthread precisava no core 0.
  vTaskDelay(50 / portTICK_PERIOD_MS);
}