#ifndef GO_H
#define GO_H
// Arduino define setup()/loop() com linkagem C++, entao estas NAO podem
// estar em extern "C". As do emulador ganharam outro nome para nao colidir.
void emu_setup(void);
void emu_loop(void);
#endif
