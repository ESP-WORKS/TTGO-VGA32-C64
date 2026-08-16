# Port do esp64 para ESP-IDF 5.5 / PlatformIO

Fontes em `src/`. Neste modo a PlatformIO compila `src/` com SCons, então os
`component.mk` e `CMakeLists.txt` de componente não são usados para o projeto.

## O que foi corrigido

| Erro | Correção |
|---|---|
| `esp_event_loop.h` deprecated | trocado por `esp_event.h` em `main.c` e `Teensy64.h` |
| `gpio_num_t was not declared` | `iopins.h` agora inclui `driver/gpio.h` e `driver/adc.h` |
| `esp_timer_get_time was not declared` | `esp_timer.h` em `Teensy64.h` e `go.cpp` |
| `portTICK_RATE_MS` | → `portTICK_PERIOD_MS` |
| `sdspi_slot_config_t` | reescrito com `spi_bus_initialize` + `sdspi_device_config_t` + `esp_vfs_fat_sdspi_mount` |
| `-Werror=unused-variable`, `char-subscripts`, `format=` | `build_unflags = -Werror=all` no `.ini` |
| `i2s_write_bytes` / `I2S_MODE_DAC_BUILT_IN` | backend de áudio reescrito em `dac_continuous` |
| `components/Wire`, `components/Audio` | removidos (ver abaixo) |

## Decisões que valem conferir

**SD e touch dividem o SPI2/HSPI.** Na IDF 3 o driver sdspi inicializava o
barramento sozinho e o touch do ILI9341 pegava carona — o `spi_bus_initialize`
dele está comentado em `ili9341_t3dma.cpp`. Preservei esse arranjo: o
`emu_init()` inicializa SPI2 explicitamente antes de montar o SD, e o
`tft.touchBegin()` continua fazendo só `spi_bus_add_device`. Se o touch parar de
responder, é aqui que se olha. O LCD segue em SPI3/VSPI.

**Áudio: `dac_continuous` no GPIO25.** O DAC agora consome amostras de 8 bits
sem sinal, então o `step()` converte de int16. Perde-se resolução em relação ao
I2S de 16 bits, mas o DAC do ESP32 é de 8 bits mesmo — o caminho antigo jogava
os 8 bits baixos fora do mesmo jeito. Canal CH0 = GPIO25; CH1 (GPIO26) fica
livre para o TX do link T-Display.

Adicionei uma guarda em `begin()`: o construtor global do `AudioPlaySystem`
chama `begin()` antes do `app_main`, e `go.cpp` chama de novo ao iniciar um
jogo. Sem a guarda o segundo `dac_continuous_new_channels` falha.

**`components/Wire` e `components/Audio` foram deletados.** Eram recortes do
core Arduino 1.0.x (`rom/ets_sys.h`, `timer_group_struct.h` com layout antigo) e
não têm conserto barato. `USE_WIRE` foi comentado no `emuapi.h`, o que faz o
teclado I²C cair no caminho `driver/i2c.h` nativo que já estava escrito no
`emuapi.cpp`. O `components/Audio` só servia ao caminho não-I2S, que não usamos.

**A lista de fontes vive em `src/CMakeLists.txt`.** Com `framework = espidf` a PlatformIO ignora `build_src_filter` e `src_dir` — ela avisa isso no log. O `src/reSID/sid.cpp` é um unity build que dá `#include` em todos os `.cc` de `src/reSID/reSID/`, então só ele e o `reSID.cpp` são listados; incluir os `.cc` daria símbolo duplicado no link.

**Os dois CMakeLists são arquivos diferentes e não se substituem.** O da raiz tem o boilerplate do projeto (`include(project.cmake)` + `project(esp64)`). O de `src/` tem o `idf_component_register`. Trocar um pelo outro dá `define_property command is not scriptable`.

**A supressão de warning mudou de lugar.** Os `-Wno-error` agora estão em `target_compile_options` dentro do `src/CMakeLists.txt`, não em `build_flags` — ali eles entram depois dos flags da IDF e realmente vencem.
unity build que dá `#include` em todos os `.cc`. Sem o filtro a PlatformIO
compila os `.cc` de novo e o link quebra com símbolo duplicado.

**Watchdog desligado no `sdkconfig.defaults`.** O laço do C64 (`oneRasterLine`)
não dá yield. Se preferir manter o WDT, o lugar de mexer é o `main_step()`.

## O que ainda pode aparecer

Não tenho como compilar isso aqui, então o que segue é palpite informado, não
verificação:

- `adc2_get_raw` / `ADC_WIDTH_BIT_12` só dão warning de deprecação na 5.5, mas o
  ADC2 conflita com o WiFi — que este projeto não usa, então deve passar.
- `spi_bus_add_device` do touch com `SPI_DEVICE_HALFDUPLEX` e `queue_size=2`
  pode reclamar dependendo de como o barramento foi inicializado pelo sdspi.
- Se o `dac_continuous_write` bloquear demais, troque `portMAX_DELAY` por um
  timeout curto no `step()`.
