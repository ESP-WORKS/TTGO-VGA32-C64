#ifndef audioplaysystem_h_
#define audioplaysystem_h_

#define DEFAULT_SAMPLESIZE   512  // 22050/50=443 samples per 20ms
#define DEFAULT_SAMPLERATE   22050

class AudioPlaySystem
{
public:
// NAO chame begin() aqui. Este objeto e' global, entao o construtor roda
// durante a static init, antes do app_main e antes do video.begin(). O
// dac_continuous usa I2S0+DMA e inicializa-lo tao cedo conflita com a
// inicializacao do SPI. go.cpp ja chama audio.begin()/start() ao carregar
// um jogo, que e' o momento correto.
AudioPlaySystem(void) { }
	void begin(void);
	void setSampleParameters(float clockfreq, float samplerate);
	void reset(void);
	void start(void);
	void stop(void);
	bool isPlaying(void);
	void sound(int C, int F, int V);  	
	void buzz(int size, int val);
	void step(void);  
};


#endif