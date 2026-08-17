# Sensor de Presença IoT — Porta

Evolução do projeto original de sensor de presença de porta (2x HLK-LD2410C + MC-38), agora com aviso sonoro local via módulo de áudio WTV020-M01, conectividade WiFi/MQTT contínua e configuração via Bluetooth (BLE).

Baseado no sketch original `sensor_presenca.ino` (pasta `C:\Users\felip\OneDrive\Documentos\Arduino\sensor_presenca`), que **não foi alterado** — este projeto reaproveita o parser de frame do LD2410C e a lógica de debounce do MC-38 como ponto de partida.

## Funcionalidades

- **Dois sensores HLK-LD2410C**: um segundo radar foi adicionado para ampliar o ângulo de captura de presença. A detecção é combinada por **OU** — se qualquer um dos dois sensores detectar alguém dentro do alcance configurado (150cm por padrão), considera-se presença.
- **Áudio local via WTV020-M01**:
  - Toca um arquivo (índice 0) **uma única vez** assim que a porta é aberta.
  - Se a porta permanecer aberta por **mais de 3 segundos sem ninguém presente**, toca outro arquivo (índice 1) **a cada 3 segundos**, repetindo até a porta fechar ou a presença ser detectada novamente.
- **Conectividade WiFi permanente**, com reconexão automática se a conexão cair.
- **Publicação no broker MQTT** (payload em JSON):
  - **Porta fechada**: publica imediatamente ao fechar e depois a cada 5 minutos.
  - **Porta aberta**: publica imediatamente ao abrir e depois a cada 5 segundos, informando se os radares detectam presença (`aberta_com_presenca`) ou não (`aberta_sem_presenca`).
- **Configuração via Bluetooth**: WiFi e broker MQTT (IP/porta/tópico), protegida por senha de aplicação. BLE fica desligado por padrão — só ativa com o botão físico, por 5 minutos.

## Hardware

- ESP32 DevKit V1
- 2x HLK-LD2410C (radar mmWave 24GHz)
- Sensor magnético MC-38
- Módulo de áudio WTV020-M01 (com cartão micro SD)
- Botão de configuração BLE

Ligações detalhadas, incluindo o resultado de uma pesquisa aprofundada no datasheet oficial do fabricante sobre a tensão de alimentação correta do WTV020-M01 (3.3V, não 5V) e o protocolo de comunicação, em [hardware.md](hardware.md).

## Bibliotecas

Instalar no Arduino IDE antes de compilar:

- **ArduinoJson** by Benoit Blanchon (v6.x) — via Gerenciador de Bibliotecas
- **PubSubClient** by Nick O'Leary — via Gerenciador de Bibliotecas
- **WTV020SD16P** — **não está** no Gerenciador de Bibliotecas; o código-fonte já vem incluído neste projeto em [lib_WTV020SD16P/](lib_WTV020SD16P/) (biblioteca de domínio público de [github.com/fablab-bayreuth/WTV020SD16P](https://github.com/fablab-bayreuth/WTV020SD16P)). Copie essa pasta para `Documentos/Arduino/libraries/WTV020SD16P` antes de compilar — ver [hardware.md](hardware.md) para o passo a passo.

`WiFi`, `Preferences` e `BLEDevice` já vêm no core do ESP32.

## Como usar

### 1. Preparar o cartão SD do WTV020-M01

Converta os dois áudios desejados para o formato `.ad4` com a ferramenta de conversão do fabricante e grave no cartão como:

- `0000.ad4` — tocado ao abrir a porta
- `0001.ad4` — tocado repetidamente se a porta ficar aberta sem presença

### 2. Configurar WiFi e broker — via Bluetooth

1. Pressione o botão de configuração (GPIO 4) — o BLE `SensorPresencaIoT` fica ativo por 5 minutos.
2. Conecte com um app BLE (ex: nRF Connect) e autentique com a senha (padrão `presenca123`, ver [hardware.md](hardware.md) para detalhes de UUIDs e formato de cada campo).
3. Escreva o WiFi e o IP/porta/tópico do broker MQTT.

### Ajustar os limites de tempo

No início do código:

```cpp
#define DISTANCIA_MAXIMA_CM      150     // alcance considerado como "presença"
#define LIMIAR_SEM_PRESENCA_MS   3000UL  // tempo sem presença até começar o alerta sonoro
#define INTERVALO_ALERTA_MS      3000UL  // intervalo de repetição do alerta sonoro
#define INTERVALO_PUB_ABERTA_MS  5000UL  // intervalo de publicação MQTT com porta aberta
#define INTERVALO_PUB_FECHADA_MS (5UL * 60UL * 1000UL) // intervalo de publicação MQTT com porta fechada
```

## Funcionamento

```
Porta fecha → publica "fechada" no MQTT imediatamente, depois a cada 5 min

Porta abre → toca áudio de abertura (índice 0) uma vez
  → publica "aberta_com_presenca" ou "aberta_sem_presenca" imediatamente, depois a cada 5s
  → se ficar 3s+ sem presença (nenhum dos 2 radares detectando):
       toca áudio de alerta (índice 1) a cada 3s, até a porta fechar ou a presença voltar
```

## Saída no Serial Monitor

```
[EVENTO] Porta abriu.
[AUDIO] Tocando aviso de abertura (indice 0).
[MQTT] {"estado":"aberta_sem_presenca","uptime_ms":184320}
[AUDIO] Tocando alerta de porta aberta sem presenca (indice 1).
[MQTT] {"estado":"aberta_com_presenca","uptime_ms":189320}
[EVENTO] Porta fechou.
[MQTT] {"estado":"fechada","uptime_ms":194320}
```
