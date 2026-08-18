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

#define MAX_FILENAME_SIZE   28

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
static char selection[MAX_FILENAME_SIZE+1]="";
static uint8_t prev_zt=0; 

// "._Nome" e' o arquivo de recurso (AppleDouble) que o macOS cria toda vez
// que copia algo para um volume FAT/exFAT -- some cartao acaba cheio deles,
// um para cada arquivo de verdade. Sem este filtro eles aparecem no menu
// como entradas invalidas (nao carregam nada, so' confundem a lista).
static inline bool isJunkFile(const char *name) {
  return (name[0] == '.' && name[1] == '_');
}

static int readNbFiles(void) {
  int totalFiles = 0;

  DIR* dir = opendir(romspath);
  if (!dir) {
    // Sem esta checagem o readdir(NULL) causa LoadProhibited. Acontece
    // quando o diretorio nao existe no cartao.
    printf("ERRO: nao consegui abrir %s (o diretorio existe no SD?)\n", romspath);
    return 0;
  }
  while (true) {
    struct dirent* de = readdir(dir);
    if (!de) {
      // no more files
      break;
    }    
    if (isJunkFile(de->d_name)) continue;
    if (de->d_type == DT_REG) {
      totalFiles++;
    }
    else if (de->d_type == DT_DIR) {
      if ( (strcmp(de->d_name,".")) && (strcmp(de->d_name,"..")) ) {
        totalFiles++;
      }
    }  
  }
  closedir(dir);
  printf("Directory read: %d files",totalFiles);
  return totalFiles;  
}

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
    video.fillScreenNoDma(RGBVAL16(0x00,0x00,0x00));
    // false = fonte pequena (8x8), igual ao resto do menu agora.
    video.drawTextNoDma(0,0, TITLE, RGBVAL16(0x00,0xff,0xff), RGBVAL16(0x00,0x00,0xff), false);  
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

int handleMenu(uint16_t bClick)
{
  int action = ACTION_NONE;

  char newpath[80];
  strcpy(newpath, romspath);
  strcat(newpath, "/");
  strcat(newpath, selection);
  
  struct stat st;
  bool newPathIsDir = false;
  if(stat(newpath,&st) == 0)
    if((st.st_mode & S_IFDIR) != 0)
      newPathIsDir = true;

  // captureTouchZone() nunca dispara nesta placa (video.isTouching() e' um
  // stub que sempre devolve false -- nao ha touchscreen na VGA32), entao os
  // ramos que dependiam dela (atalho numerico 1-9, setas via toque) foram
  // removidos daqui. A navegacao inteira vem do PS/2 e do gamepad da
  // ponte T-Display, os dois entrando em bClick via MASK_JOY2_*.
  //
  // ENTER (MASK_JOY2_BTN) faz tudo: se o item selecionado for pasta, entra
  // nela; se for arquivo, roda. O F1 (USER1) so' alterna o SWAP -- antes ele
  // tambem entrava em pasta, o que era redundante com o ENTER e confuso.
  if ( (bClick & MASK_JOY2_BTN) && newPathIsDir ) {
      menuRedraw=true;
      strcpy(romspath,newpath);
      curFile = 0;
      nbFiles = readNbFiles();     
  }
  else if ( (bClick & MASK_JOY2_BTN) ) {
      menuRedraw=true;
      action = ACTION_RUN;       
  }
  else if (bClick & MASK_JOY2_UP) {
    if (curFile!=0) {
      menuRedraw=true;
      curFile--;
    }
  }
  else if (bClick & MASK_JOY2_DOWN)  {
    if ((curFile<(nbFiles-1)) && (nbFiles)) {
      curFile++;
      menuRedraw=true;
    }
  }
  // LEFT/RIGHT = pagina inteira (util com muitos arquivos). Antes RIGHT
  // pulava PRA CIMA e LEFT pulava PRA BAIXO -- herdado do layout de toque
  // removido, mas contraintuitivo no teclado. Agora LEFT=cima, RIGHT=baixo.
  else if (bClick & MASK_JOY2_LEFT) {
    if ((curFile-MAX_MENULINES)>=0) {
      menuRedraw=true;
      curFile -= MAX_MENULINES;
    } else if (curFile!=0) {
      menuRedraw=true;
      curFile=0;
    }
  }
  else if (bClick & MASK_JOY2_RIGHT) {
    if ((curFile<(nbFiles-MAX_MENULINES)) && (nbFiles)) {
      curFile += MAX_MENULINES;
      menuRedraw=true;
    }
    else if ((curFile<(nbFiles-1)) && (nbFiles)) {
      curFile = nbFiles-1;
      menuRedraw=true;
    }
  }
  else if (bClick & MASK_KEY_USER1) {
    emu_SwapJoysticks(0);
    menuRedraw=true;  
  }   

    
  if (menuRedraw && nbFiles) {
         
    int fileIndex = 0;
    DIR* dir = opendir(romspath);
    if (!dir) {
      printf("ERRO: nao consegui abrir %s\n", romspath);
      return (action);
    }
    
    video.drawRectNoDma(MENU_FILE_XOFFSET,MENU_FILE_YOFFSET, MENU_FILE_W, MENU_FILE_H, MENU_FILE_BGCOLOR);
    if (curFile <= (MAX_MENULINES-1)) topFile=0;
    else topFile=curFile-(MAX_MENULINES/2);
    
    int i=0;
    while (i<MAX_MENULINES) {
      struct dirent* de = readdir(dir);
      if (!de) {
        break;
      }     
      if (isJunkFile(de->d_name)) continue;
      if ( (de->d_type == DT_REG) || ((de->d_type == DT_DIR) && (strcmp(de->d_name,".")) && (strcmp(de->d_name,"..")) ) ) {
        if (fileIndex >= topFile) {              
          if ((i+topFile) < nbFiles ) {
            if ((i+topFile)==curFile) {
              video.drawTextNoDma(MENU_FILE_XOFFSET,i*TEXT_HEIGHT+MENU_FILE_YOFFSET, de->d_name, RGBVAL16(0xff,0xff,0x00), RGBVAL16(0xff,0x00,0x00), false);
              strncpy(selection,de->d_name,MAX_FILENAME_SIZE);
              selection[MAX_FILENAME_SIZE]=0;
            }
            else {
              video.drawTextNoDma(MENU_FILE_XOFFSET,i*TEXT_HEIGHT+MENU_FILE_YOFFSET, de->d_name, MENU_FILE_FGCOLOR, MENU_FILE_BGCOLOR, false);      
            }
          }
          i++; 
        }
        fileIndex++;    
      }
    }
    closedir(dir);

    // ENTER abre pasta ou roda arquivo; F1 so' alterna o SWAP.
    char footer[41];
    snprintf(footer, sizeof(footer), "ENTER=abrir/rodar ARROWS=nav F1=SWAP(%d)",
             emu_SwapJoysticks(1));
    video.drawTextNoDma(MENU_FILE_XOFFSET, MENU_FOOTER_YOFFSET, footer,
                        RGBVAL16(0x00,0xff,0xff), RGBVAL16(0x00,0x00,0x00), false);

    menuRedraw=false;     
  }


  return (action);  
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

#ifdef HAS_PS2KBD
  ps2kbd_begin();
#endif

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

  strcpy(romspath,"/sdcard/");
  strcat(romspath,ROMSDIR);
  printf("dir is : %s\n",romspath);

  nbFiles = readNbFiles(); 
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

  if (joySwapped) {
    retval = ((j1 << 8) | j2);
  }
  else {
    retval = ((j2 << 8) | j1);
  }

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
  bClick |= ps2kbd_get_events();
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

  bClick |= levelClick;
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