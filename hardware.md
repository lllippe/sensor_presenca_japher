# Hardware — sensor_presenca_iot

## Componentes

| Componente | Modelo |
|---|---|
| Microcontrolador | ESP32 DevKit V1 |
| Sensor de presença (radar 1) | HLK-LD2410C (mmWave 24GHz) — já existente |
| Sensor de presença (radar 2) | HLK-LD2410C (mmWave 24GHz) — novo, amplia o ângulo de captura |
| Sensor magnético de porta | MC-38 |
| Módulo de áudio | WTV020-M01 (com cartão micro SD) |
| Conectividade | WiFi + Bluetooth (BLE) — ambos nativos do ESP32 |

---

## HLK-LD2410C #1 (existente) → ESP32

| LD2410C #1 | ESP32 | Observação |
|---|---|---|
| VCC | 5V (VIN) | **Precisa de 5V** — não funciona em 3.3V |
| GND | GND | |
| TX | GPIO 16 | ESP32 RX2 (`Serial2`) |
| RX | GPIO 17 | ESP32 TX2 (`Serial2`) |
| OUT | GPIO 27 | HIGH = presença. Não é usado na lógica do firmware (que lê os dados via UART), mantido apenas para depuração |

## HLK-LD2410C #2 (novo) → ESP32

Posicionado com um ângulo diferente do sensor 1, para cobrir uma área maior de detecção na porta.

| LD2410C #2 | ESP32 | Observação |
|---|---|---|
| VCC | 5V (VIN) | **Precisa de 5V** — não funciona em 3.3V |
| GND | GND | |
| TX | GPIO 33 | ESP32 RX1 (`Serial1`, pino remapeado via `Serial1.begin(baud, config, rx, tx)`) |
| RX | GPIO 32 | ESP32 TX1 (`Serial1`, pino remapeado) |
| OUT | GPIO 34 | HIGH = presença. Pino somente-entrada no ESP32; não é usado na lógica do firmware |

> No ESP32, `Serial1` normalmente usa os pinos GPIO 9/10 por padrão (ligados à flash em alguns módulos), mas ao chamar `begin()` informando `rx`/`tx` customizados o UART é remapeado via matriz de GPIO interna do chip — por isso é seguro usar os pinos 33/32 acima.

A lógica de presença combina os dois sensores com **OU**: se qualquer um dos dois detectar alguém dentro de `DISTANCIA_MAXIMA_CM` (150cm por padrão), considera-se que há presença.

---

## MC-38 (sensor magnético) → ESP32

| MC-38 | ESP32 | Observação |
|---|---|---|
| Um terminal | GPIO 25 | configurado com `INPUT_PULLUP` |
| Outro terminal | GND | |

- Contato fechado (ímã próximo) = porta fechada → GPIO em LOW.
- Contato aberto (ímã afastado) = porta aberta → GPIO em HIGH (pull-up interno).

---

## WTV020-M01 (módulo de áudio) → ESP32

Pesquisa feita a partir do **datasheet oficial do fabricante** (Guangzhou Waytronic Electronic Co., LTD — chip WTV020-SD, usado tanto no `WTV020-SD-16P`/`M01` DIP16 quanto no `WTV020-SD-20S` SOP20; fonte: [WTV020-SD Module datasheet, SparkFun mirror](https://dlnmh9ip6v2uc.cloudfront.net/datasheets/Widgets/WTV020SD.pdf)) e da biblioteca Arduino real usada neste projeto (ver seção Bibliotecas). Resolve as duas dúvidas em aberto da primeira versão deste hardware.md.

**Tensão de alimentação (confirmada pelo datasheet oficial): DC 2.7V ~ 3.5V.** O módulo é alimentado nativamente em **3.3V**, não em 5V — o datasheet inclusive recomenda dois diodos em série (1N4001/1N4007) na entrada positiva caso a única fonte disponível seja 5V, para abaixar a tensão. Como o ESP32 já disponibiliza um pino 3.3V regulado, é isso que se deve usar — **sem necessidade de level shifter**, já que os pinos de sinal (RESET/CLK/DATA) também operam nessa mesma faixa de tensão.

| WTV020-M01 (pinagem DIP16 / `WTV020-SD-16P`) | ESP32 | Observação |
|---|---|---|
| Pino 16 — VDD | **3.3V** | **Não ligar em VIN/5V** — fora da faixa 2.7~3.5V do datasheet |
| Pino 8 — GND | GND | |
| Pino 1 — RESET | GPIO 18 | |
| Pino 7 — P04 (CLK, modo dois-fios) | GPIO 19 | |
| Pino 10 — P05 (DI/DATA, modo dois-fios) | GPIO 21 | |
| Pino 15 — P06 (BUSY) | GPIO 35 | Pino somente-entrada no ESP32; HIGH enquanto o módulo toca um áudio |
| Pino 4/5 — SPK+ / SPK- (saída PWM) | Alto-falante 8Ω pequeno, direto ou via amplificador | |
| Pino 2 — AUDIO-L (saída DAC) | Alternativa ao PWM, para ligar a um amplificador externo (ex: LM386) | Não usado neste projeto (usamos SPK+/SPK-) |

> A pinagem acima é a do encapsulamento DIP16 (`WTV020-SD-16P`), que é o formato mais comum vendido como "WTV020-M01". Se seu módulo vier em outro formato (ex: `WTV020-SD-20S`, SOP20), a numeração física dos pinos muda, mas as funções (RESET/CLK/DATA/BUSY/VDD/GND/SPK) são as mesmas — confira a serigrafia da sua placa.

### Modo de operação usado: dois fios (CLK/DATA)

O chip WTV020-SD suporta vários modos (MP3 por botão, "key mode", loop automático, dois-fios). Este projeto usa o **modo serial de dois fios**, controlado por software (não por botões físicos), que é o modo implementado pela biblioteca `WTV020SD16P`:

- O microcontrolador envia um valor de 16 bits (MSB primeiro) pelos pinos CLK/DATA para escolher e tocar uma faixa (`0x0000`–`0x01FF` = índices 0 a 511, correspondendo aos arquivos `0000.ad4` a `0511.ad4`).
- Comandos especiais fora dessa faixa: `0xFFFE` = play/pause da faixa atual, `0xFFFF` = parar, `0xFFF0`–`0xFFF7` = ajuste de volume (8 níveis).
- O pino BUSY fica em HIGH enquanto uma faixa está tocando.
- Um pulso no pino RESET (LOW por alguns ms, depois HIGH) reinicia o módulo; a biblioteca faz isso automaticamente ao ser construída.

### Arquivos de áudio no cartão SD

Grave os arquivos convertidos no formato `.ad4` (usando a ferramenta de conversão do fabricante do WTV020, geralmente `VoiceConverter.exe`; taxa de amostragem suportada: 6kHz a 36kHz), nomeados por índice decimal com 4 dígitos:

| Índice | Arquivo | Quando toca |
|---|---|---|
| 0 | `0000.ad4` | Uma vez, assim que a porta é aberta |
| 1 | `0001.ad4` | A cada 3 segundos, enquanto a porta estiver aberta há mais de 3s sem presença detectada, até fechar ou a presença voltar |

O cartão SD deve estar formatado em FAT. Capacidade suportada pelo módulo: até 1GB de cartão SD (ou até 512 faixas endereçáveis no modo dois-fios).

---

## Botão de configuração BLE → ESP32

| Botão | ESP32 | Observação |
|---|---|---|
| Um terminal | GPIO 4 | configurado com `INPUT_PULLUP` — ativo em LOW |
| Outro terminal | GND | |

> Pressionar o botão ativa o Bluetooth por 5 minutos para configuração (WiFi e broker MQTT). Pressionar novamente durante a sessão estende o tempo. Passado esse período, o BLE é desligado automaticamente.

---

## Diagrama simplificado

```
                         ┌───────────────────────────────────────────┐
                         │                 ESP32 DevKit               │
                         │                                             │
LD2410C #1 TX ───────────┤GPIO16 (RX2)      GPIO18 ─────────────────────┼──── WTV020 RESET
LD2410C #1 RX ───────────┤GPIO17 (TX2)      GPIO19 ─────────────────────┼──── WTV020 CLK
LD2410C #1 OUT ──────────┤GPIO27            GPIO21 ─────────────────────┼──── WTV020 DATA
                         │                  GPIO35 (in only) ───────────┼──── WTV020 BUSY
LD2410C #2 TX ───────────┤GPIO33 (RX1)                                  │
LD2410C #2 RX ───────────┤GPIO32 (TX1)      GPIO4  ─────────────────────┼──── Botão config BLE
LD2410C #2 OUT ──────────┤GPIO34 (in only)                              │
                         │                                             │
MC-38 ────────────────────┤GPIO25 (pull-up)                             │
                         │                                             │
5V (VIN) ─────────────────┤VIN     apenas os 2 radares LD2410C          │
3.3V ───────────────────────┤3V3     apenas o WTV020-M01 (2.7~3.5V)        │
GND ───────────────────────┤GND     comum a todos os módulos             │
                         └───────────────────────────────────────────┘
```

---

## Bibliotecas necessárias

Instalar via **Arduino IDE → Gerenciador de Bibliotecas**:

- **ArduinoJson** by Benoit Blanchon (v6.x)
- **PubSubClient** by Nick O'Leary (cliente MQTT)

**WTV020SD16P — já incluída neste projeto**, na pasta [lib_WTV020SD16P/](lib_WTV020SD16P/):

Essa biblioteca não está disponível no Gerenciador de Bibliotecas do Arduino IDE, então seu código-fonte foi copiado (não apenas referenciado) para dentro deste projeto, seguindo a mesma prática usada em `inicio_sem_parar/R200/` na pasta `saida`. Dados da biblioteca:

- **Origem**: [github.com/fablab-bayreuth/WTV020SD16P](https://github.com/fablab-bayreuth/WTV020SD16P), v1.4.0 — criada por Diego J. Arevalo (2012), modificada por Ryszard Malinowski, Dan F e Thomas A. Hirsch (FabLab Bayreuth). **Licença: domínio público** ("Released into the public domain", conforme o cabeçalho do código-fonte).
- **Conteúdo copiado**: `src/WTV020SD16P.h`, `src/WTV020SD16P.cpp`, `library.properties`, `keywords.txt`, `README.md` e o exemplo `examples/ESP8266/ESP8266.ino` (mesma família de chip Espressif do ESP32, serve de referência de uso).
- **Para instalar no Arduino IDE**: copie a pasta `lib_WTV020SD16P` inteira para dentro de `Documentos/Arduino/libraries/`, renomeando-a para `WTV020SD16P` (o Arduino IDE espera o nome da pasta igual ao da biblioteca). Alternativamente, compacte o conteúdo de `lib_WTV020SD16P` em `.zip` e use **Sketch → Include Library → Add .ZIP Library**.
- **API confirmada lendo o código-fonte** (não documentação de terceiros): construtor `WTV020SD16P(resetPin, clockPin, dataPin, busyPin)` — já chama `reset()` internamente. Métodos públicos: `playVoice(indice)` (bloqueante, espera o pino BUSY liberar), `asyncPlayVoice(indice)` (não bloqueante — é o usado neste projeto), `stopVoice()`, `pauseVoice()`, `mute()`, `unmute()`, `setVolume(0 a 7)`, `reset()`. **Não existem métodos `begin()` nem `isPlaying()`** nessa biblioteca (uma suposição errada de uma versão anterior deste hardware.md foi corrigida).

Já inclusas no core do ESP32 (não precisa instalar):

- `WiFi.h`, `Preferences.h`, `BLEDevice.h` / `BLEServer.h` / `BLEUtils.h` / `BLE2902.h`

---

## Fontes consultadas na pesquisa sobre o WTV020-M01

- [WTV020-SD Module datasheet (fabricante Waytronic, mirror SparkFun)](https://dlnmh9ip6v2uc.cloudfront.net/datasheets/Widgets/WTV020SD.pdf) — pinagem oficial, tensão de operação (2.7~3.5V), protocolo de dois fios (CLK/DATA), códigos de comando (play/pause/stop/volume), formato dos arquivos de voz no cartão SD.
- [github.com/fablab-bayreuth/WTV020SD16P](https://github.com/fablab-bayreuth/WTV020SD16P) — biblioteca Arduino usada neste projeto (código-fonte copiado para `lib_WTV020SD16P/`).
- [WTV020M01 Datasheet PDF – MP3 Audio Voice Module SD (datasheetcafe.com)](https://www.datasheetcafe.com/wtv020m01-datasheet-pdf/) — referência cruzada da tensão de operação.

---

## Configuração via Bluetooth (BLE)

Ative pressionando o botão de configuração (GPIO 4). Nome do dispositivo BLE: `SensorPresencaIoT`. Sessão ativa por 5 minutos (renovável pressionando o botão de novo).

Fluxo com um app genérico (ex: **nRF Connect**):

1. Conectar no dispositivo `SensorPresencaIoT`.
2. Escrever a senha na característica de **autenticação**. Senha padrão: `presenca123` — troque assim que possível editando `BLE_SENHA_PADRAO` no código ou reescrevendo o valor salvo em `Preferences` (chave `ble_pass`).
3. Após autenticado, escrever nas demais características (texto puro/UTF-8):

| Característica | UUID (final) | Payload | Exemplo |
|---|---|---|---|
| Auth | `...9001` | `SENHA` | `presenca123` |
| WiFi | `...9002` | `SSID\|SENHA` | `MinhaRede\|minhasenha123` |
| Broker MQTT | `...9003` | `IP\|PORTA\|TOPICO` | `192.168.1.10\|1883\|casa/porta` |
| Status (notify) | `...9004` | somente leitura/notificação | `WIFI_SALVO`, `BROKER_SALVO`, `NAO_AUTENTICADO`, etc. |

> Qualquer escrita em WiFi/Broker sem autenticação prévia é rejeitada (`NAO_AUTENTICADO`).
> Ao salvar novas credenciais de WiFi ou broker, o firmware força uma reconexão imediata usando os novos dados.

---

## Broker MQTT

- O ESP32 mantém conexão permanente com o WiFi e o broker MQTT (reconectando automaticamente se cair), diferente de outros projetos da pasta `saida` que conectam sob demanda — necessário aqui porque a porta aberta publica a cada 5 segundos.
- Payload publicado em JSON, no tópico configurado via BLE:

**Porta fechada** (publica imediatamente ao fechar e depois a cada 5 minutos):
```json
{ "estado": "fechada", "uptime_ms": 123456 }
```

**Porta aberta com presença** (publica imediatamente ao abrir e depois a cada 5 segundos, enquanto os radares detectarem alguém):
```json
{ "estado": "aberta_com_presenca", "uptime_ms": 123456 }
```

**Porta aberta sem presença** (mesma cadência de 5 segundos, quando nenhum dos dois radares detecta ninguém):
```json
{ "estado": "aberta_sem_presenca", "uptime_ms": 123456 }
```
