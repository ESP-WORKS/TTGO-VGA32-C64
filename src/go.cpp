#include <Arduino.h>
#include "esp_heap_caps.h"
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

static void input_task(void *args)
{
  while(true) {
#ifdef HAS_TDISPLAY_LINK
    link_poll();   // core 0, a few bytes out of a FIFO: costs nothing
#endif
    if ( ((emu_ReadKeys() & (MASK_KEY_USER1+MASK_KEY_USER2)) == (MASK_KEY_USER1+MASK_KEY_USER2))
      || (emu_ReadKeys() & MASK_KEY_USER4 ) )
    {  
      printf("rebooting\n");
      esp_restart();    
    }

    uint16_t bClick = emu_DebounceLocalKeys();
    if (bClick & MASK_KEY_USER2) { 
      printf("%d\n",emu_SwapJoysticks(1)); 
      emu_SwapJoysticks(0);
    }
    else {
      emu_Input(bClick);
    }
#ifdef HAS_SND      
    audio.step();
#endif  
    vTaskDelay(20 / portTICK_PERIOD_MS);
  } 
}

static bool inputTaskStarted = false;

static void main_step() {
  if (menuActive()) {
#ifdef HAS_TDISPLAY_LINK
    // input_task may not exist yet (or the picker was re-opened while it runs
    // but does not own this branch): without this the pad looks dead in the
    // ROM picker.
    link_poll();
#endif
    uint16_t bClick = emu_DebounceLocalKeys();
    int action = handleMenu(bClick);
    char * filename = menuSelection();
    if (action == ACTION_RUN) {
#ifdef HAS_SND      
      audio.begin();
      audio.start();
#endif                 
      toggleMenu(false); 
      video.fillScreenNoDma( RGBVAL16(0x00,0x00,0x00) );
      if (!inputTaskStarted) {
        // Core 1: tira do core 0 tudo que nao e' o emulador. O audio.step()
        // desta task chama i2s_write, que pode bloquear.
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
      toggleMenu(true);   // clears the T-Display "now playing" line
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
}

void emu_loop(void)
{
  video.refresh();   // libera a ultima linha do quadro que ficou no scratch
  main_step();

  // c64_Step() emula UMA linha de raster, entao main_step() e' chamado umas
  // 15600 vezes por segundo -- delay a cada chamada mataria o desempenho.
  // Mas sem ceder CPU nenhuma o IDLE0 do core 0 nunca roda e o task watchdog
  // derruba a placa. Cedemos um tick a cada 10 ms de tempo real: custa ~1 ms
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

  // em 20 ms de quadro e mantem o watchdog alimentado. 50 ms em vez de 10 ms:
  // vTaskDelay(1) custa ate' 1 ms, entao ceder a cada 10 ms tirava ~10% da CPU
  // do emulador. O watchdog e' de 5 s, 50 ms sobra de folga.
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
  // O emulador roda na emuthread; esta task so' cutuca o teclado. Serve como
  // rede de seguranca: se o PS/2 responder aqui mas nao no menu, o problema
  // e' o caminho emu_ReadKeys(), nao a FabGL.
#ifdef HAS_PS2KBD
  ps2kbd_get_mask();
#endif
  vTaskDelay(50 / portTICK_PERIOD_MS);
}