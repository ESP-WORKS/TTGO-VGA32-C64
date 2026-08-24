/*
  sidlite -- ver cabecalho em sidlite.h para o contexto da adaptacao.

  Adaptado de T-HMI-C64 (retroelec, GPLv3), src/SID.cpp.

  --- v2: otimizacoes de custo de CPU ---------------------------------------

  A v1 derrubou o FPS de ~48 para ~39. Mudancas desta versao, em ordem de
  ganho esperado:

  1. -O2 apenas neste arquivo, via atributo de funcao. O projeto usa -Os
     porque o gargalo geral e' busca de instrucao na flash, mas o loop de
     audio e' pequeno e roda 22050 vezes por segundo: e' exatamente o caso
     em que inlining e desenrolamento pagam.
  2. Divisao de float ELIMINADA do caminho quente. O FPU do LX6 nao tem
     instrucao de divisao -- cada `/` vira uma chamada a __divsf3 em
     software, dezenas de ciclos. Havia uma por amostra. Trocada por
     multiplicacao por reciproco de tabela.
  3. fabsf() removida da onda triangular. Com -Os podia virar chamada de
     biblioteca; a forma com comparacao e' equivalente e nao chama nada.
  4. Ganho final pre-calculado. Eram duas multiplicacoes por amostra
     (c64Volume * emuVolume); agora sao recalculadas so' quando $D418 ou o
     volume do emulador mudam.
  5. Saida rapida quando as tres vozes estao em IDLE: zera o resto do bloco
     e sai. Cobre todo o tempo em que nao ha som -- menus, telas de load,
     jogos sem musica. O oscilador deixa de correr livre com o gate
     fechado, o que na teoria muda a fase inicial da proxima nota; na
     pratica e' inaudivel.
*/

#include "sidlite.h"
#include "AudioPlaySystem.h"   /* DEFAULT_SAMPLERATE */
#include <math.h>
#include <string.h>

#ifndef DEFAULT_SAMPLERATE
#define DEFAULT_SAMPLERATE 22050
#endif

#define SR ((float)DEFAULT_SAMPLERATE)

/* ==========================================================================
   INTERRUPTOR DE MEDICAO

   Em 1, o sidlite_fill() apenas zera o buffer e retorna. Todo o resto do
   sistema fica identico: o I2S continua rodando, o step() continua pedindo
   441 amostras por frame, o dispatch de $D400 continua sendo chamado. A
   unica coisa que some e' o custo de gerar as amostras.

   O FPS medido com 1 e' o teto do que otimizar este arquivo pode render.
   Comparar com o FPS medido com 0 da' o custo real do SID, sem hipotese.
   ========================================================================== */
#define SIDLITE_MUTE 0

/* -O2 so' aqui. Ver nota 1 no cabecalho. */
#define HOT __attribute__((optimize("O2")))

/* Ganho final. sample e' normalizado em [-1,1] e multiplicado pelo volume do
   C64 ($D418, 0..1) e por este ganho. 255*110 = 28050, deixa ~14% de folga
   ate' o fundo de escala do int16 -- o caminho do I2S soma +32767 depois e
   um clip ali viraria estalo. */
#define VOLUME_GAIN 110.0f

/* Tempos de ataque e de decaimento/release do 6581, em segundos, indexados
   pelos nibbles de $D405/$D406. */
static const float attackLUT[16] = {
    0.002f, 0.008f, 0.016f, 0.024f, 0.038f, 0.056f, 0.068f, 0.080f,
    0.100f, 0.250f, 0.500f, 0.800f, 1.000f, 3.000f, 5.000f, 8.000f};

static const float releaseDecayLUT[16] = {
    0.008f, 0.024f, 0.048f, 0.072f, 0.114f, 0.168f, 0.204f, 0.240f,
    0.300f, 0.750f, 1.500f, 2.400f, 3.000f, 9.000f, 15.00f, 24.00f};

/* Reciprocos de 0..4, para nao dividir no caminho quente. Ver nota 2. */
static const float invN[5] = {0.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f};

enum ADSRState { ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE, ADSR_IDLE };

struct Voice {
  float phase;
  float phaseInc;
  float pulseWidth;
  float sustainVol;
  float attackAdd;
  float decayAdd;
  float releaseAdd;
  float envelope;
  float sample;
  float noiseValue;
  uint32_t lfsr;
  uint8_t control;
  uint8_t regAD;          /* guardado para recalcular decayAdd quando SR mudar */
  uint8_t adsrState;
  bool syncNextVoice;
  bool ringmod;
  Voice *nextVoice;
  Voice *prevVoice;
};

static Voice v[3];
static uint8_t sidreg[0x20];
static float c64Volume;         /* $D418 nibble baixo, 0..1 */
static float emuVolume;         /* ganho do emulador, ja' multiplicado */
static float finalGain;         /* c64Volume * emuVolume, pre-calculado */
static uint8_t emuVolumeScaled; /* 0..255, o que a UI mostra */
static bool voice3silent;       /* $D418 bit 7 */
static uint32_t rndState = 0x12345678u;

static inline void recalcGain(void) { finalGain = c64Volume * emuVolume; }

/* ---------------------------------------------------------------- voz --- */

static void voiceInit(Voice *o) {
  o->phase = 1.0f;
  o->phaseInc = 0.0f;
  o->pulseWidth = 0.0f;
  o->sustainVol = 0.0f;
  o->attackAdd = 0.0f;
  o->decayAdd = 0.0f;
  o->releaseAdd = 0.0f;
  o->envelope = 0.0f;
  o->sample = 0.0f;
  o->noiseValue = 0.0f;
  o->lfsr = 0x7FFFF8u;
  o->control = 0;
  o->regAD = 0;
  o->adsrState = ADSR_IDLE;
  o->syncNextVoice = false;
  o->ringmod = false;
}

static inline bool voiceActive(const Voice *o) { return o->adsrState != ADSR_IDLE; }

/* $D400/$D401 -- frequencia. O passo de fase e' freq * fclk / 2^24 / SR. */
static void voiceSetFreq(Voice *o, uint16_t freq) {
  o->phaseInc = (float)freq * 985248.0f / 16777216.0f / SR;
}

/* $D402/$D403 -- largura do pulso, 12 bits. */
static void voiceSetPW(Voice *o, uint16_t pw) {
  o->pulseWidth = (float)(pw & 0x0fff) / 4095.0f;
}

/* $D406 -- sustain/release. Recalcula tambem o decayAdd, que depende do
   sustain: no original o decayAdd so' era recalculado na escrita de $D405,
   entao um jogo que escrevesse SR depois de AD (a ordem mais comum, porque
   os registradores sao percorridos em ordem crescente) ficava com o
   decaimento errado para sempre. */
static void voiceSetSR(Voice *o, uint8_t val) {
  o->sustainVol = (float)(val & 0x0f) / 15.0f;
  float t = releaseDecayLUT[val & 0x0f];
  o->releaseAdd = (o->sustainVol > 0.0f) ? (o->sustainVol / (t * SR))
                                         : (1.0f / (t * SR));
  o->decayAdd = (1.0f - o->sustainVol) / (releaseDecayLUT[o->regAD & 0x0f] * SR);
}

/* $D405 -- attack/decay. */
static void voiceSetAD(Voice *o, uint8_t val) {
  o->regAD = val;
  o->attackAdd = 1.0f / (attackLUT[(val >> 4) & 0x0f] * SR);
  o->decayAdd = (1.0f - o->sustainVol) / (releaseDecayLUT[val & 0x0f] * SR);
}

/* $D404 -- control. Gate, sync, ringmod, test e forma de onda. */
static void voiceSetControl(Voice *o, uint8_t val) {
  uint8_t oldControl = o->control;
  o->control = val;

  bool oldGate = (oldControl & 0x01) != 0;
  bool newGate = (val & 0x01) != 0;

  /* bit 0 -- gate: borda de subida dispara o ataque, descida o release */
  if (!oldGate && newGate) {
    o->adsrState = ADSR_ATTACK;
    o->envelope = 0.0f;
  } else if (oldGate && !newGate) {
    o->adsrState = ADSR_RELEASE;
  }

  /* bit 1 -- sync: a voz anterior passa a resetar a fase desta.
     As vozes estao encadeadas em circulo (voz 3 -> voz 1), entao ao
     contrario do original a voz 1 tambem pode ser sincronizada pela voz 3,
     que e' o comportamento do chip real, e sem risco de ponteiro nulo. */
  o->prevVoice->syncNextVoice = (val & 0x02) != 0;

  /* bit 2 -- ringmod */
  o->ringmod = (val & 0x04) != 0;

  /* bit 3 -- test: congela o oscilador e recarrega o LFSR */
  if (!(oldControl & 0x08) && (val & 0x08)) {
    o->phase = 0.0f;
    o->lfsr = 0x7FFFF8u;
  }

  /* bits 4-7 -- forma de onda: trocar a onda com o gate aberto reinicia */
  if (newGate && ((oldControl & 0xf0) != (val & 0xf0))) {
    o->adsrState = ADSR_ATTACK;
    o->phase = 0.0f;
  }
}

static inline float voiceUpdateEnvelope(Voice *o) {
  switch (o->adsrState) {
  case ADSR_ATTACK:
    o->envelope += o->attackAdd;
    if (o->envelope >= 1.0f) {
      o->envelope = 1.0f;
      o->adsrState = ADSR_DECAY;
    }
    break;
  case ADSR_DECAY:
    o->envelope -= o->decayAdd;
    if (o->envelope <= o->sustainVol) {
      o->envelope = o->sustainVol;
      o->adsrState = ADSR_SUSTAIN;
    }
    break;
  case ADSR_SUSTAIN:
    break;
  case ADSR_RELEASE:
    o->envelope -= o->releaseAdd;
    if (o->envelope <= 0.0f) {
      o->envelope = 0.0f;
      o->adsrState = ADSR_IDLE;
    }
    break;
  default:
    o->envelope = 0.0f;
    break;
  }
  return o->envelope;
}

static inline void voiceNextLFSR(Voice *o) {
  uint32_t bit22 = (o->lfsr >> 22) & 1u;
  uint32_t bit17 = (o->lfsr >> 17) & 1u;
  o->lfsr = (o->lfsr << 1) | (bit22 ^ bit17);
}

/* Os 12 bits do ruido nao sao contiguos no LFSR de 23 bits -- o SID puxa
   taps espalhados. Normalizado para [-1,1). */
static inline float voiceNoise(const Voice *o) {
  uint32_t l = o->lfsr;
  uint16_t n = (uint16_t)((((l >> 22) & 1u) << 11) | (((l >> 20) & 1u) << 10) |
                          (((l >> 16) & 1u) << 9)  | (((l >> 13) & 1u) << 8)  |
                          (((l >> 11) & 1u) << 7)  | (((l >> 7)  & 1u) << 6)  |
                          (((l >> 6)  & 1u) << 5)  | (((l >> 3)  & 1u) << 4)  |
                          (((l >> 1)  & 1u) << 3)  | (((l >> 0)  & 1u) << 2)  |
                          (((l >> 18) & 1u) << 1)  | (((l >> 14) & 1u) << 0));
  /* 1/2047.5 pre-calculado: o FPU do LX6 nao divide. */
  return ((float)n * 0.00048840048840f) - 1.0f;
}

static inline float voiceGenerate(Voice *o, bool active) {
  /* Avanca a fase. Subtrai 1.0 em vez de zerar: zerar descarta a parte
     fracionaria acumulada e desafina progressivamente as notas agudas.
     O passo maximo do SID (~3,9 kHz) cabe em 0,18 a 22050 Hz, entao um
     unico subtrai basta. */
  o->phase += o->phaseInc;
  if (o->phase >= 1.0f) {
    o->phase -= 1.0f;
    if (o->syncNextVoice) {
      o->nextVoice->phase = 0.0f;
    }
  }

  float s = 0.0f;
  uint8_t wavecnt = 0;
  uint8_t ctrl = o->control;

  if (active) {
    if (ctrl & 0x10) { /* triangular -- sem fabsf, ver nota 3 */
      float ph = o->phase;
      float half = (ph < 0.5f) ? (0.5f - ph) : (ph - 0.5f);
      float tri = 4.0f * half - 1.0f;
      if (o->ringmod && o->prevVoice->phase >= 0.5f) tri = -tri;
      s += tri;
      wavecnt++;
    }
    if (ctrl & 0x20) { /* dente de serra */
      s += 2.0f * o->phase - 1.0f;
      wavecnt++;
    }
    if (ctrl & 0x40) { /* pulso */
      s += (o->phase < o->pulseWidth) ? 1.0f : -1.0f;
      wavecnt++;
    }
  }

  if (ctrl & 0x80) { /* ruido -- o LFSR avanca a cada ciclo da fase */
    if (o->phase < o->phaseInc) {
      voiceNextLFSR(o);
      if (active) o->noiseValue = voiceNoise(o);
    }
    if (active) {
      s += o->noiseValue;
      wavecnt++;
    }
  }

  /* Combinar formas de onda no SID real e' um AND entre tabelas, nao uma
     soma. A media e' uma aproximacao grosseira, mas so' importa nos poucos
     jogos que ligam duas ondas ao mesmo tempo. */
  if (wavecnt > 1) s *= invN[wavecnt];

  o->sample = s;
  return s;
}

/* --------------------------------------------------------------- API --- */

void sidlite_init(void) {
  memset(sidreg, 0, sizeof(sidreg));
  c64Volume = 0.0f;
  voice3silent = false;
  for (int i = 0; i < 3; i++) voiceInit(&v[i]);
  /* encadeamento circular: cada voz conhece a anterior (ringmod, sync) e a
     seguinte (alvo do sync) */
  v[0].nextVoice = &v[1]; v[1].nextVoice = &v[2]; v[2].nextVoice = &v[0];
  v[0].prevVoice = &v[2]; v[1].prevVoice = &v[0]; v[2].prevVoice = &v[1];
  if (emuVolumeScaled == 0) sidlite_setVolume(255);
  recalcGain();
}

void sidlite_setVolume(uint8_t vol) {
  emuVolumeScaled = vol;
  emuVolume = (float)vol * VOLUME_GAIN;
  recalcGain();
}

uint8_t sidlite_getVolume(void) { return emuVolumeScaled; }

void sidlite_write(uint16_t addr, uint8_t val) {
  /* $D400-$D7FF espelha o bloco de 32 registradores */
  uint8_t idx = (uint8_t)(addr & 0x1f);
  sidreg[idx] = val;

  if (idx <= 0x14) {
    uint8_t voice = idx / 7;      /* 0x00-0x06, 0x07-0x0D, 0x0E-0x14 */
    switch (idx % 7) {
    case 0:
    case 1:
      voiceSetFreq(&v[voice], (uint16_t)(sidreg[voice * 7] |
                                         (sidreg[1 + voice * 7] << 8)));
      break;
    case 2:
    case 3:
      voiceSetPW(&v[voice], (uint16_t)(sidreg[2 + voice * 7] |
                                       (sidreg[3 + voice * 7] << 8)));
      break;
    case 4: voiceSetControl(&v[voice], val); break;
    case 5: voiceSetAD(&v[voice], val); break;
    case 6: voiceSetSR(&v[voice], val); break;
    }
  } else if (idx == 0x18) {
    c64Volume = (float)(val & 0x0f) / 15.0f;
    voice3silent = (val & 0x80) != 0;
    recalcGain();
  }
  /* $D415-$D417 (filtro) sao guardados em sidreg[] mas nao emulados */
}

uint8_t sidlite_read(uint16_t addr) {
  uint8_t idx = (uint8_t)(addr & 0x1f);
  if (idx == 0x1b) {
    /* $D41B -- saida da voz 3. Jogos usam como gerador de numeros
       aleatorios, com a voz 3 em ruido e silenciada pelo bit 7 de $D418. */
    rndState = rndState * 1664525u + 1013904223u;
    return (uint8_t)(rndState >> 24);
  }
  if (idx == 0x1c) {
    /* $D41C -- envelope da voz 3, usado para modulacao. O original fazia
       o cast para uint8_t ANTES de multiplicar por 255, o que so' podia
       devolver 0 ou 1. */
    float e = v[2].envelope;
    if (e < 0.0f) e = 0.0f;
    if (e > 1.0f) e = 1.0f;
    return (uint8_t)(e * 255.0f);
  }
  return sidreg[idx];
}

HOT void sidlite_fill(int16_t *out, int nsamples) {
#if SIDLITE_MUTE
  memset(out, 0, (size_t)nsamples * sizeof(int16_t));
  return;
#else
  Voice *v0 = &v[0], *v1 = &v[1], *v2 = &v[2];
  float gain = finalGain;

  for (int n = 0; n < nsamples; n++) {
    bool a0 = voiceActive(v0);
    bool a1 = voiceActive(v1);
    bool a2 = voiceActive(v2);
    uint8_t cnt = (uint8_t)(a0 + a1 + a2);

    /* Saida rapida: nenhuma voz soando. Cobre menus, telas de load e todo
       silencio entre notas -- o grosso do tempo na maioria dos jogos. */
    if (cnt == 0) {
      memset(&out[n], 0, (size_t)(nsamples - n) * sizeof(int16_t));
      return;
    }

    float acc = 0.0f;
    acc += voiceUpdateEnvelope(v0) * voiceGenerate(v0, a0);
    acc += voiceUpdateEnvelope(v1) * voiceGenerate(v1, a1);
    {
      float e2 = voiceUpdateEnvelope(v2);
      float s2 = voiceGenerate(v2, a2);
      if (!voice3silent) acc += e2 * s2;
    }

    /* Dividir pelo numero de vozes ATIVAS evita clipping, mas faz o volume
       pular quando uma voz entra ou sai (efeito "bombeando"). E' o
       comportamento do original; se incomodar, trocar invN[cnt] por
       0.3333f fixo. */
    out[n] = (int16_t)(acc * invN[cnt] * gain);
  }
#endif
}