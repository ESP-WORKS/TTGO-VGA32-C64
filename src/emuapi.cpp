#define KEYMAP_PRESENT 1

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern "C" {
  #include "emuapi.h"
  #include "iopins.h"
}

#include "video_vga.h"
#include "ps2kbd.h"
//#include "logo.h"

#ifdef HAS_TDISPLAY_LINK
#include "port_link.h"
#endif

#include "esp_event.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <sys/stat.h>
#include <driver/adc.h>
#include <stdlib.h>          // qsort, malloc/realloc/free
#include <strings.h>         // strcasecmp
#include "esp_timer.h"       // ritmo de auto-repeat do menu

// Aliases legadas caso a versao da IDF nao as exponha mais.
#ifndef HSPI_HOST
#define HSPI_HOST SPI2_HOST
#endif
#ifndef VSPI_HOST
#define VSPI_HOST SPI3_HOST
#endif

// SD e touch do ILI9341 dividem este barramento.
#define SDSPI_HOST_ID HSPI_HOST

#ifdef HAS_I2CKBD
#ifdef USE_WIRE
#include "Wire.h"
#else
#include <driver/i2c.h>
#define ACK_CHECK_EN          0x1              /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS         0x0              /*!< I2C master will not check ack from slave */
#define ACK_VAL               0x0              /*!< I2C ack value */
#define NACK_VAL              0x1              /*!< I2C nack value */
#endif

#define I2C_FREQ_HZ           400000           /*!< I2C master clock frequency */
static bool i2cKeyboardPresent = false;
#endif


extern VGA_Video video;

static char romspath[64];
static int calMinX=-1,calMinY=-1,calMaxX=-1,calMaxY=-1;
static sdmmc_card_t* card;
const uint16_t deflogo[] = {
  0x0000,0x0000
};
static const uint16_t * logo = deflogo;

#define CALIBRATION_FILE    "/sdcard/cal.cfg"

#define MAX_FILENAME_SIZE   28    // largura de EXIBICAO, em caracteres
#define MENU_NAME_MAXLEN    63    // tamanho maximo do nome REAL do arquivo

// Fonte do menu: 8x8 (doublesize=false), nao mais 8x16. Com isso cabem bem
// mais linhas na tela sem precisar rolar tanto -- MAX_MENULINES nao depende
// mais de MKEY_L9 (era 9, resquicio do grid de toque removido).
#define TEXT_HEIGHT         8
#define TEXT_WIDTH          8
#define MAX_MENULINES       24
#define MENU_FILE_XOFFSET   (1*TEXT_WIDTH)
#define MENU_FILE_YOFFSET   (2*TEXT_HEIGHT)
#define MENU_FILE_W         (MAX_FILENAME_SIZE*TEXT_WIDTH)
#define MENU_FILE_H         (MAX_MENULINES*TEXT_HEIGHT)
#define MENU_FILE_FGCOLOR   RGBVAL16(0xff,0xff,0xff)
#define MENU_FILE_BGCOLOR   RGBVAL16(0x00,0x00,0x20)
// Rodape (status "SWAP" + dica de teclas), logo abaixo da lista.
#define MENU_FOOTER_YOFFSET (MENU_FILE_YOFFSET + MENU_FILE_H + TEXT_HEIGHT)

// Os arrays menutouchareas/menutouchactions e as constantes MKEY_* foram
// removidos: eram o grid de toque do menu (atalho numerico 1-9 + setas via
// touchscreen). A VGA32 nao tem touch, entao nunca disparavam. O
// captureTouchZone() em si continua (ainda serve ao teclado virtual noutra
// tela), so' o mapa do menu saiu.

  
static bool menuOn=true;
static bool callibrationOn=false;
static int callibrationStep=0;
static bool menuRedraw=true;
static int nbFiles=0;
static int curFile=0;
static int topFile=0;
// Nome do item selecionado. Guardado INTEIRO (ate MENU_NAME_MAXLEN), nao
// truncado em MAX_FILENAME_SIZE -- este ultimo e' so' a largura de EXIBICAO.
// Antes um arquivo com nome longo aparecia cortado na tela e o corte ia junto
// para o emu_FileOpen(), que entao nao achava o arquivo.
static char selection[MENU_NAME_MAXLEN+1]="";
static bool selIsDir=false;
static uint8_t prev_zt=0; 

static int  prevCurFile = -1;   // -1 = ainda nao desenhamos nada

// "._Nome" e' o arquivo de recurso (AppleDouble) que o macOS cria toda vez
// que copia algo para um volume FAT/exFAT -- some cartao acaba cheio deles,
// um para cada arquivo de verdade. Sem este filtro eles aparecem no menu
// como entradas invalidas (nao carregam nada, so' confundem a lista).
static inline bool isJunkFile(const char *name) {
  return (name[0] == '.' && name[1] == '_');
}

// ===== Catalogo de arquivos ==============================================
//
// O diretorio e' lido UMA vez ao abrir a pasta, ordenado e mantido em RAM
// INTERNA (esta placa nao tem PSRAM). "Ler tudo de uma vez" parece caro, mas
// e' o caminho RAPIDO: enumerar um diretorio e' leitura sequencial de alguns
// setores (nao e' abrir/ler cada arquivo) e roda UMA vez por pasta. O lento
// era o codigo antigo -- relia o diretorio a cada troca de pagina e ainda
// chamava stat() a cada quadro.
//
// Para caber na RAM interna, sem PSRAM:
//   - o buffer de nomes CRESCE sob demanda (realloc), usando so' o que a
//     pasta precisa -- tipicamente poucos KB, nao um bloco fixo enorme;
//   - o catalogo e' LIBERADO quando um jogo comeca a rodar (menu_freeCatalog),
//     devolvendo a RAM ao emulador, e relido quando o menu reabre.
// Guardamos so' um offset (uint16) + flag por arquivo; o nome vive uma vez
// so' na arena. Organizar as ROMs em subpastas mantem cada dir pequeno.

#define MENU_MAX_FILES      2000
#define MENU_ARENA_CAP      (60*1024)   // < 64KB: offset cabe em uint16_t

typedef struct {
  uint16_t off;    // deslocamento do nome dentro de menuArena
  uint8_t  isDir;
} MenuEntry;

static MenuEntry *menuEntries    = NULL;
static uint32_t   menuEntriesCap = 0;    // capacidade atual (entradas)
static char      *menuArena      = NULL; // nomes empacotados, '\0' entre eles
static uint32_t   menuArenaCap   = 0;
static uint32_t   menuArenaUsed  = 0;
static bool       catalogLoaded  = false;
static char       romsbase[64]   = "";   // raiz das ROMs -- limite do ".."

#define ENTRY_NAME(i)  (menuArena + menuEntries[i].off)

static inline bool menu_isUpEntry(const char *n) {
  return (n[0]=='.' && n[1]=='.' && n[2]==0);
}

static int menu_cmpEntry(const void *a, const void *b) {
  const MenuEntry *ea = (const MenuEntry*)a;
  const MenuEntry *eb = (const MenuEntry*)b;
  const char *na = menuArena + ea->off;
  const char *nb = menuArena + eb->off;
  bool ua = menu_isUpEntry(na), ub = menu_isUpEntry(nb);
  if (ua != ub) return ua ? -1 : 1;                       // ".." no topo
  if (ea->isDir != eb->isDir) return ea->isDir ? -1 : 1;  // pastas antes
  return strcasecmp(na, nb);                              // A-Z sem case
}

// Libera o catalogo. Chamado ao rodar um jogo (devolve RAM ao emulador) e no
// inicio de cada rescan.
void menu_freeCatalog(void) {
  free(menuEntries); menuEntries = NULL; menuEntriesCap = 0;
  free(menuArena);   menuArena   = NULL; menuArenaCap   = 0;
  menuArenaUsed = 0;
  nbFiles = 0;
  catalogLoaded = false;
}

// Garante espaco para +1 entrada e +nameLen bytes de nome, dobrando os
// buffers quando preciso. false = estourou o teto (RAM ou MENU_ARENA_CAP).
// Como as entradas guardam OFFSET (nao ponteiro), o realloc da arena pode
// mover a memoria sem invalidar nada.
static bool menu_reserve(uint32_t nameLen) {
  if ((uint32_t)nbFiles + 1 > menuEntriesCap) {
    uint32_t cap = menuEntriesCap ? menuEntriesCap * 2 : 128;
    if (cap > MENU_MAX_FILES) cap = MENU_MAX_FILES;
    if ((uint32_t)nbFiles + 1 > cap) return false;
    MenuEntry *p = (MenuEntry*)realloc(menuEntries, cap * sizeof(MenuEntry));
    if (!p) return false;
    menuEntries = p; menuEntriesCap = cap;
  }
  if (menuArenaUsed + nameLen + 1 > menuArenaCap) {
    uint32_t cap = menuArenaCap ? menuArenaCap : 4096;
    while (cap < menuArenaUsed + nameLen + 1) cap *= 2;
    if (cap > MENU_ARENA_CAP) cap = MENU_ARENA_CAP;
    if (menuArenaUsed + nameLen + 1 > cap) return false;
    char *p = (char*)realloc(menuArena, cap);
    if (!p) return false;
    menuArena = p; menuArenaCap = cap;
  }
  return true;
}

static void menu_addEntry(const char *name, bool isDir) {
  uint32_t len = strlen(name);
  if (len > MENU_NAME_MAXLEN) len = MENU_NAME_MAXLEN;
  if (!menu_reserve(len)) return;   // teto atingido: ignora o resto
  uint16_t off = (uint16_t)menuArenaUsed;
  memcpy(menuArena + off, name, len);
  menuArena[off + len] = 0;
  menuArenaUsed += len + 1;
  menuEntries[nbFiles].off   = off;
  menuEntries[nbFiles].isDir = isDir ? 1 : 0;
  nbFiles++;
}

// Le romspath inteiro (uma passada) e ordena. So' no boot, na troca de pasta
// e ao reabrir o menu -- nunca entre teclas.
static int menu_rescan(void) {
  menu_freeCatalog();

  // Aviso de leitura: numa pasta com centenas de arquivos a varredura leva
  // uns instantes. Some assim que a lista e' desenhada por cima.
  video.drawTextNoDma(MENU_FILE_XOFFSET, MENU_FOOTER_YOFFSET,
                      "Lendo cartao...", RGBVAL16(0xff,0xff,0x00),
                      RGBVAL16(0x00,0x00,0x00), false);

  // ".." quando nao estamos na raiz de ROMs.
  if (romsbase[0] && strcmp(romspath, romsbase) != 0)
    menu_addEntry("..", true);

  DIR* dir = opendir(romspath);
  if (!dir) {
    // Sem esta checagem o readdir(NULL) causa LoadProhibited.
    printf("ERRO: nao consegui abrir %s (o diretorio existe no SD?)\n", romspath);
    catalogLoaded = true;   // catalogo valido, so' vazio
    return nbFiles;
  }
  struct dirent* de;
  while ((de = readdir(dir)) != NULL && nbFiles < MENU_MAX_FILES) {
    if (isJunkFile(de->d_name)) continue;
    if (de->d_type == DT_DIR) {
      if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;
      menu_addEntry(de->d_name, true);
    } else if (de->d_type == DT_REG) {
      menu_addEntry(de->d_name, false);
    }
  }
  closedir(dir);

  qsort(menuEntries, nbFiles, sizeof(MenuEntry), menu_cmpEntry);
  catalogLoaded = true;
  printf("Catalogo: %d entradas em %s (%u/%u bytes de nomes)\n",
         nbFiles, romspath, (unsigned)menuArenaUsed, (unsigned)menuArenaCap);
  return nbFiles;
}

// Protótipo: menu_setSelection() e' definido junto do resto do desenho do
// menu, mais abaixo, mas toggleMenu() (acima daquele ponto) ja' o chama ao
// recarregar o catalogo. Sem esta linha o compilador para em toggleMenu.
static void menu_setSelection(void);

static char captureTouchZone(const unsigned short * areas, const unsigned short * actions, int *rx, int *ry, int *rw, int * rh) {
    uint16_t xt=0;
    uint16_t yt=0;
    uint16_t zt=0;
    bool hDir=true;  
  
    if (video.isTouching())
    {
        if (prev_zt == 0) {
            prev_zt =1;
            video.readCal(&xt,&yt,&zt);
            if (zt<1000) {
              prev_zt=0; 
              return ACTION_NONE;
            }
            int i=0;
            int k=0;
            int y2=0, y1=0;
            int x2=0, x1=0;
            int x=KEYBOARD_X,y=KEYBOARD_Y;
            int w=TAREA_W_DEF,h=TAREA_H_DEF;
            uint8_t s;
            while ( (s=areas[i++]) != TAREA_END ) {
                if (s == TAREA_XY) {
                    x = areas[i++];
                    y = areas[i++];                    
                    x2 = x;
                    y2 = y;  
                }
                else if (s == TAREA_WH) {
                    w = areas[i++];
                    h = areas[i++];
                }                     
                else if (s == TAREA_NEW_ROW) {
                  hDir = true;
                  y1 = y2;
                  y2 = y1 + h;
                  x2 = x;
                }  
                else if (s == TAREA_NEW_COL) {
                  hDir = false;
                  x1 = x2;
                  x2 = x1 + w;
                  y2 = y;                  
                }
                else { 
                    if (hDir) {
                      x1 = x2;
                      x2 = x1+s;                                                            
                    } else {
                      y1 = y2;
                      y2 = y1+s;                      
                    }
                    if ( (yt >= y1) && (yt < y2) && (xt >= x1) && (xt < x2)  ) {
                        *rx = x1;
                        *ry = y1;
                        *rw = x2-x1;
                        *rh = y2-y1;
                        return (actions[k]);  
                    }
                    k++;
                }                
            }
        } 
        prev_zt =1; 
    } else {
        prev_zt=0; 
    } 
  
    return ACTION_NONE;   
} 

void toggleMenu(bool on) {
  if (on) {
#ifdef HAS_TDISPLAY_LINK
    link_send_game_name("");   // no game is running
#endif
    callibrationOn=false;
    menuOn=true;
    menuRedraw=true;  
    prevCurFile=-1;   // invalida cache: a proxima chamada faz redraw full
    video.fillScreenNoDma(RGBVAL16(0x00,0x00,0x00));
    // false = fonte pequena (8x8), igual ao resto do menu agora.
    video.drawTextNoDma(0,0, TITLE, RGBVAL16(0x00,0xff,0xff), RGBVAL16(0x00,0x00,0xff), false);
    // Recarrega o catalogo se foi liberado ao rodar o ultimo jogo. No boot
    // ele ja' esta carregado (emu_init), entao aqui nao ha leitura dupla.
    if (!catalogLoaded) { menu_rescan(); menu_setSelection(); }
  } else {
    menuOn = false;    
  }
}


static void callibrationInit(void) 
{
  callibrationOn=true;
  menuOn=false;
  callibrationStep = 0;
  calMinX=0,calMinY=0,calMaxX=0,calMaxY=0;
  video.fillScreenNoDma(RGBVAL16(0xff,0xff,0xff));
  video.drawTextNoDma(0,100, "          Callibration process:", RGBVAL16(0x00,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
  video.drawTextNoDma(0,116, "     Hit the red cross at each corner", RGBVAL16(0x00,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
  video.drawTextNoDma(0,0, "+", RGBVAL16(0xff,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
  prev_zt = 1;  
}

static void readCallibration(void) 
{
  FILE * file = fopen(CALIBRATION_FILE, "rb");
  if (file) {
    fscanf(file,"%d %d %d %d\n",&calMinX,&calMinY,&calMaxX,&calMaxY);
    fclose(file);
    printf("Current callibration params: %d %d %d %d\n",calMinX,calMinY,calMaxX,calMaxY);                
  }
  else {
    printf("Callibration read error\n");
  }  
  video.callibrateTouch(calMinX,calMinY,calMaxX,calMaxY);   
}

static void writeCallibration(void) 
{
  video.callibrateTouch(calMinX,calMinY,calMaxX,calMaxY);
  FILE * file = fopen(CALIBRATION_FILE, "wb");
  if (file) {
    fprintf(file,"%d %d %d %d\n",calMinX,calMinY,calMaxX,calMaxY);
    fclose(file);
  }
  else {
    printf("Callibration write error\n");
  }  
}


bool callibrationActive(void) 
{
  return (callibrationOn);
}



int handleCallibration(uint16_t bClick) {
  uint16_t xt=0;
  uint16_t yt=0;
  uint16_t zt=0;  
  if (video.isTouching()) {
    if (prev_zt == 0) {
      prev_zt = 1;
      video.readRaw(&xt,&yt,&zt);
      if (zt < 1000) {
        return 0;
      }
      switch (callibrationStep) 
      {
        case 0:
          callibrationStep++;
          video.drawTextNoDma(0,0, " ", RGBVAL16(0xff,0xff,0xff), RGBVAL16(0xff,0xff,0xff), true);
          video.drawTextNoDma(VGA_XRES-8,0, "+", RGBVAL16(0xff,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
          calMinX += xt;
          calMinY += yt;          
          break;
        case 1:
          callibrationStep++;
          video.drawTextNoDma(VGA_XRES-8,0, " ", RGBVAL16(0xff,0xff,0xff), RGBVAL16(0xff,0xff,0xff), true);
          video.drawTextNoDma(VGA_XRES-8,VGA_YRES-16, "+", RGBVAL16(0xff,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
          calMaxX += xt;
          calMinY += yt;           
          break;
        case 2:
          callibrationStep++;
          video.drawTextNoDma(VGA_XRES-8,VGA_YRES-16, " ", RGBVAL16(0xff,0xff,0xff), RGBVAL16(0xff,0xff,0xff), true);
          video.drawTextNoDma(0,VGA_YRES-16, "+", RGBVAL16(0xff,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
          calMaxX += xt;
          calMaxY += yt;
          break;
        case 3:
          video.fillScreenNoDma(RGBVAL16(0xff,0xff,0xff));
          video.drawTextNoDma(0,100, "          Callibration done!", RGBVAL16(0x00,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);
          video.drawTextNoDma(0,116, "        (Click center to exit)", RGBVAL16(0xff,0x00,0x00), RGBVAL16(0xff,0xff,0xff), true);           
          callibrationStep++;
          calMinX += xt;
          calMaxY += yt;       
          break;                 
        case 4:
          if ( (xt > (VGA_XRES/4)) && (xt < (VGA_XRES*3)/4) 
            && (yt > (VGA_YRES/4)) && (yt < (VGA_YRES*3)/4) ) {
            calMinX /= 2;
            calMinY /= 2;
            calMaxX /= 2;
            calMaxY /= 2;
            writeCallibration();                       
            toggleMenu(true);
          }
          else {
            callibrationInit();              
          }
          break; 
                           
      }
      vTaskDelay(100 / portTICK_PERIOD_MS); 
    }  
  }
  else {
    prev_zt = 0;
  } 
  return 1; 
}



bool menuActive(void) 
{
  return (menuOn);
}

// ----- Redesenho parcial do menu -----------------------------------------
//
// A pagina e' fixa: topFile = (curFile / MAX_MENULINES) * MAX_MENULINES.
// O cursor anda de 1 em 1 dentro da pagina e so' as DUAS linhas que trocaram
// sao redesenhadas. A pagina inteira so' e' repintada quando o cursor cruza
// a borda -- 1 vez a cada 24 teclas.

static void menu_drawLine(int row, const char *name, bool isDir, bool highlight)
{
  char text[MAX_FILENAME_SIZE+2];
  uint16_t fg = MENU_FILE_FGCOLOR;
  uint16_t bg = MENU_FILE_BGCOLOR;

  if (name == NULL) {
    // Linha vazia (fim da lista numa pagina incompleta).
    memset(text, ' ', MAX_FILENAME_SIZE);
    text[MAX_FILENAME_SIZE] = 0;
  } else {
    if (isDir) {
      // Pastas com "/" na frente e em verde: da' pra distinguir de um .prg
      // de relance, sem precisar entrar para descobrir.
      snprintf(text, sizeof(text), "/%-*.*s",
               MAX_FILENAME_SIZE-1, MAX_FILENAME_SIZE-1, name);
      fg = RGBVAL16(0x60,0xff,0x60);
    } else {
      snprintf(text, sizeof(text), "%-*.*s",
               MAX_FILENAME_SIZE, MAX_FILENAME_SIZE, name);
    }
    if (highlight) { fg = RGBVAL16(0x00,0x00,0x00); bg = RGBVAL16(0xff,0xc0,0x00); }
  }

  // O texto ja' vem preenchido com espacos ate' a largura fixa, e o
  // drawTextNoDma pinta o fundo do glifo -- entao a linha inteira e' repintada
  // pela propria escrita (uma passada de video por linha, nao duas).
  video.drawTextNoDma(MENU_FILE_XOFFSET,
                      row*TEXT_HEIGHT + MENU_FILE_YOFFSET,
                      text, fg, bg, false);
}

// Copia o nome do item sob o cursor para selection[] e guarda se e' pasta.
// E' aqui que o stat() por quadro deixou de ser necessario.
static void menu_setSelection(void)
{
  if (curFile >= 0 && curFile < nbFiles && menuArena) {
    strncpy(selection, ENTRY_NAME(curFile), MENU_NAME_MAXLEN);
    selection[MENU_NAME_MAXLEN] = 0;
    selIsDir = menuEntries[curFile].isDir;
  } else {
    selection[0] = 0;
    selIsDir = false;
  }
}

static void menu_drawHeader(void)
{
  // Caminho atual, na linha livre entre o titulo e a lista.
  char path[40];
  snprintf(path, sizeof(path), "%-38.38s", romspath);
  video.drawTextNoDma(MENU_FILE_XOFFSET, TEXT_HEIGHT, path,
                      RGBVAL16(0x80,0x80,0x80), RGBVAL16(0x00,0x00,0x00), false);
}

static void menu_drawFooter(void)
{
  int page  = (topFile / MAX_MENULINES) + 1;
  int pages = (nbFiles + MAX_MENULINES - 1) / MAX_MENULINES;
  if (pages < 1) pages = 1;
  char body[64], footer[40];
  snprintf(body, sizeof(body), "ENTER=abrir <>=pag F1=SWAP(%d) %d/%d",
           emu_SwapJoysticks(1), page, pages);
  // Largura fixa: sem isto, ao passar de "1/10" para "1/9" sobrava um digito
  // do desenho anterior na tela.
  snprintf(footer, sizeof(footer), "%-38.38s", body);
  video.drawTextNoDma(MENU_FILE_XOFFSET, MENU_FOOTER_YOFFSET, footer,
                      RGBVAL16(0x00,0xff,0xff), RGBVAL16(0x00,0x00,0x00), false);
}

static void menu_drawFullPage(void)
{
  for (int row = 0; row < MAX_MENULINES; row++) {
    int idx = topFile + row;
    if (idx < nbFiles && menuArena)
      menu_drawLine(row, ENTRY_NAME(idx), menuEntries[idx].isDir, idx == curFile);
    else
      menu_drawLine(row, NULL, false, false);
  }
  menu_drawHeader();
  menu_drawFooter();
}

// Troca de diretorio (entrar numa pasta ou subir com "..").
static void menu_changeDir(const char *name)
{
  if (menu_isUpEntry(name)) {
    char *slash = strrchr(romspath, '/');
    if (slash && slash != romspath) *slash = 0;
  } else {
    size_t used = strlen(romspath);
    if (used && romspath[used-1] != '/') {
      if (used + 1 < sizeof(romspath)) { romspath[used++] = '/'; romspath[used] = 0; }
    }
    // strncat com o espaco que REALMENTE sobra: romspath tem 64 bytes e um
    // nome longo de subpasta estourava o buffer no strcpy/strcat originais.
    strncat(romspath, name, sizeof(romspath) - strlen(romspath) - 1);
  }
  curFile     = 0;
  topFile     = 0;
  prevCurFile = -1;          // forca full redraw do novo diretorio
  menu_rescan();
  menu_setSelection();
  menuRedraw = true;
}

int handleMenu(uint16_t bClick)
{
  int action = ACTION_NONE;

  // O stat() que ficava aqui foi removido: era executado a cada iteracao do
  // loop, com ou sem tecla, e cada chamada era um acesso ao SD. O
  // selIsDir ja' vem do catalogo em RAM.
  //
  // captureTouchZone() nunca dispara nesta placa (video.isTouching() e' um
  // stub que sempre devolve false), entao os ramos de toque continuam fora.
  //
  // ENTER (MASK_JOY2_BTN) faz tudo: pasta -> entra; ".." -> sobe; arquivo ->
  // roda. F1 (USER1) so' alterna o SWAP.
  if (bClick & MASK_JOY2_BTN) {
    if (selIsDir) {
      menu_changeDir(selection);
    } else if (nbFiles) {
      action = ACTION_RUN;
    }
  }
  else if (bClick & MASK_JOY2_UP) {
    if (curFile > 0) curFile--;
  }
  else if (bClick & MASK_JOY2_DOWN)  {
    if ((curFile < nbFiles-1) && nbFiles) curFile++;
  }
  // LEFT/RIGHT = pagina inteira. LEFT=cima, RIGHT=baixo.
  else if (bClick & MASK_JOY2_LEFT) {
    if (curFile >= MAX_MENULINES) curFile -= MAX_MENULINES;
    else curFile = 0;
  }
  else if (bClick & MASK_JOY2_RIGHT) {
    if (curFile + MAX_MENULINES < nbFiles) curFile += MAX_MENULINES;
    else if (nbFiles) curFile = nbFiles - 1;
  }
  else if (bClick & MASK_KEY_USER1) {
    emu_SwapJoysticks(0);
    menuRedraw=true;    // rodape mostra o novo estado do SWAP
  }

  // Ao rodar um jogo, libera o catalogo AGORA: selection[] ja' foi copiado
  // (buffer separado da arena), entao a RAM dos nomes volta ao emulador antes
  // do jogo carregar. O menu relê quando reabrir (toggleMenu).
  if (action == ACTION_RUN) {
    menu_freeCatalog();
    return action;
  }

  if (!nbFiles) {
    if (menuRedraw) { menu_drawFullPage(); menuRedraw = false; }
    return action;
  }

  // ---- Redesenho ---------------------------------------------------------
  //   1. menuRedraw setado (troca de pasta, SWAP, primeira entrada) -> full
  //   2. curFile mudou de pagina                                    -> full
  //   3. curFile mudou de linha na mesma pagina                     -> 2 linhas
  //   4. nada mudou                                                 -> NADA
  //
  // O caso 4 e' o que faltava: antes, mesmo sem tecla, o handleMenu ainda
  // pagava o stat() do SD toda vez que era chamado.

  int newTopFile = (curFile / MAX_MENULINES) * MAX_MENULINES;

  if (menuRedraw || newTopFile != topFile || prevCurFile < 0) {
    topFile = newTopFile;
    menu_setSelection();
    menu_drawFullPage();
    menuRedraw  = false;
    prevCurFile = curFile;
  }
  else if (curFile != prevCurFile) {
    int oldRow = prevCurFile - topFile;
    int newRow = curFile     - topFile;
    if (oldRow >= 0 && oldRow < MAX_MENULINES && (topFile+oldRow) < nbFiles) {
      int i = topFile+oldRow;
      menu_drawLine(oldRow, ENTRY_NAME(i), menuEntries[i].isDir, false);
    }
    if (newRow >= 0 && newRow < MAX_MENULINES && (topFile+newRow) < nbFiles) {
      int i = topFile+newRow;
      menu_drawLine(newRow, ENTRY_NAME(i), menuEntries[i].isDir, true);
    }
    menu_setSelection();
    prevCurFile = curFile;
  }

  return action;
}

char * menuSelection(void)
{
  return (selection);  
}
  
#ifdef HAS_I2CKBD
#ifdef USE_WIRE
#else
static esp_err_t i2c_master_read_slave_reg(i2c_port_t i2c_num, uint8_t i2c_addr, uint8_t* data_rd, size_t size)
{
    if (size == 0) {
        return ESP_OK;
    }
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, ( i2c_addr << 1 ) | I2C_MASTER_READ, ACK_CHECK_EN);
    if (size > 1) {
        i2c_master_read(cmd, data_rd, size - 1, (i2c_ack_type_t)ACK_VAL);
    }
    i2c_master_read_byte(cmd, data_rd + size - 1, (i2c_ack_type_t)NACK_VAL);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_num, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}
#endif
#endif


void emu_init(void)
{

  esp_err_t ret = 0;

  printf("mounting sd...\n");

  // IDF 5: o barramento SPI precisa ser inicializado explicitamente antes do
  // sdspi. Na IDF 3 o driver sdspi fazia isso sozinho, e o touch do ILI9341
  // pegava carona (o spi_bus_initialize dele esta comentado em
  // ili9341_t3dma.cpp). Mantemos SPI2/HSPI para preservar esse arranjo:
  // video.touchBegin() mais abaixo faz spi_bus_add_device(HSPI_HOST, ...).
  spi_bus_config_t sd_bus_cfg;
  memset(&sd_bus_cfg, 0, sizeof(sd_bus_cfg));
  sd_bus_cfg.mosi_io_num     = SPIN_NUM_MOSI;
  sd_bus_cfg.miso_io_num     = SPIN_NUM_MISO;
  sd_bus_cfg.sclk_io_num     = SPIN_NUM_CLK;
  sd_bus_cfg.quadwp_io_num   = -1;
  sd_bus_cfg.quadhd_io_num   = -1;
  sd_bus_cfg.max_transfer_sz = 4000;

  ret = spi_bus_initialize(SDSPI_HOST_ID, &sd_bus_cfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    printf("spi_bus_initialize failed: %d\n", (int)ret);
  }

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  // 10 MHz e' agressivo para cartao em soquete de placa de dev. 4 MHz costuma
  // estabilizar; suba de novo depois que tudo estiver funcionando.
  host.max_freq_khz = SD_FREQ_KHZ;
  host.slot = SDSPI_HOST_ID;

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = (gpio_num_t)SPIN_NUM_CS;
  slot_config.host_id = SDSPI_HOST_ID;

  esp_vfs_fat_sdmmc_mount_config_t mount_config;
  memset(&mount_config, 0, sizeof(mount_config));
  mount_config.format_if_mount_failed = false;
  mount_config.max_files              = 5;
  mount_config.allocation_unit_size   = 16 * 1024;

  int tries = 0;
  while((ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card)) != ESP_OK) {
    if (++tries >= 10) {
      // Antes isto era um laco infinito: sem cartao o watchdog acabava
      // matando a task e a placa entrava em boot loop sem explicacao.
      printf("SD: desisti apos %d tentativas (ultimo erro %d). Seguindo sem cartao.\n",
             tries, (int)ret);
      card = NULL;
      break;
    }
    printf("SD: tentativa %d falhou (%d)\n", tries, (int)ret);
    vTaskDelay(500 / portTICK_PERIOD_MS);   
  }
  if (ret == ESP_OK) printf("SD montado a %d kHz\n", (int)SD_FREQ_KHZ);

  // Teclado PS/2 SO' agora, com o SD ja' montado: ps2kbd_begin() le o
  // /sdcard/bootl.rc para pegar os pinos CLK/DAT (fallback 33/32). Se
  // inicializasse antes da montagem, o fopen falharia sempre e cairia no
  // default mesmo com um bootl.rc valido no cartao.
#ifdef HAS_PS2KBD
  ps2kbd_begin();
#endif

  strcpy(romspath,"/sdcard/");
  strcat(romspath,ROMSDIR);
  strcpy(romsbase,romspath);   // limite do ".." -- nao sobe acima da pasta de ROMs
  printf("dir is : %s\n",romspath);

  nbFiles = menu_rescan();
  menu_setSelection();
  printf("SD initialized, files found: %d\n",nbFiles);

 
  video.touchBegin();
  //uint16_t xt=0;
  //uint16_t yt=0;
  //uint16_t zt=0;  
  //video.readRo(&xt,&yt,&zt);


  emu_InitJoysticks();

  // Calibracao de toque removida daqui: a VGA32 nao tem touchscreen
  // (video.isTouching() e' um stub que sempre devolve false), entao
  // readCallibration()/callibrationInit() so' imprimiam "Callibration read
  // error" a cada boot sem servir para nada. Quem decide o que a tela mostra
  // no boot (BASIC direto ou o menu) e' o go.cpp, logo depois desta funcao.

#ifdef HAS_I2CKBD
  uint8_t msg[7]={0,0,0,0,0,0,0};
  
#ifdef USE_WIRE
  Wire.begin(I2C_SDA_IO, I2C_SCL_IO);
  Wire.requestFrom(8, 7, I2C_FREQ_HZ);  // request 5 bytes from slave device #8 
  int i = 0;
  int hitindex=-1;
  while (Wire.available() && (i<7) ) { // slave may send less than requested
    uint8_t b = Wire.read(); // receive a byte
    if (b != 0xff) hitindex=i; 
    msg[i++] = b;        
  }  
#else
  int i2c_master_port = I2C_NUM_1;
  i2c_config_t conf;
  memset(&conf, 0, sizeof(conf));   // IDF 5 tem clk_flags: precisa zerar
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = I2C_SDA_IO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = I2C_SCL_IO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = I2C_FREQ_HZ;
  i2c_param_config((i2c_port_t)i2c_master_port, &conf);
  if (i2c_driver_install((i2c_port_t)i2c_master_port, conf.mode,0, 0, 0) != ESP_OK)
    printf("I2C Failed initialized\n");
  
  if (i2c_master_read_slave_reg( I2C_NUM_1, 8, &msg[0], 7 ) != ESP_OK) 
      printf("I2C Failed \n");    
#endif  




  if ( (msg[0] == 0xff) && (msg[1] == 0xff) && 
       (msg[2] == 0xff) && (msg[3] == 0xff) && 
       (msg[4] == 0xff) && (msg[5] == 0xff) && (msg[6] == 0xff)) {
    i2cKeyboardPresent = true;
    printf("i2C keyboard found\n");            
  }
#endif 
}


void emu_printf(char * text)
{
  printf("%s\n",text);
}


void emu_printi(int val)
{
  printf("%d\n",val);
}

void * emu_Malloc(int size)
{
  void * retval =  malloc(size);
  if (!retval) {
    printf("failled to allocate %d\n",size);
  }
  else {
    printf("could allocate %d\n",size); 
  }
  
  return retval;
}

void emu_Free(void * pt)
{
  free(pt);
}


static FILE * lastfileOpened;


int emu_FileOpen(char * filename)
{
  int retval = 0;

  char filepath[80];
  strcpy(filepath, romspath);
  strcat(filepath, "/");
  strcat(filepath, filename);
  //printf("FileOpen...%s\n",filepath);
    
  lastfileOpened = fopen(filepath, "rb");
  if (lastfileOpened) {
    retval = 1;  
  }
  else {
    //printf("FileOpen failed\n");
  }
  return (retval);
}

int emu_FileRead(char * buf, int size)
{
  int retval = fread(buf, 1, size, lastfileOpened);
  if (retval != size) {
    printf("FileRead failed\n");
  }
  return (retval);     
}

unsigned char emu_FileGetc(void) {
  unsigned char c;
  int retval = fread(&c, 1, 1, lastfileOpened);
  if (retval != 1) {
    printf("emu_FileGetc failed\n");
  }  
  return c; 
}


void emu_FileClose(void)
{
  fclose(lastfileOpened);  
}

int emu_FileSize(char * filename) 
{
  int filesize=0;
  char filepath[80];
  strcpy(filepath, romspath);
  strcat(filepath, "/");
  strcat(filepath, filename);
  printf("FileSize...%s\n",filepath);

  FILE * file = fopen(filepath, "rb");
  if (file) {
    fseek(file, 0L, SEEK_END);
    filesize = ftell(file);
    //fseek(file, 0L, SEEK_SET);
    printf("filesize is...%d\n",filesize);    
    fclose(file);    
  }
 
  return(filesize);  
}

int emu_FileSeek(int seek) 
{
  fseek(lastfileOpened, seek, SEEK_SET);     
  return (seek);
}

int emu_LoadFile(char * filename, char * buf, int size)
{
  int filesize = 0;
    
  char filepath[80];
  strcpy(filepath, romspath);
  strcat(filepath, "/");
  strcat(filepath, filename);
  printf("LoadFile...%s\n",filepath);  

  filesize = emu_FileSize(filename);
  FILE * file = fopen(filepath, "rb");
  if (file) {
    if (size >= filesize)
    {
      if (fread(buf, 1, filesize, file) != filesize) {
        printf("File read failed\n");
      }        
    }
    fclose(file);
  }
  
  return(filesize);
}

int emu_LoadFileSeek(char * filename, char * buf, int size, int seek)
{
  int filesize = 0;
    
  char filepath[80];
  strcpy(filepath, romspath);
  strcat(filepath, "/");
  strcat(filepath, filename);
  printf("LoadFileSeek...%d bytes at %d from %s\n",size,seek,filepath); 

  FILE * file = fopen(filepath, "rb");
  if (file) {
    fseek(file, seek, SEEK_SET);       
    if (fread(buf, size, 1, file) != size) {
      printf("File read failed\n");
    }        
    fclose(file);
  }
  
  return(filesize);
}

static int keypadval=0; 
static bool joySwapped = false;
static uint16_t bLastState;

// ----- Ritmo da navegacao do menu ----------------------------------------
// Ajuste ao gosto: DELAY = espera antes de comecar a repetir ao SEGURAR a
// seta; RATE = intervalo entre repeticoes depois disso.
#define MENU_REP_DELAY_MS  320
#define MENU_REP_RATE_MS    60

static uint16_t menuPrevArrows = 0;
static bool     menuPrevBtn    = false;
static int64_t  menuNextRepUs  = 0;

// Chamado ao entrar/sair do menu (de go.cpp via keys_resync). Sem isto o
// bLastState fica com o valor do ultimo emu_GetMenuKeys(), que pode ter sido
// ha' minutos, e a borda (bCurState & ~bLastState) na primeira leitura do
// menu dispara bits fantasma -- a lista pula sozinha ou uma acao dispara sem
// o usuario tocar em nada.
void emu_ResetKeyState(void) {
  bLastState = emu_ReadKeys();
#ifdef HAS_PS2KBD
  // Esvazia a fila de eventos ao entrar/sair do menu. Sem isto os repeats da
  // propria tecla que abriu o menu (F9) vazavam para a primeira leitura e a
  // lista "pulava sozinha".
  for (int guard = 0; guard < 64; guard++) if (!ps2kbd_get_events()) break;
#endif
  menuPrevArrows = 0;
  menuPrevBtn    = false;
  menuNextRepUs  = 0;
}
static int xRef;
static int yRef;

int emu_ReadAnalogJoyX(int min, int max) 
{
  int val; //adc1_get_raw((adc1_channel_t)PIN_JOY2_A1X);  
  adc2_get_raw((adc2_channel_t)PIN_JOY2_A1X, ADC_WIDTH_BIT_12,&val);
  //printf("refX:%d X:%d\n",xRef,val); 
  val = val-xRef;
  //val = ((val*140)/100);
  if ( (val > -xRef/4) && (val < xRef/4) ) val = 0;
#if INVX
  val = xRef-val;
#else
  val = val+xRef;
#endif  

  return (val*(max-min))/(xRef*2);
}

int emu_ReadAnalogJoyY(int min, int max) 
{
  int val; //= adc1_get_raw((adc1_channel_t)PIN_JOY2_A2Y);
  adc2_get_raw((adc2_channel_t)PIN_JOY2_A2Y, ADC_WIDTH_BIT_12,&val);
  //printf("refY:%d Y:%d\n",yRef,val); 
  val = val-yRef;
  //val = ((val*120)/100);
  if ( (val > -yRef/4) && (val < yRef/4) ) val = 0;
#if INVY
  val = yRef-val;
#else
  val = val+yRef;
#endif  
  return (val*(max-min))/(yRef*2);
}


static uint16_t readAnalogJoystick(void)
{
  uint16_t joysval = 0;

#ifdef NO_ANALOG_JOYSTICK
  // GPIO2 (ADC2_CH2) e' o MISO do SD e GPIO32 e' o DAT do PS/2 nesta placa.
  // Ler daqui atrapalharia os dois. O direcional vem do link T-Display.
  return 0;
#else
  int xReading = emu_ReadAnalogJoyX(0,256);
  if (xReading > 128) joysval |= MASK_JOY2_LEFT;
  else if (xReading < 128) joysval |= MASK_JOY2_RIGHT;
  
  int yReading = emu_ReadAnalogJoyY(0,256);
  if (yReading < 128) joysval |= MASK_JOY2_UP;
  else if (yReading > 128) joysval |= MASK_JOY2_DOWN;
  
  joysval |= ((gpio_get_level((gpio_num_t)PIN_JOY2_BTN) == 1) ? 0 : MASK_JOY2_BTN);

  return (joysval);     
#endif
}


int emu_SwapJoysticks(int statusOnly) {
  if (!statusOnly) {
    if (joySwapped) {
      joySwapped = false;
    }
    else {
      joySwapped = true;
    }
  }
  return(joySwapped?1:0);
}

int emu_GetPad(void) 
{
  return(keypadval|((joySwapped?1:0)<<7));
}

int emu_ReadKeys(void) 
{
  uint16_t retval;
  uint16_t j1 = readAnalogJoystick();

#ifdef HAS_TDISPLAY_LINK
  // OR-merged so the local joystick and the bluetooth pad both drive the game.
  j1 |= link_get_mask();
#endif
#ifdef HAS_PS2KBD
  // Le hotkeys do PS/2 (F1/F5/F6). As setas ja' NAO estao aqui: ficaram so'
  // em s_events, consumido pelo menu -- no jogo elas viram cursor, nao
  // joystick. Ver ps2kbd_poll() em Ps2kbd.cpp para a separacao.
  j1 |= ps2kbd_get_mask();
#endif

  uint16_t j2 = 0;

  // Os bits de ATALHO ficam FORA da troca de joystick.
  //
  // O swap desloca j1 oito bits para a esquerda, e j1 carrega tambem os
  // atalhos do teclado. Com a troca ligada, MASK_KEY_MENU (0x4000) e
  // MASK_KEY_RESET (0x8000) saiam pela borda do uint16_t e viravam ZERO --
  // F9/F10 simplesmente paravam de responder. Pior: MASK_KEY_USER2 (0x40)
  // virava 0x4000, ou seja, MENU, e MASK_KEY_USER3 (0x80) virava 0x8000, ou
  // seja, RESET. Um bit de botao qualquer resetava o C64 do nada.
  //
  // Separamos os atalhos antes de deslocar e recolocamos depois.
  // Tudo que NAO e' joystick analogico/gamepad (bits JOY2 no byte baixo)
  // deve ser separado antes do swap e reinjetado depois:
  //  - hotkeys (USER1..4, MENU, RESET): 0x0020..0x8000 no byte baixo
  //  - joystick de teclado J1: M_JOY1_* no byte alto (0x0100..0x1000)
  // Sem isto o shift do swap joga esses bits para fora do uint16_t.
  const uint16_t hotBits = MASK_KEY_USER1 | MASK_KEY_USER2 | MASK_KEY_USER3 |
                           MASK_KEY_USER4 | MASK_KEY_MENU  | MASK_KEY_RESET |
                           MASK_JOY1_RIGHT| MASK_JOY1_LEFT | MASK_JOY1_UP   |
                           MASK_JOY1_DOWN | MASK_JOY1_BTN;
  uint16_t hot = j1 & hotBits;
  j1 &= ~hotBits;

  if (joySwapped) {
    retval = ((j1 << 8) | j2);
  }
  else {
    retval = ((j2 << 8) | j1);
  }

  retval |= hot;

#ifdef HAS_PS2KBD
  // Joystick por teclado (F12): os bits ja vem posicionados como M_JOY1_*
  // ou M_JOY2_* conforme o modo. Adicionados DEPOIS do swap para nao serem
  // embaralhados -- o usuario escolheu a porta, nao depende do swap global.
  // ps2kbd_get_mask() devolve os bits de joyMode junto com os hotkeys.
#endif

  // Botoes fisicos USER1..4 NAO existem na TTGO VGA32. Os GPIOs 35/34/39/36
  // ficam flutuando (sem pull-up ligado no emu_InitJoysticks), e leitura
  // ocasional voltava 0 = "botao pressionado" fantasma. Quando USER1 + USER2
  // "batiam" ao mesmo tempo, o input_task interpretava como o combo de reset
  // e reiniciava a placa -- sintoma classico: apertar F1 e a placa reseta.
  // O F1 do PS/2 ja injeta MASK_KEY_USER1 direto em j1 acima, entao nao
  // perdemos o comando de LOAD"".
  //if (gpio_get_level((gpio_num_t)PIN_KEY_USER1) == 0 ) retval |= MASK_KEY_USER1;
#ifndef HAS_TDISPLAY_LINK
  //if (gpio_get_level((gpio_num_t)PIN_KEY_USER2) == 0 ) retval |= MASK_KEY_USER2;
#endif
  //if (gpio_get_level((gpio_num_t)PIN_KEY_USER3) == 0 ) retval |= MASK_KEY_USER3;
  //if (gpio_get_level((gpio_num_t)PIN_KEY_USER4) == 0 ) retval |= MASK_KEY_USER4;

  //printf("%d\n",retval);   
  return (retval);
}

unsigned short emu_DebounceLocalKeys(void)
{  
  uint16_t bCurState = emu_ReadKeys();
  uint16_t bClick = bCurState & ~bLastState;
  bLastState = bCurState;

  return (bClick);
}

// Leitura de teclas ESPECIFICA do menu.
//
// O problema que isto resolve: emu_DebounceLocalKeys() faz deteccao de borda
// (bCurState & ~bLastState). Para os botoes fisicos (USER1..4) e para o
// gamepad da T-Display isso e' certo -- um toque = uma acao. Mas para as
// setas do PS/2 o s_mask fica em NIVEL enquanto a tecla esta pressionada,
// entao a borda so' acontece uma vez: segurar a seta nao repetia, e ate'
// mover um item exigia soltar e reapertar varias vezes ("apertar 5x").
//
// Aqui as setas/ENTER vem de ps2kbd_get_events() (cada 'down', incluindo os
// auto-repeats do teclado, conta uma vez) e o resto continua por borda.
unsigned short emu_GetMenuKeys(void)
{
  uint16_t bClick = 0;

#ifdef HAS_PS2KBD
  // Eventos do teclado: setas (nav) e ENTER (MASK_JOY2_BTN). Ja' vem
  // "pulsados", entao entram direto, sem passar pelo edge-detect.
  //
  // O laco DRENA a fila inteira a cada chamada. Antes so' um lote saia por
  // iteracao: quando o loop atrasava (era o stat() do SD), os auto-repeats do
  // teclado se empilhavam e o cursor continuava andando depois da tecla
  // solta -- o "buffer" reportado. Drenando sempre, o atraso nunca vira fila.
  for (int guard = 0; guard < 64; guard++) {
    uint16_t ev = ps2kbd_get_events();
    if (!ev) break;
    bClick |= ev;
  }
#endif

  // Botoes fisicos e gamepad continuam por borda. Mascaramos as setas e o
  // ENTER do estado de nivel para nao competir com os eventos do PS/2 acima
  // (o gamepad da T-Display, se usado, ainda dispara por estes bits via
  // link_get_mask -> s de nivel; para ele a borda esta correta).
  uint16_t bCurState = emu_ReadKeys();
  uint16_t levelClick = bCurState & ~bLastState;
  bLastState = bCurState;

#ifdef HAS_PS2KBD
  // Evita contagem dupla das setas/ENTER quando vieram do PS/2: se o evento
  // ja' pegou, ignoramos a borda de nivel dos mesmos bits nesta leitura.
  const uint16_t navBits = MASK_JOY2_UP|MASK_JOY2_DOWN|MASK_JOY2_LEFT|
                           MASK_JOY2_RIGHT|MASK_JOY2_BTN;
  if (bClick & navBits) levelClick &= ~navBits;
#endif

  // Os atalhos do emulador NAO sao assunto do menu. O go.cpp cuida deles
  // (F9 abre/fecha, F10 reseta). Se a tecla que abriu o menu ainda estiver
  // apertada, ou se o bLastState estiver desatualizado -- ele so' e' escrito
  // aqui, e enquanto o jogo roda ninguem chama esta funcao --, a borda de
  // nivel entrega MASK_KEY_MENU para o handleMenu(), que nao sabe o que
  // fazer com isso.
  levelClick &= ~(MASK_KEY_MENU | MASK_KEY_RESET | MASK_KEY_USER1);

  bClick |= levelClick;

  // ---- Auto-repeat proprio ------------------------------------------------
  // Com a fila drenada acima, segurar a seta produziria um passo por iteracao
  // do loop -- rapido demais para escolher um jogo. O repeat passa a ser
  // NOSSO: o primeiro toque anda na hora, depois espera MENU_REP_DELAY_MS e
  // so' entao repete a cada MENU_REP_RATE_MS. Como e' baseado em relogio, e
  // nao em contagem de eventos, a velocidade nao muda se o loop engasgar.
  const uint16_t arrowBits = MASK_JOY2_UP|MASK_JOY2_DOWN|
                             MASK_JOY2_LEFT|MASK_JOY2_RIGHT;
  uint16_t arrows = bClick & arrowBits;
  int64_t  now    = esp_timer_get_time();

  if (arrows) {
    if (arrows != menuPrevArrows) {          // tecla nova: passa e arma o delay
      menuNextRepUs  = now + (int64_t)MENU_REP_DELAY_MS * 1000;
      menuPrevArrows = arrows;
    } else if (now < menuNextRepUs) {        // repeticao cedo demais: descarta
      bClick &= ~arrowBits;
    } else {
      menuNextRepUs  = now + (int64_t)MENU_REP_RATE_MS * 1000;
    }
  } else {
    menuPrevArrows = 0;
  }

  // ENTER nunca repete: so' a transicao solto->pressionado conta. Sem isto o
  // auto-repeat podia carregar o jogo e, ao voltar ao menu, disparar de novo.
  if (bClick & MASK_JOY2_BTN) {
    if (menuPrevBtn) bClick &= ~MASK_JOY2_BTN;
    else             menuPrevBtn = true;
  } else {
    menuPrevBtn = false;
  }

  return bClick;
}


int emu_ReadI2CKeyboard(void) {
  int retval=0;
#if defined(HAS_PS2KBD) && !PS2_HELD_KEYS
  // Caminho pulsado. Desligado por padrao: com PS2_HELD_KEYS a tecla ja entra
  // direto na matriz em cia1PORTA/PORTB, e usar os dois faria cada tecla
  // contar duas vezes.
  retval = ps2kbd_read_ascii();
  if (retval) return retval;
#endif
#ifdef HAS_I2CKBD 
  if (i2cKeyboardPresent) {
    uint8_t msg[7]; 
#ifdef USE_WIRE
    Wire.requestFrom(8, 7, I2C_FREQ_HZ);    // request 5 bytes from slave device #8 
    int i = 0;
    int hitindex=-1;
    while (Wire.available() && (i<7) ) { // slave may send less than requested
      uint8_t b = Wire.read(); // receive a byte
      if (b != 0xff) hitindex=i; 
      msg[i++] = b;        
    } 
#else
    if (i2c_master_read_slave_reg( I2C_NUM_1, 8, &msg[0], 7 ) != ESP_OK) 
      printf("I2C Failed \n");
    int hitindex=-1;
    int i = 0;
    while (i<7) {
      if (msg[i] != 0xff) hitindex=i;
      i++;
    }
#endif     
    //printf("I2C 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n",
    //  msg[0],msg[1],msg[2],msg[3],msg[4],msg[5],msg[6]);  
    if ((hitindex >=0 ) && (hitindex <=6 )) {
      unsigned short match = ((~msg[hitindex])&0x00FF) | (hitindex<<8);
      for (i=0; i<sizeof(i2ckeys); i++) {
        if (match == i2ckeys[i]) {
          //printf("I2C %d\n",keys[i]);          
          return (keys[i]);
        }
      }
    }    
  }
#endif
  return(retval);
}

void emu_InitJoysticks(void) {  
#ifndef NO_ANALOG_JOYSTICK
  // PIN_JOY2_BTN e' o GPIO32, que nesta placa e' a linha DATA do PS/2.
  // Reconfigurar o pino aqui arranca ele da matriz RTC que a FabGL montou
  // no ps2kbd_begin() -- e o teclado para de responder.
  gpio_set_direction((gpio_num_t)PIN_JOY2_BTN, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_JOY2_BTN, GPIO_PULLUP_ONLY);
#endif
  gpio_set_direction((gpio_num_t)PIN_KEY_USER1, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_KEY_USER1, GPIO_PULLUP_ONLY);
#ifndef HAS_TDISPLAY_LINK
  gpio_set_direction((gpio_num_t)PIN_KEY_USER2, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_KEY_USER2, GPIO_PULLUP_ONLY);
#endif
  gpio_set_direction((gpio_num_t)PIN_KEY_USER3, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_KEY_USER3, GPIO_PULLUP_ONLY);
  gpio_set_direction((gpio_num_t)PIN_KEY_USER4, GPIO_MODE_INPUT);
  gpio_set_pull_mode((gpio_num_t)PIN_KEY_USER4, GPIO_PULLUP_ONLY);

  //adc1_config_channel_atten((adc1_channel_t)PIN_JOY2_A1X,ADC_ATTEN_DB_11);
  //adc1_config_channel_atten((adc1_channel_t)PIN_JOY2_A2Y,ADC_ATTEN_DB_11);
#ifndef NO_ANALOG_JOYSTICK
  adc2_config_channel_atten((adc2_channel_t)PIN_JOY2_A1X,ADC_ATTEN_DB_11);
  adc2_config_channel_atten((adc2_channel_t)PIN_JOY2_A2Y,ADC_ATTEN_DB_11);
#endif
  xRef=0; yRef=0;
#ifndef NO_ANALOG_JOYSTICK
  for (int i=0; i<10; i++) {
    int val;
    adc2_get_raw((adc2_channel_t)PIN_JOY2_A1X, ADC_WIDTH_BIT_12, &val);
    //val = adc1_get_raw((adc1_channel_t)PIN_JOY2_A1X);
    xRef += val;
    adc2_get_raw((adc2_channel_t)PIN_JOY2_A2Y,ADC_WIDTH_BIT_12, &val);
    //val = adc1_get_raw((adc1_channel_t)PIN_JOY2_A2Y); 
    yRef += val;
    vTaskDelay(20 / portTICK_PERIOD_MS); 
  }
  xRef /= 10;
  yRef /= 10;
  printf("refs: %d %d\n",xRef,yRef); 
#else
  printf("joystick analogico desligado (pinos usados por SD e PS/2)\n");
#endif
}



static bool vkbKeepOn = false;
static bool vkbActive = false;
static bool vkeyRefresh=false;
static bool exitVkbd = false;
static uint8_t keyPressCount=0; 


bool virtualkeyboardIsActive(void) {
    return (vkbActive);
}

void toggleVirtualkeyboard(bool keepOn) {     
    if (keepOn) {      
        video.drawSpriteNoDma(0,0,(uint16_t*)logo);
        //prev_zt = 0;
        vkbKeepOn = true;
        vkbActive = true;
        exitVkbd = false;  
    }
    else {
        vkbKeepOn = false;
        if ( (vkbActive) /*|| (exitVkbd)*/ ) {
            video.fillScreenNoDma( RGBVAL16(0x00,0x00,0x00) );
#ifdef DMA_FULLgpio_get_level
            video.begin();
            video.refresh();
#endif                        
            //prev_zt = 0; 
            vkbActive = false;
            exitVkbd = false;
        }
        else {
#ifdef DMA_FULL          
            video.stop();
            video.begin();      
            video.start();
#endif                       
            video.drawSpriteNoDma(0,0,(uint16_t*)logo);           
            //prev_zt = 0;
            vkbActive = true;
            exitVkbd = false;
        }
    }   
}

 
void handleVirtualkeyboard() {
  int rx=0,ry=0,rw=0,rh=0;

    if (keyPressCount == 0) {
      keypadval = 0;      
    } else {
      keyPressCount--;
    }

    if ( (!virtualkeyboardIsActive()) && (video.isTouching()) && (!keyPressCount) ) {
        toggleVirtualkeyboard(false);
        return;
    }
    
    if ( ( (vkbKeepOn) || (virtualkeyboardIsActive())  )  ) {
        char c = captureTouchZone(keysw, keys, &rx,&ry,&rw,&rh);
        if (c) {
            video.drawRectNoDma( rx,ry,rw,rh, KEYBOARD_HIT_COLOR );
            if ( (c >=1) && (c <= ACTION_MAXKBDVAL) ) {
              keypadval = c;
              keyPressCount = 10;
              vTaskDelay(50 / portTICK_PERIOD_MS); 
              vkeyRefresh = true;
              exitVkbd = true;
            }
            else if (c == ACTION_EXITKBD) {
              vkeyRefresh = true;
              exitVkbd = true;  
            }
        }   
     }    
     
    if (vkeyRefresh) {
        vkeyRefresh = false;
        video.drawSpriteNoDma(0,0,(uint16_t*)logo, rx, ry, rw, rh);
    }  
         
    if ( (exitVkbd) && (vkbActive) ) {      
        if (!vkbKeepOn) {             
            toggleVirtualkeyboard(false);
        }
        else {         
            toggleVirtualkeyboard(true);           
        } 
    }     
}

int emu_setKeymap(int index) {
  if (index) {
    //logo = ;
    //keysw = ;      
  }
  else {
    //logo = ;
    //keysw = ;  
  }
  return 0;
}



static unsigned short palette16[PALETTE_SIZE];
static int fskip=0;

void emu_SetPaletteEntry(unsigned char r, unsigned char g, unsigned char b, int index)
{
  if (index<PALETTE_SIZE) {
    //printf("%d: %d %d %d\n", index, r,g,b);
    palette16[index] = RGBVAL16(r,g,b);    
  }
}

void emu_DrawVsync(void)
{
  //printf("sync %d\n",skip);  
  fskip += 1;
  fskip &= VID_FRAME_SKIP;
}

void emu_DrawLine(unsigned char * VBuf, int width, int height, int line) 
{
  if (fskip==0) {
    video.writeLine(width,height,line, VBuf, palette16);
  }
}  

void emu_DrawScreen(unsigned char * VBuf, int width, int height, int stride) 
{  
  if (fskip==0) {
    video.writeScreen(width,height-VBUFFER_YCROP,stride, VBuf+(VBUFFER_YCROP/2)*stride, palette16);
  }
}

int emu_FrameSkip(void)
{
  return fskip;
}

void * emu_LineBuffer(int line)
{
  return (void*)video.getLineBuffer(line);
}

#ifdef HAS_SND
#include "AudioPlaySystem.h"
extern AudioPlaySystem audio;

void emu_sndInit() {
}

void emu_sndPlaySound(int chan, int volume, int freq)
{
  if (chan < 6) {
    audio.sound(chan, freq, volume); 
  } 
}

void emu_sndPlayBuzz(int size, int val) {
  //mymixer.buzz(size,val);  
}
#endif