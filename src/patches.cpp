/*
	Copyright Frank Bösing, 2017

	This file is part of Teensy64.

    Teensy64 is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Teensy64 is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Teensy64.  If not, see <http://www.gnu.org/licenses/>.

    Diese Datei ist Teil von Teensy64.

    Teensy64 ist Freie Software: Sie können es unter den Bedingungen
    der GNU General Public License, wie von der Free Software Foundation,
    Version 3 der Lizenz oder (nach Ihrer Wahl) jeder späteren
    veröffentlichten Version, weiterverbreiten und/oder modifizieren.

    Teensy64 wird in der Hoffnung, dass es nützlich sein wird, aber
    OHNE JEDE GEWÄHRLEISTUNG, bereitgestellt; sogar ohne die implizite
    Gewährleistung der MARKTFÄHIGKEIT oder EIGNUNG FÜR EINEN BESTIMMTEN ZWECK.
    Siehe die GNU General Public License für weitere Details.

    Sie sollten eine Kopie der GNU General Public License zusammen mit diesem
    Programm erhalten haben. Wenn nicht, siehe <http://www.gnu.org/licenses/>.

*/

#include "patches.h"
#include <string.h>


#define DIRECTORY ROMSDIR + "/\0"

static char filename[64];
static char buffer[2];

extern char * menuSelection(void);


/* ===========================================================================
   Suporte a .T64 (Tape containers for C64s)

   Layout exato conforme T64.TXT (Peter Schepers, revisao 1.5):
   https://ist.uwaterloo.ca/~schepers/formats/T64.TXT

     offset 0x00-0x02 : assinatura "C64" (ASCII)
     offset 0x24-0x25 : numero de entradas USADAS no diretorio (low/high)
     offset 0x40 + n*32 : entradas de diretorio, 32 bytes cada
       +0x00 : tipo C64s (0 = livre, 1 = arquivo normal -- so' isto suportamos)
       +0x01 : tipo 1541 (na pratica, != 0 significa PRG)
       +0x02-03 : endereco de carga (load address)
       +0x04-05 : endereco final -- NAO CONFIAVEL. Ha' T64s antigos gerados
                  pelo CONV64 com bug conhecido que grava $C3C6 fixo aqui
                  independente do tamanho real. Nunca usamos este campo.
       +0x08-0B : offset do arquivo dentro do container (32 bits, low/high)
       +0x10-1F : nome do arquivo, 16 bytes, ASCII, padded com espaco ($20,
                  nao $A0 como no disco -- por isso nomes T64 nao podem ter
                  espaco no final)

   Como o formato nao guarda o tamanho de cada arquivo, ele e' inferido pela
   diferenca entre o offset desta entrada e o offset da proxima entrada mais
   proxima no arquivo (ou o fim do arquivo, para a ultima). Por isso lemos o
   diretorio inteiro antes de decidir o tamanho de qualquer entrada.

   Sessao T64: LOAD"" com um .t64 selecionado no menu abre o container e
   carrega a primeira entrada valida. Enquanto esse container continuar
   "aberto" (s_t64Container nao vazio), um LOAD"NOME" subsequente procura
   NOME dentro do MESMO container em vez de tentar abrir "NOME" como arquivo
   solto no cartao -- e' o padrao que jogos multi-parte usam (ex.: o loader
   faz LOAD"PART2",8,1 depois do primeiro LOAD""). LOAD"*" carrega a proxima
   entrada em ordem, tambem um idioma comum em loaders C64.
   =========================================================================== */

#define T64_DIR_ENTRY_SIZE  32
#define T64_DIR_START       0x40
#define T64_MAX_ENTRIES     64    /* qualquer T64 real cabe nisso; sem alloc dinamico */

typedef struct {
  uint16_t loadAddr;
  uint32_t fileOffset;
  char     name[17];
} T64Entry;

static char s_t64Container[64] = "";   /* "" = nenhuma sessao T64 ativa */
static int  s_t64NextIndex = 0;

static int t64IsT64(const char *fname) {
  int len = strlen(fname);
  if (len < 4) return 0;
  const char *ext = fname + len - 4;
  return (ext[0] == '.' &&
          (ext[1] == 't' || ext[1] == 'T') &&
          (ext[2] == '6') &&
          (ext[3] == '4'));
}

/* Compara ignorando maiusc/minusc; nomes T64 ja' vem sem o padding de espacos
   (removido em t64ReadDirectory), entao os dois devem terminar juntos. */
static int t64NamesMatch(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a++, cb = *b++;
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb) return 0;
  }
  return (*a == 0 && *b == 0);
}

/* Le o diretorio inteiro do container ja' identificado por 'container'.
   Devolve o numero de entradas validas lidas (0 se nao for um T64 valido). */
static int t64ReadDirectory(const char *container, T64Entry *entries, int maxEntries) {
  if (emu_FileOpen((char*)container) == 0) return 0;

  uint8_t hdr[T64_DIR_START];
  int gotHdr = (emu_FileRead((char*)hdr, sizeof(hdr)) == (int)sizeof(hdr));

  if (!gotHdr || hdr[0] != 'C' || hdr[1] != '6' || hdr[2] != '4') {
    emu_FileClose();
    return 0;
  }

  int used = hdr[0x24] | (hdr[0x25] << 8);
  if (used > maxEntries) used = maxEntries;

  int count = 0;
  for (int i = 0; i < used; i++) {
    uint8_t e[T64_DIR_ENTRY_SIZE];
    emu_FileSeek(T64_DIR_START + i * T64_DIR_ENTRY_SIZE);
    if (emu_FileRead((char*)e, T64_DIR_ENTRY_SIZE) != T64_DIR_ENTRY_SIZE) break;

    if (e[0] != 1) continue;  /* 0 = entrada livre; so' aceitamos "arquivo normal" */

    T64Entry *t = &entries[count];
    t->loadAddr   = e[2] | (e[3] << 8);
    t->fileOffset = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                    ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    memcpy(t->name, &e[0x10], 16);
    t->name[16] = 0;
    for (int j = 15; j >= 0 && t->name[j] == ' '; j--) t->name[j] = 0;
    count++;
  }

  emu_FileClose();
  return count;
}

/* Tamanho de uma entrada = offset da proxima entrada mais proxima (por
   POSICAO no arquivo, nao por indice de diretorio) menos o offset desta.
   A ultima usa o tamanho do container inteiro. O "endereco final" do
   cabecalho nunca e' usado (ver nota do bug do CONV64 acima). */
static int t64EntrySize(T64Entry *entries, int count, int idx, int containerSize) {
  uint32_t start = entries[idx].fileOffset;
  uint32_t bestNext = (uint32_t)containerSize;
  for (int i = 0; i < count; i++) {
    if (entries[i].fileOffset > start && entries[i].fileOffset < bestNext)
      bestNext = entries[i].fileOffset;
  }
  return (int)(bestNext - start);
}

/* Carrega a entrada 'idx' direto na RAM do C64. Devolve 0 se falhar. */
static int t64LoadEntry(const char *container, T64Entry *entries, int count, int idx) {
  int containerSize = emu_FileSize((char*)container);
  int size = t64EntrySize(entries, count, idx, containerSize);
  if (size < 2) return 0;

  if (emu_FileOpen((char*)container) == 0) return 0;
  emu_FileSeek(entries[idx].fileOffset);

  uint8_t addrBytes[2];
  emu_FileRead((char*)addrBytes, 2);
  uint16_t addr = addrBytes[0] | (addrBytes[1] << 8);

  emu_FileRead((char*)&cpu.RAM[addr], size - 2);
  emu_FileClose();

  cpu.RAM[0xAF] = (addr + size - 2) & 0xff;
  cpu.RAM[0xAE] = (addr + size - 2) / 256;

  printf("T64: \"%s\" carregada em $%04X, %d bytes\n", entries[idx].name, addr, size - 2);
  return 1;
}

/* Ponto de entrada usado pelo patchLOAD(). 'reqName' == NULL ou "*" carrega a
   proxima entrada em ordem; qualquer outro valor busca esse nome no
   diretorio. Devolve 1 se carregou, 0 se nao achou / erro. */
static int t64Load(const char *reqName) {
  T64Entry entries[T64_MAX_ENTRIES];
  int count = t64ReadDirectory(s_t64Container, entries, T64_MAX_ENTRIES);
  if (count == 0) {
    printf("T64: nao consegui ler o diretorio de \"%s\"\n", s_t64Container);
    return 0;
  }

  int idx = -1;
  if (reqName == NULL || strcmp(reqName, "*") == 0) {
    if (s_t64NextIndex < count) idx = s_t64NextIndex;
  } else {
    for (int i = 0; i < count; i++) {
      if (t64NamesMatch(entries[i].name, reqName)) { idx = i; break; }
    }
  }

  if (idx < 0) {
    printf("T64: \"%s\" nao encontrado em \"%s\" (%d entradas)\n",
           reqName ? reqName : "*", s_t64Container, count);
    return 0;
  }

  int ok = t64LoadEntry(s_t64Container, entries, count, idx);
  if (ok) s_t64NextIndex = idx + 1;
  return ok;
}

void patchLOAD(void) {

int device;
int secondaryAddress;
uint16_t addr,size;

	//printf("Patched LOAD\n");
	device = cpu.RAM[0xBA];
	if (device != 1) {
		//Jump to unpatched original address:
		cpu.pc = rom_kernal[cpu.pc - 0xe000 + 1] * 256 + rom_kernal[cpu.pc - 0xe000];
		return;
	};


#if XXX
	if (cpu.RAM[cpu.RAM[0xBC] * 256 + cpu.RAM[0xBB]] == '$' && cpu.RAM[0xB7] == 1) {
		//Directoy listing with LOAD "$"
		printf("Listing of ");
		printf(DIRECTORY);
		printf("\n");
		file = SD.open(DIRECTORY);
		int blocks, start, len;
		addr = cpu.RAM[0x2C] * 256 + cpu.RAM[0x2B];

		/*first line of BASIC listing */
		start = addr;
		cpu.RAM[addr++] = (start + 30) & 0xff;
		cpu.RAM[addr++] = (start + 30) >> 8;
		blocks = 0;
		cpu.RAM[addr++] = blocks & 0xff;
		cpu.RAM[addr++] = blocks >> 8;

		const char title[] = "\x12\"TEENSY64        \" FB " VERSION;
		strcpy((char * )&cpu.RAM[addr], title);
		addr = start + 30;

		while (true) {
			entry = file.openNextFile();
			if (! entry) {
				// no more files
				break;
			}
			int offset;
			if (!entry.isDirectory()) {

				/* Listing to BASIC-RAM */
				start = addr;
				offset = 0;

				//pointer to next line:
				cpu.RAM[addr++] = (start + 32) & 0xff;
				cpu.RAM[addr++] = (start + 32) >> 8;

				//# of blocks
				blocks = ceil((float)entry.size()/256.0f);
				cpu.RAM[addr++] = blocks & 0xff;
				cpu.RAM[addr++] = blocks >> 8;

				if (blocks < 100)   { cpu.RAM[addr++] = ' '; offset++;}
				if (blocks < 10)    { cpu.RAM[addr++] = ' '; offset++; }
			 	cpu.RAM[addr++] = ' ';

				//filename:
				cpu.RAM[addr++] = '"';
				char *s = (char * )&cpu.RAM[addr];
				entry.getName(s, 17);
				while(*s) {*s = toupper(*s); s++;}
				//strcpy((char * )&cpu.RAM[addr], entry.name());
				len = strlen((char * )&cpu.RAM[addr]);

				if (len > 16) len = 16;
				addr += len;
				cpu.RAM[addr++] = '"';

				//fill with space
				while ((addr-start) < (32)) { cpu.RAM[addr++] = ' ';}

				//display "PRG"
				addr = start + 23 + offset;
				cpu.RAM[addr++] = ' ';
				cpu.RAM[addr++] = 'P';
				cpu.RAM[addr++] = 'R';
				cpu.RAM[addr++] = 'G';

				//line-ending
				cpu.RAM[start+31] = 0;
				addr = start + 32;

				/* Listing to serial console */
				itoa (blocks,filename,10);
				len = strlen(filename);
				while (len < 4) { strcat(filename," "); len++; };
				strcat(filename, "\"");
				char nbuf[18] = {0};
				entry.getName(nbuf, 17);
				strcat(filename, nbuf);
				//strcat(filename, entry.getName());
				strcat(filename, "\"");
				len = strlen(filename);
				while (len < 18+4) { strcat(filename," "); len++; };
				strcat(filename," PRG   ");
				//printf(filename);

			}
			entry.close();
		}
		file.close();

        /*add last line to BASIC listing*/
		start = addr;
		cpu.RAM[addr++] = (start + 32) & 0xff;
		cpu.RAM[addr++] = (start + 32) >> 8;
		//# of blocks. todo : determine free space on sd card
		blocks = 65535;
		cpu.RAM[addr++] = blocks & 0xff;
		cpu.RAM[addr++] = blocks >> 8;
		if (blocks < 100) { cpu.RAM[addr++] = ' ';}
		if (blocks < 10)  { cpu.RAM[addr++] = ' ';}
		const char blockfree[] = "BLOCKS FREE.";

		strcpy((char * )&cpu.RAM[addr], blockfree);
		len = strlen(blockfree);
		addr += len;
		while ((addr-start) < (32)) { cpu.RAM[addr++] = ' ';}
		cpu.RAM[start+31] = 0;
		cpu.RAM[start+32] = 0;
		cpu.RAM[start+33] = 0;

		cpu.y = 0x49; //Offset for "LOADING"
		cpu.pc = 0xF12B; //Print and return
		return;
	} // end directory listing
#endif

	//$B7    : Length of file name or disk command
	//$BB-$BC: Pointer to current file name or disk command
    memset(filename,0,sizeof(filename));
    if ( cpu.RAM[0xB7] == 0) {
		strcpy(filename,menuSelection());
    }
    else {
		strncpy(filename, (char*)&cpu.RAM[cpu.RAM[0xBC] * 256 + cpu.RAM[0xBB]], cpu.RAM[0xB7] );
    }     
	secondaryAddress = cpu.RAM[0xB9];

	printf("%s,%d,%d:", filename, device, secondaryAddress);

	// --- Sessao T64 ---------------------------------------------------------
	// LOAD"" com um .t64 selecionado no menu: abre o container e lembra dele.
	// LOAD"NOME" com uma sessao T64 ativa: procura NOME dentro do MESMO
	// container, em vez de tratar "NOME" como um arquivo solto no cartao --
	// e' o padrao que loaders multi-parte usam.
	if (cpu.RAM[0xB7] == 0) {
		s_t64Container[0] = 0;
		if (t64IsT64(filename)) {
			strncpy(s_t64Container, filename, sizeof(s_t64Container) - 1);
			s_t64NextIndex = 0;
		}
	}

	if (s_t64Container[0] != 0) {
		int ok = (cpu.RAM[0xB7] == 0) ? t64Load(NULL) : t64Load(filename);
		if (!ok) {
			printf("not found.\n");
			cpu.pc = 0xf530; //Jump to $F530
			return;
		}
		cpu.y = 0x49; //Offset for "LOADING"
		cpu.pc = 0xF12B; //Print and return
		printf("loaded.\n");
		return;
	}

	if (emu_FileOpen(filename) == 0) {
		printf("not found.\n");
		cpu.pc = 0xf530; //Jump to $F530
		return;
	}

	size = emu_FileSize(filename);
	emu_FileOpen(filename);
	emu_FileRead(buffer, 2);
	addr = buffer[1] * 256 + buffer[0];
	emu_FileRead((char*)&cpu.RAM[addr], size - 2);
	emu_FileClose();

	cpu.RAM[0xAF] = (addr + size - 2) & 0xff;
	cpu.RAM[0xAE] = (addr + size - 2) / 256;

	cpu.y = 0x49; //Offset for "LOADING"
	cpu.pc = 0xF12B; //Print and return
	printf("loaded.\n");

	return;
}

void patchSAVE(void) {
#ifdef XXX
int device;
int secondaryAddress;
uint16_t addr,size;

	Serial.println("Patched SAVE");
	device = cpu.RAM[0xBA];
	if (device != 1) {
		//Jump to unpatched original address:
		cpu.pc = rom_kernal[cpu.pc - 0xe000 + 1] * 256 + rom_kernal[cpu.pc - 0xe000];
		return;
	};

	if (!SDinitialized) {
		cpu.pc = 0xF707; //Device not present error
		Serial.println("SD Card not initialized");
		return;
	}

	if( !SD.exists(DIRECTORY) && SD.mkdir(DIRECTORY) ) {
		cpu.pc = 0xF707; //Device not present error
		Serial.println("SD: Could not create " DIRECTORY);
	}

	//$B7    : Length of file name or disk command
	//$BB-$BC: Pointer to current file name or disk command
	memset(filename,0,sizeof(filename));
	strcpy(filename, DIRECTORY);
	strncat(filename, (char*)&cpu.RAM[cpu.RAM[0xBC] * 256 + cpu.RAM[0xBB]], cpu.RAM[0xB7] );

	secondaryAddress = cpu.RAM[0xB9];

	Serial.print(filename);
	Serial.print(",");
	Serial.print(device);
	Serial.print(",");
	Serial.print(secondaryAddress);
	Serial.print(":");

	addr = cpu.RAM[cpu.a + 1] * 256 + cpu.RAM[cpu.a];
	size = (cpu.y * 256 + cpu.x) - addr;

	buffer[0] = addr & 0xff;
	buffer[1] = addr >> 8;

	if (SD.exists(filename)) SD.remove(filename);
	file = SD.open(filename, FILE_WRITE);
	if (!file) {
		Serial.println ("not possible.");
		cpu.pc = 0xf530; //Jump to $F530
		return;
	}
	file.write(buffer, 2);
	file.write(&cpu.RAM[addr], size);
	file.close();

	if (cpu.RAM[0x9D] & 128) {
		uint16_t pushval = 0xF68D;
		cpu.RAM[BASE_STACK + cpu.sp] = (pushval >> 8) & 0xFF;
		cpu.RAM[BASE_STACK + ((cpu.sp - 1) & 0xFF)] = pushval & 0xFF;
		cpu.sp -= 2;

		cpu.y = 0x51;
		cpu.pc = 0xF12F;
	} else {
		cpu.pc = 0xF68D;
	}

	Serial.println("saved.");
	return;
#endif	
}