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
#include "video_vga.h"


#define DIRECTORY ROMSDIR + "/\0"

static char filename[64];
static char buffer[2];

extern char * menuSelection(void);


/* ===========================================================================
   Suporte a .D64 (imagem do disquete 5.25" do 1541)

   Um .d64 e' o dump byte a byte de um disquete de 35 trilhas do 1541. Nao
   tem cabecalho -- e' 174848 bytes de setores concatenados em ordem, e a
   geometria e' fixa (nao esta no arquivo):

     trilhas 1-17  : 21 setores cada  (0..20)
     trilhas 18-24 : 19 setores cada
     trilhas 25-30 : 18 setores cada
     trilhas 31-35 : 17 setores cada
     total: 683 setores * 256 bytes = 174848 bytes

   Alguns .d64 tem 175531 bytes (com "error bytes" no fim) -- ignoramos, o
   miolo dos primeiros 174848 bytes e' o mesmo.

   Estrutura relevante:
     trilha 18, setor 0  : BAM + nome do disco (nao lemos o BAM aqui)
     trilha 18, setor 1  : primeiro setor do diretorio

   Diretorio (blocos de 256 bytes, encadeados):
     +0-1  : trilha e setor do PROXIMO bloco (00 00 se fim)
     +2..  : 8 entradas de 32 bytes cada:
       +0x02 : tipo do arquivo (bit 7 = "closed"; bits 0-3: 0=DEL, 1=SEQ,
               2=PRG, 3=USR, 4=REL). So' aceitamos PRG (0x82 = fechado+PRG).
       +0x03 : trilha do primeiro setor do arquivo
       +0x04 : setor do primeiro setor do arquivo
       +0x05-0x14 : nome, 16 bytes em PETSCII, padded com $A0
       +0x1E-0x1F : tamanho em blocos de 254 bytes (aproximado)

   Cada bloco de arquivo (256 bytes) tem 2 bytes de link no inicio:
     +0-1 : proxima trilha/setor. Se trilha == 0, o setor indica quantos
            bytes DESTE bloco sao validos (o resto e' padding).
     +2-255 : dados uteis (254 bytes por bloco cheio).

   O primeiro bloco tem, alem dos 2 bytes de link, o load address nos dois
   bytes seguintes (offsets 2 e 3), igual a um PRG. Ou seja, os "dados" do
   PRG comecam realmente no offset 4 do primeiro bloco.
   =========================================================================== */

#define D64_SECTOR_SIZE     256
#define D64_STD_SIZE        174848
#define D64_DIR_TRACK       18
#define D64_DIR_FIRST_SECT  1
#define D64_ENTRIES_PER_BLK 8
#define D64_ENTRY_SIZE      32
#define D64_MAX_ENTRIES     144   /* 18 blocos * 8 entradas, cabe o dir cheio */

typedef struct {
  uint8_t  firstTrack;
  uint8_t  firstSector;
  uint16_t sizeInBlocks;
  char     name[17];
} D64Entry;

static char s_d64Container[64] = "";   /* "" = nenhuma sessao D64 ativa */

static int d64IsD64(const char *fname) {
  int len = strlen(fname);
  if (len < 4) return 0;
  const char *ext = fname + len - 4;
  return (ext[0] == '.' &&
          (ext[1] == 'd' || ext[1] == 'D') &&
          (ext[2] == '6') &&
          (ext[3] == '4'));
}

/* Numero de setores em cada trilha do 1541. Trilhas 1-based; entrada [0]
   nao existe. */
static int d64SectorsInTrack(int track) {
  if (track >= 1  && track <= 17) return 21;
  if (track >= 18 && track <= 24) return 19;
  if (track >= 25 && track <= 30) return 18;
  if (track >= 31 && track <= 35) return 17;
  return 0;
}

/* Offset (em bytes) do inicio de um setor dentro do .d64. */
static int d64SectorOffset(int track, int sector) {
  int off = 0;
  for (int t = 1; t < track; t++) off += d64SectorsInTrack(t) * D64_SECTOR_SIZE;
  return off + sector * D64_SECTOR_SIZE;
}

/* Compara nome ignorando caixa e padding $A0/espaco. */
static int d64NamesMatch(const char *stored, const char *req) {
  int i = 0;
  while (i < 16 && stored[i] && req[i]) {
    char a = stored[i], b = req[i];
    if (a == (char)0xA0 || a == ' ') break;
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return 0;
    i++;
  }
  /* Fim do requerido bate com fim do armazenado (ou padding) */
  if (req[i] != 0) return 0;
  if (i < 16 && stored[i] != 0 && stored[i] != (char)0xA0 && stored[i] != ' ') return 0;
  return 1;
}

/* Le todo o diretorio (percorre a cadeia de blocos a partir de 18/1).
   Devolve o numero de entradas PRG validas. */
static int d64ReadDirectory(const char *container, D64Entry *entries, int maxEntries) {
  if (emu_FileOpen((char*)container) == 0) {
    printf("D64: emu_FileOpen(\"%s\") FALHOU\n", container);
    return 0;
  }

  int count = 0;
  int track = D64_DIR_TRACK, sector = D64_DIR_FIRST_SECT;
  int safety = 40;   /* limite de blocos de dir a percorrer, evita loop */

  while (safety-- > 0 && track != 0) {
    uint8_t blk[D64_SECTOR_SIZE];
    emu_FileSeek(d64SectorOffset(track, sector));
    if (emu_FileRead((char*)blk, D64_SECTOR_SIZE) != D64_SECTOR_SIZE) break;

    /* proxima trilha/setor do dir esta' nos 2 primeiros bytes */
    int nextTrack  = blk[0];
    int nextSector = blk[1];

    for (int i = 0; i < D64_ENTRIES_PER_BLK && count < maxEntries; i++) {
      uint8_t *e = &blk[2 + i * D64_ENTRY_SIZE];
      uint8_t type = e[0];

      /* Aceita so' PRG "closed" (0x82). SEQ/USR/REL/DEL sao ignorados. */
      if (type != 0x82) continue;

      D64Entry *d = &entries[count];
      d->firstTrack   = e[1];
      d->firstSector  = e[2];
      d->sizeInBlocks = e[0x1C] | (e[0x1D] << 8);
      memcpy(d->name, &e[3], 16);
      d->name[16] = 0;
      /* Remove padding $A0 do fim */
      for (int j = 15; j >= 0 && (d->name[j] == (char)0xA0 || d->name[j] == ' '); j--)
        d->name[j] = 0;
      printf("D64:   \"%s\"  T/S=%d/%d  ~%d blocos\n",
             d->name, d->firstTrack, d->firstSector, d->sizeInBlocks);
      count++;
    }

    track = nextTrack;
    sector = nextSector;
  }

  emu_FileClose();
  printf("D64: %d PRG(s) no diretorio\n", count);
  return count;
}

/* Percorre a cadeia de setores do arquivo a partir de (startT,startS) e
   despeja os dados na RAM do C64 a partir de 'addr'. Devolve o total de
   bytes copiados, ou 0 se erro. */
static int d64FollowChain(int startT, int startS, uint16_t addr, int skipHead) {
  int total = 0;
  int track = startT, sector = startS;
  int safety = 720;   /* mais que os 683 setores possiveis, com folga */

  while (safety-- > 0 && track != 0) {
    uint8_t blk[D64_SECTOR_SIZE];
    emu_FileSeek(d64SectorOffset(track, sector));
    if (emu_FileRead((char*)blk, D64_SECTOR_SIZE) != D64_SECTOR_SIZE) {
      printf("D64: erro lendo T/S=%d/%d\n", track, sector);
      return 0;
    }

    int nextTrack  = blk[0];
    int nextSector = blk[1];

    /* Bytes uteis DESTE bloco: se next-track for 0, next-sector diz onde
       terminam os dados validos; caso contrario, o bloco esta' cheio. */
    int useful;
    if (nextTrack == 0) useful = nextSector - 1;   /* -1 porque nextSector aponta o "ultimo byte", 1-based */
    else                useful = D64_SECTOR_SIZE - 2;

    int start = 2 + skipHead;  /* pula link + (opcionalmente) load address do 1o bloco */
    useful -= skipHead;
    skipHead = 0;

    if (useful > 0) {
      /* Nao ultrapassa 64 KB da RAM do C64 */
      if ((int)addr + total + useful > 0x10000) useful = 0x10000 - ((int)addr + total);
      if (useful > 0) {
        memcpy(&cpu.RAM[addr + total], &blk[start], useful);
        total += useful;
      }
    }

    if (nextTrack == 0) break;
    track = nextTrack;
    sector = nextSector;
  }

  return total;
}

/* Carrega a entrada de indice 'idx'. 'relocate' vem do secondary address:
   0 = ignora o load address do arquivo e carrega em $0801 (como KERNAL real
       faz para LOAD"" sem ,8,1);
   1 = respeita o load address embutido nos 2 primeiros bytes do arquivo. */
static int d64LoadEntry(const char *container, D64Entry *entries, int count,
                        int idx, bool relocate) {
  if (idx < 0 || idx >= count) return 0;

  if (emu_FileOpen((char*)container) == 0) return 0;

  /* Le so' o primeiro bloco para descobrir o load address */
  uint8_t blk[D64_SECTOR_SIZE];
  emu_FileSeek(d64SectorOffset(entries[idx].firstTrack, entries[idx].firstSector));
  if (emu_FileRead((char*)blk, D64_SECTOR_SIZE) != D64_SECTOR_SIZE) {
    printf("D64: erro lendo primeiro setor T/S=%d/%d\n",
           entries[idx].firstTrack, entries[idx].firstSector);
    emu_FileClose();
    return 0;
  }

  uint16_t fileAddr = blk[2] | (blk[3] << 8);
  uint16_t addr = relocate ? 0x0801 : fileAddr;

  /* Le a cadeia inteira, pulando link + load address do primeiro bloco. */
  int bytes = d64FollowChain(entries[idx].firstTrack, entries[idx].firstSector,
                             addr, 2 /* skipHead: link+load addr */);
  emu_FileClose();

  if (bytes <= 0) {
    printf("D64: falhou ao ler o arquivo\n");
    return 0;
  }

  cpu.RAM[0xAF] = (addr + bytes) & 0xff;
  cpu.RAM[0xAE] = (addr + bytes) / 256;

  printf("D64: \"%s\" carregado em $%04X (arquivo dizia $%04X%s), %d bytes\n",
         entries[idx].name, addr, fileAddr,
         relocate ? ", relocado p/ BASIC" : "", bytes);
  return 1;
}

/* Ponto de entrada usado pelo patchLOAD().
   'reqName' == NULL     : LOAD"" -> primeira entrada do diretorio (idioma
                           comum para "carrega o que estiver ai").
   'reqName' == "*"      : idem (curinga do KERNAL real).
   'reqName' == "$"      : NAO tratado aqui -- o KERNAL usa isso para listar
                           o diretorio no BASIC. Ficaria bom mas e' outro
                           trabalho (formatar cada entrada como linha BASIC). */
static int d64Load(const char *reqName, bool relocate) {
  D64Entry entries[D64_MAX_ENTRIES];
  int count = d64ReadDirectory(s_d64Container, entries, D64_MAX_ENTRIES);
  if (count == 0) {
    printf("D64: nao consegui ler o diretorio de \"%s\"\n", s_d64Container);
    return 0;
  }

  int idx = -1;
  if (reqName == NULL || strcmp(reqName, "*") == 0 || reqName[0] == 0) {
    idx = 0;   /* primeira entrada */
  } else {
    for (int i = 0; i < count; i++) {
      if (d64NamesMatch(entries[i].name, reqName)) { idx = i; break; }
    }
  }

  if (idx < 0) {
    printf("D64: \"%s\" nao encontrado (%d entradas no diretorio)\n",
           reqName ? reqName : "*", count);
    return 0;
  }

  return d64LoadEntry(s_d64Container, entries, count, idx, relocate);
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

	// --- Sessao D64 ---------------------------------------------------------
	// LOAD"" com um .d64 selecionado no menu: abre a imagem e lembra dela.
	// LOAD"NOME" com uma sessao D64 ativa: procura NOME dentro da MESMA
	// imagem, em vez de tratar "NOME" como arquivo solto no cartao. E' o
	// idioma dos loaders multi-parte (LOAD"PART2",8,1 depois do LOAD"").
	if (cpu.RAM[0xB7] == 0) {
		s_d64Container[0] = 0;
		if (d64IsD64(filename)) {
			strncpy(s_d64Container, filename, sizeof(s_d64Container) - 1);
		}
	}

	if (s_d64Container[0] != 0) {
		// secondary=0 -> relocate para $0801 (LOAD sem ,8,1); secondary!=0 ->
		// respeita endereco do arquivo (LOAD"nome",8,1). Igual ao KERNAL real.
		bool relocate = (secondaryAddress == 0);
		int ok = (cpu.RAM[0xB7] == 0) ? d64Load(NULL, relocate)
		                              : d64Load(filename, relocate);
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

	// Nao aplicamos a relocacao secondary=0 -> $0801 aqui (como faz o KERNAL
	// real). E' "errado", mas na pratica funciona MELHOR: os PRGs que circulam
	// costumam ser autocontidos e esperam ser carregados no seu proprio
	// endereco, e um LOAD"" seguido de RUN normalmente encontra um stub
	// BASIC 10 SYS xxxx que faz o pulo. Aplicar a regra do KERNAL fazia o
	// exolon.prg (que carregava em endereco != $0801) dar ?SYNTAX ERROR IN
	// 12544, porque o RUN via lixo em $0801. No D64 aplicamos a regra do
	// KERNAL (la' faz diferenca), aqui no PRG solto deixamos como estava.

	emu_FileRead((char*)&cpu.RAM[addr], size - 2);
	emu_FileClose();

	cpu.RAM[0xAF] = (addr + size - 2) & 0xff;
	cpu.RAM[0xAE] = (addr + size - 2) / 256;

	printf("PRG: carregado em $%04X, %d bytes\n", addr, size - 2);

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