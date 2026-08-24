/*
  sidlite -- emulacao de SID leve para o esp64 / TTGO VGA32.

  Adaptado de T-HMI-C64 (retroelec, GPLv3), src/SID.cpp + src/SID.h.
  Substitui o reSID, que era caro demais (clocava o chip a 985248 Hz e
  reamostrava para 22050 com SAMPLE_FAST, gerando aliasing) e cuja
  integracao via bloco de 443 amostras nunca casou com as 441 do frame PAL.

  Diferencas em relacao ao original do retroelec:

  - Modelo PULL em vez de PUSH. O original chamava fillBuffer() a cada
    rasterline, gerando 2-3 amostras, e despejava o frame inteiro no driver
    na linha 311. Aqui quem manda e' o SND_Process, que pede N amostras
    quando o I2S tem espaco. Isso elimina o problema de descasamento de
    bloco por construcao: nao existe "bloco de frame" para desalinhar.

  - Taxa de amostragem vem do DEFAULT_SAMPLERATE do projeto (22050), nao
    dos 44100 fixos do original. O fillBuffer original assumia ~2,8
    amostras por rasterline (44100/312) e estouraria o buffer em 22050
    (441/312 = 1,41); como o modelo agora e' pull, essa matematica sumiu.

  - API em C, para poder ser chamada do pla.cpp sem arrastar C++.

  Limitacao herdada do original: NAO ha emulacao de filtro. As escritas em
  $D415-$D417 (cutoff/ressonancia/roteamento) sao guardadas mas ignoradas.
  Jogos que usam o filtro para timbre soam mais crus, porem limpos.
*/
#ifndef SIDLITE_H
#define SIDLITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zera vozes, registradores e envelopes. Chamar do c64_Init(). */
void sidlite_init(void);

/* Escrita do 6502. Aceita o endereco completo ($D400-$D7FF, o espelho de
   32 bytes e' resolvido aqui dentro) ou ja o indice 0x00-0x1F. */
void sidlite_write(uint16_t addr, uint8_t val);

/* Leitura do 6502. $D41B devolve ruido, $D41C o envelope da voz 3. */
uint8_t sidlite_read(uint16_t addr);

/* Gera nsamples amostras mono int16 assinadas em out[].
   Chamado pelo SND_Process. nsamples = contagem de amostras, NAO bytes. */
void sidlite_fill(int16_t *out, int nsamples);

/* Volume geral do emulador, 0-255. Independente do $D418 do C64. */
void sidlite_setVolume(uint8_t v);
uint8_t sidlite_getVolume(void);

#ifdef __cplusplus
}
#endif

#endif /* SIDLITE_H */