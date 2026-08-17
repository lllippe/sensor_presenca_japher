/*
 * Sensor de Presença IoT - Porta com 2x HLK-LD2410C + MC-38 + WTV020-M01 + MQTT + BLE
 * Hardware: ESP32 + 2x HLK-LD2410C (radar mmWave 24GHz) + MC-38 (magnético)
 *           + WTV020-M01 (módulo de áudio SD)
 *
 * Baseado no sketch original "sensor_presenca.ino" (pasta
 * C:\Users\felip\OneDrive\Documentos\Arduino\sensor_presenca), que NÃO foi alterado.
 * Este projeto reaproveita o parser de frame do LD2410C e a lógica de debounce
 * do MC-38 e incrementa com:
 *
 *   - Um segundo HLK-LD2410C, para aumentar o ângulo de captura de presença
 *     (presença combinada = sensor 1 OU sensor 2 detectando alguém).
 *   - Módulo de áudio WTV020-M01:
 *       - Ao abrir a porta, toca o arquivo de índice 0 (0000.ad4) uma única vez.
 *       - Se a porta ficar mais de 3s aberta sem presença, toca o arquivo de
 *         índice 1 (0001.ad4) a cada 3s, até a porta fechar ou a presença voltar.
 *   - Conexão WiFi permanente (com reconexão automática) para publicar no
 *     broker MQTT:
 *       - Porta fechada: publica a cada 5 minutos.
 *       - Porta aberta: publica a cada 5 segundos, informando se há presença
 *         (aberta_com_presenca) ou não (aberta_sem_presenca).
 *     Em toda transição de estado (abriu/fechou) publica imediatamente também.
 *   - Configuração de WiFi e broker MQTT via Bluetooth (BLE), sob demanda
 *     (botão físico ativa por 5 minutos), protegida por senha.
 *
 * BIBLIOTECA DO WTV020-M01:
 *   Não há uma biblioteca oficial do WTV020-M01/WTV020SD16P no Gerenciador de
 *   Bibliotecas do Arduino IDE. Este projeto usa a biblioteca de domínio
 *   público "WTV020SD16P" (Diego J. Arevalo e colaboradores, mantida por
 *   FabLab Bayreuth: https://github.com/fablab-bayreuth/WTV020SD16P),
 *   cujo código-fonte foi copiado para a pasta lib_WTV020SD16P/ deste
 *   projeto (ver hardware.md para instruções de instalação).
 *   API confirmada lendo o .h/.cpp da biblioteca (v1.4.0):
 *     construtor WTV020SD16P(resetPin, clockPin, dataPin, busyPin) — já
 *     executa reset() internamente; métodos públicos: playVoice(indice)
 *     [bloqueante], asyncPlayVoice(indice) [não bloqueante], stopVoice(),
 *     pauseVoice(), mute(), unmute(), setVolume(0-7), reset(). Não existem
 *     métodos begin() nem isPlaying() nessa biblioteca.
 *
 * PRIMEIRO USO:
 *   1. Grave este código no ESP32.
 *   2. Grave no cartão SD do WTV020-M01 os arquivos de áudio no formato .ad4,
 *      convertidos com a ferramenta do fabricante, nomeados 0000.ad4 e 0001.ad4.
 *   3. Abra o Serial Monitor a 115200 baud.
 *   4. Pressione o botão de configuração BLE (GPIO 4) para ativar o Bluetooth.
 *   5. Conecte com um app BLE (ex: nRF Connect), autentique com a senha padrão
 *      (ver BLE_SENHA_PADRAO abaixo) e configure WiFi e broker MQTT.
 */

#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WTV020SD16P.h>

// ── Pinos — HLK-LD2410C #1 (existente) ──────────────────────────────────────
#define LD2410_1_OUT_PIN  27   // Pino OUT do sensor 1 (HIGH = presença) — não usado na lógica, mantido para referência/debug
#define LD2410_1_RX_PIN   16   // ESP32 RX2 ← LD2410C #1 TX
#define LD2410_1_TX_PIN   17   // ESP32 TX2 → LD2410C #1 RX

// ── Pinos — HLK-LD2410C #2 (novo, amplia o ângulo de captura) ──────────────
#define LD2410_2_OUT_PIN  34   // Pino OUT do sensor 2 — não usado na lógica, mantido para referência/debug
#define LD2410_2_RX_PIN   33   // ESP32 RX1 ← LD2410C #2 TX
#define LD2410_2_TX_PIN   32   // ESP32 TX1 → LD2410C #2 RX

// ── Pino — MC-38 ─────────────────────────────────────────────────────────────
#define MC38_PIN 25   // LOW = fechada, HIGH = aberta (INPUT_PULLUP)

// ── Pinos — WTV020-M01 ───────────────────────────────────────────────────────
#define WTV_RESET_PIN 18
#define WTV_CLOCK_PIN 19
#define WTV_DATA_PIN  21
#define WTV_BUSY_PIN  35

// ── Pino — Botão de configuração BLE ────────────────────────────────────────
#define BOTAO_BLE_PIN 4

// ── Índices dos áudios gravados no cartão do WTV020-M01 ─────────────────────
#define AUDIO_ABERTURA_IDX 0   // 0000.ad4 — toca 1x ao abrir a porta
#define AUDIO_ALERTA_IDX   1   // 0001.ad4 — toca a cada 3s se ficar aberta sem presença

// ── Configurações gerais ─────────────────────────────────────────────────────
#define UART_BAUD              256000  // padrão LD2410C (tente 115200 se não funcionar)
#define DISTANCIA_MAXIMA_CM    150     // ignora presença além desse limite
#define DEBOUNCE_MS            80      // tempo para considerar o estado da porta estável

#define LIMIAR_SEM_PRESENCA_MS   3000UL   // 3s sem presença com porta aberta -> começa o alerta sonoro
#define INTERVALO_ALERTA_MS      3000UL   // repete o áudio de alerta a cada 3s
#define INTERVALO_PUB_ABERTA_MS  5000UL   // publica no broker a cada 5s com a porta aberta
#define INTERVALO_PUB_FECHADA_MS (5UL * 60UL * 1000UL) // publica no broker a cada 5 min com a porta fechada

#define WIFI_RECONNECT_MS  10000UL  // intervalo entre tentativas de reconexão WiFi
#define MQTT_RECONNECT_MS   5000UL  // intervalo entre tentativas de reconexão MQTT
#define BLE_SESSAO_MS       (5UL * 60UL * 1000UL)  // BLE fica ativo por 5 min após o botão

#define BLE_SENHA_PADRAO  "presenca123"   // troque assim que configurar o dispositivo

// UUIDs do serviço/características BLE de configuração
#define SERVICE_UUID      "7b1d9000-9a3e-4f2a-8a5e-1c2d3e4f5a01"
#define CHAR_AUTH_UUID    "7b1d9001-9a3e-4f2a-8a5e-1c2d3e4f5a01"
#define CHAR_WIFI_UUID    "7b1d9002-9a3e-4f2a-8a5e-1c2d3e4f5a01"
#define CHAR_BROKER_UUID  "7b1d9003-9a3e-4f2a-8a5e-1c2d3e4f5a01"
#define CHAR_STATUS_UUID  "7b1d9004-9a3e-4f2a-8a5e-1c2d3e4f5a01"

// ── Estrutura de um sensor HLK-LD2410C ──────────────────────────────────────
#define MAX_FRAME_LEN  32

struct SensorRadar {
  HardwareSerial *serial;
  uint8_t  outPin;
  uint16_t distanciaAtualCm;
  uint8_t  estadoAtual;
  uint8_t  frameBuf[MAX_FRAME_LEN];
  uint8_t  frameIdx;
  bool     coletandoFrame;
};

SensorRadar radar1 = { &Serial2, LD2410_1_OUT_PIN, 0, 0, {0}, 0, false };
SensorRadar radar2 = { &Serial1, LD2410_2_OUT_PIN, 0, 0, {0}, 0, false };

// ── Objetos globais ──────────────────────────────────────────────────────────
Preferences prefs;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WTV020SD16P wtv(WTV_RESET_PIN, WTV_CLOCK_PIN, WTV_DATA_PIN, WTV_BUSY_PIN);

String wifiSsid, wifiPass;
String mqttIp, mqttTopic;
uint16_t mqttPort = 1883;
String blePass;

unsigned long ultimaTentativaWifi = 0;
unsigned long ultimaTentativaMqtt = 0;

bool bleAtiva = false;
bool bleAutenticado = false;
unsigned long bleSessaoFim = 0;
bool botaoAnterior = HIGH;
BLECharacteristic *charStatus = nullptr;

// ── Estado da porta / presença ──────────────────────────────────────────────
bool portaAbertaAnterior = false;
bool portaEstavel        = false;
unsigned long ultimaMudancaPorta = 0;

bool contandoSemPresenca = false;
unsigned long semPresencaDesde = 0;
bool alertaAtivo = false;
unsigned long ultimoAlerta = 0;

unsigned long ultimaPublicacaoAberta  = 0;
unsigned long ultimaPublicacaoFechada = 0;

// ── Parser de frame do LD2410C (mesma lógica do sketch original) ───────────
void processarFrame(SensorRadar &s, uint8_t* buf, uint8_t len) {
  if (len < 23)       return;
  if (buf[6] != 0x02) return;  // só processa frames de dados periódicos
  if (buf[7] != 0xAA) return;  // valida sub-header

  uint8_t  estado  = buf[8];
  uint16_t distDet = buf[15] | (buf[16] << 8);

  s.estadoAtual      = estado;
  s.distanciaAtualCm = distDet;
}

void lerUART(SensorRadar &s) {
  while (s.serial->available()) {
    uint8_t b = s.serial->read();

    if (!s.coletandoFrame) {
      if (b == 0xF4) {
        s.frameBuf[0]   = b;
        s.frameIdx      = 1;
        s.coletandoFrame = true;
      }
    } else {
      if (s.frameIdx < MAX_FRAME_LEN) s.frameBuf[s.frameIdx++] = b;

      // Detecta tail F8 F7 F6 F5
      if (s.frameIdx >= 4 &&
          s.frameBuf[s.frameIdx-4] == 0xF8 &&
          s.frameBuf[s.frameIdx-3] == 0xF7 &&
          s.frameBuf[s.frameIdx-2] == 0xF6 &&
          s.frameBuf[s.frameIdx-1] == 0xF5) {
        processarFrame(s, s.frameBuf, s.frameIdx);
        s.coletandoFrame = false;
        s.frameIdx = 0;
      }
    }
  }
}

bool presencaSensor(SensorRadar &s) {
  return (s.estadoAtual > 0) && (s.distanciaAtualCm > 0) &&
         (s.distanciaAtualCm <= DISTANCIA_MAXIMA_CM);
}

// Presença combinada: qualquer um dos dois radares detectando já conta,
// já que o segundo sensor existe justamente para ampliar o ângulo de captura.
bool presencaCombinada() {
  return presencaSensor(radar1) || presencaSensor(radar2);
}

// ── WiFi (conexão permanente, com reconexão automática) ─────────────────────
void garantirWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (wifiSsid.length() == 0) return;
  if (millis() - ultimaTentativaWifi < WIFI_RECONNECT_MS) return;

  ultimaTentativaWifi = millis();
  Serial.println("Tentando conectar ao WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

// ── MQTT (conexão permanente, com reconexão automática) ─────────────────────
void garantirMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttIp.length() == 0) return;
  if (mqttClient.connected()) return;
  if (millis() - ultimaTentativaMqtt < MQTT_RECONNECT_MS) return;

  ultimaTentativaMqtt = millis();
  mqttClient.setServer(mqttIp.c_str(), mqttPort);
  if (mqttClient.connect("ESP32-SensorPresenca")) {
    Serial.println("Conectado ao broker MQTT.");
  } else {
    Serial.println("Falha ao conectar no broker MQTT.");
  }
}

void publicarEstado(const char* estado) {
  if (mqttTopic.length() == 0) {
    Serial.println("Tópico MQTT não configurado — evento não enviado.");
    return;
  }
  if (!mqttClient.connected()) {
    Serial.println("MQTT não conectado — evento não enviado.");
    return;
  }

  DynamicJsonDocument doc(256);
  doc["estado"]     = estado;
  doc["uptime_ms"]  = millis();

  char payload[256];
  serializeJson(doc, payload);

  mqttClient.publish(mqttTopic.c_str(), payload);
  Serial.print("[MQTT] "); Serial.println(payload);
}

// ── Áudio (WTV020-M01) ───────────────────────────────────────────────────────
void tocarAudioAbertura() {
  wtv.asyncPlayVoice(AUDIO_ABERTURA_IDX);
  Serial.println("[AUDIO] Tocando aviso de abertura (indice 0).");
}

void tocarAudioAlerta() {
  wtv.asyncPlayVoice(AUDIO_ALERTA_IDX);
  Serial.println("[AUDIO] Tocando alerta de porta aberta sem presenca (indice 1).");
}

// ── Bluetooth (BLE) — configuração sob demanda ──────────────────────────────
void enviarStatusBle(const String &msg) {
  if (charStatus) {
    charStatus->setValue(msg.c_str());
    charStatus->notify();
  }
  Serial.println("[BLE] " + msg);
}

class AuthCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String valor = String(c->getValue().c_str());
    if (valor == blePass) {
      bleAutenticado = true;
      enviarStatusBle("AUTH_OK");
    } else {
      bleAutenticado = false;
      enviarStatusBle("AUTH_FALHOU");
    }
  }
};

// Payload esperado: "SSID|SENHA"
class WifiCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    if (!bleAutenticado) { enviarStatusBle("NAO_AUTENTICADO"); return; }
    String valor = String(c->getValue().c_str());
    int sep = valor.indexOf('|');
    if (sep < 0) { enviarStatusBle("FORMATO_INVALIDO"); return; }

    wifiSsid = valor.substring(0, sep);
    wifiPass = valor.substring(sep + 1);
    prefs.putString("wifi_ssid", wifiSsid);
    prefs.putString("wifi_pass", wifiPass);

    WiFi.disconnect(true); // força usar as novas credenciais na próxima tentativa
    ultimaTentativaWifi = 0;
    enviarStatusBle("WIFI_SALVO");
  }
};

// Payload esperado: "IP|PORTA|TOPICO"
class BrokerCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    if (!bleAutenticado) { enviarStatusBle("NAO_AUTENTICADO"); return; }
    String valor = String(c->getValue().c_str());
    int sep1 = valor.indexOf('|');
    int sep2 = valor.indexOf('|', sep1 + 1);
    if (sep1 < 0 || sep2 < 0) { enviarStatusBle("FORMATO_INVALIDO"); return; }

    mqttIp    = valor.substring(0, sep1);
    mqttPort  = valor.substring(sep1 + 1, sep2).toInt();
    mqttTopic = valor.substring(sep2 + 1);
    prefs.putString("mqtt_ip", mqttIp);
    prefs.putUShort("mqtt_port", mqttPort);
    prefs.putString("mqtt_topic", mqttTopic);

    mqttClient.disconnect(); // força reconectar no broker/tópico novo
    ultimaTentativaMqtt = 0;
    enviarStatusBle("BROKER_SALVO");
  }
};

class ServidorBleCallback : public BLEServerCallbacks {
  void onDisconnect(BLEServer *server) override {
    bleAutenticado = false;
    if (bleAtiva) server->getAdvertising()->start();
  }
};

void iniciarBLE() {
  BLEDevice::init("SensorPresencaIoT");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServidorBleCallback());
  BLEService *service = server->createService(SERVICE_UUID);

  BLECharacteristic *charAuth = service->createCharacteristic(
    CHAR_AUTH_UUID, BLECharacteristic::PROPERTY_WRITE);
  charAuth->setCallbacks(new AuthCallback());

  BLECharacteristic *charWifi = service->createCharacteristic(
    CHAR_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  charWifi->setCallbacks(new WifiCallback());

  BLECharacteristic *charBroker = service->createCharacteristic(
    CHAR_BROKER_UUID, BLECharacteristic::PROPERTY_WRITE);
  charBroker->setCallbacks(new BrokerCallback());

  charStatus = service->createCharacteristic(
    CHAR_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  charStatus->addDescriptor(new BLE2902());

  service->start();
  server->getAdvertising()->start();

  bleAutenticado = false;
  bleAtiva = true;
  bleSessaoFim = millis() + BLE_SESSAO_MS;
  Serial.println("BLE ativado para configuração (5 min).");
}

void pararBLE() {
  BLEDevice::deinit(true);
  charStatus = nullptr;
  bleAtiva = false;
  bleAutenticado = false;
  Serial.println("BLE desativado (economia de energia).");
}

// ── Lógica de porta + presença + áudio + MQTT ───────────────────────────────
void tratarPortaAberta(bool acabouDeAbrir) {
  bool presenca = presencaCombinada();

  if (acabouDeAbrir) {
    tocarAudioAbertura();
    contandoSemPresenca = false;
    alertaAtivo = false;
    // força publicação imediata ao abrir
    ultimaPublicacaoAberta = millis() - INTERVALO_PUB_ABERTA_MS;
  }

  if (!presenca) {
    if (!contandoSemPresenca) {
      contandoSemPresenca = true;
      semPresencaDesde = millis();
    }
    if (millis() - semPresencaDesde >= LIMIAR_SEM_PRESENCA_MS) {
      if (!alertaAtivo || millis() - ultimoAlerta >= INTERVALO_ALERTA_MS) {
        tocarAudioAlerta();
        ultimoAlerta = millis();
        alertaAtivo = true;
      }
    }
  } else {
    contandoSemPresenca = false;
    alertaAtivo = false;
  }

  if (millis() - ultimaPublicacaoAberta >= INTERVALO_PUB_ABERTA_MS) {
    ultimaPublicacaoAberta = millis();
    publicarEstado(presenca ? "aberta_com_presenca" : "aberta_sem_presenca");
  }
}

void tratarPortaFechada(bool acabouDeFechar) {
  contandoSemPresenca = false;
  alertaAtivo = false;

  if (acabouDeFechar) {
    // força publicação imediata ao fechar
    ultimaPublicacaoFechada = millis() - INTERVALO_PUB_FECHADA_MS;
  }

  if (millis() - ultimaPublicacaoFechada >= INTERVALO_PUB_FECHADA_MS) {
    ultimaPublicacaoFechada = millis();
    publicarEstado("fechada");
  }
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LD2410_1_OUT_PIN, INPUT);
  pinMode(LD2410_2_OUT_PIN, INPUT);
  pinMode(MC38_PIN, INPUT_PULLUP);  // pull-up interno: LOW=fechada, HIGH=aberta
  pinMode(BOTAO_BLE_PIN, INPUT_PULLUP);

  Serial2.begin(UART_BAUD, SERIAL_8N1, LD2410_1_RX_PIN, LD2410_1_TX_PIN);
  Serial1.begin(UART_BAUD, SERIAL_8N1, LD2410_2_RX_PIN, LD2410_2_TX_PIN);
  delay(200);

  wtv.reset(); // o construtor já reseta o módulo; chamada extra não tem efeito colateral

  prefs.begin("presenca_iot", false);
  wifiSsid  = prefs.getString("wifi_ssid", "");
  wifiPass  = prefs.getString("wifi_pass", "");
  mqttIp    = prefs.getString("mqtt_ip", "");
  mqttPort  = prefs.getUShort("mqtt_port", 1883);
  mqttTopic = prefs.getString("mqtt_topic", "");
  blePass   = prefs.getString("ble_pass", BLE_SENHA_PADRAO);

  Serial.println();
  Serial.println("=== Sensor de Presenca IoT - Porta ===");
  Serial.print("Baud UART radares: "); Serial.println(UART_BAUD);
  Serial.print("Limite de distancia: "); Serial.print(DISTANCIA_MAXIMA_CM); Serial.println("cm");
  Serial.println("Pressione o botao de configuracao para ativar o Bluetooth.");
  Serial.println();

  // Leitura inicial do MC-38 para conhecer o estado da porta ao ligar
  bool portaInicialAberta = digitalRead(MC38_PIN) == HIGH;
  portaEstavel         = portaInicialAberta;
  portaAbertaAnterior   = portaInicialAberta;
  ultimaMudancaPorta    = millis();

  if (!portaInicialAberta) {
    ultimaPublicacaoFechada = millis() - INTERVALO_PUB_FECHADA_MS; // publica assim que conectar
    Serial.println("[OK] Porta fechada.");
  } else {
    ultimaPublicacaoAberta = millis() - INTERVALO_PUB_ABERTA_MS;
    Serial.println("[AVISO] Porta ja estava aberta ao ligar.");
  }
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  lerUART(radar1);
  lerUART(radar2);

  // Botão físico de ativação do BLE
  bool botaoAtual = digitalRead(BOTAO_BLE_PIN);
  if (botaoAnterior == HIGH && botaoAtual == LOW) {
    if (!bleAtiva) {
      iniciarBLE();
    } else {
      bleSessaoFim = millis() + BLE_SESSAO_MS; // pressionar de novo estende a sessão
      Serial.println("Sessão BLE estendida.");
    }
    delay(50); // debounce simples
  }
  botaoAnterior = botaoAtual;

  if (bleAtiva && millis() > bleSessaoFim) {
    pararBLE();
  }

  garantirWiFi();
  garantirMqtt();
  mqttClient.loop();

  bool leituraAtual = digitalRead(MC38_PIN) == HIGH;

  // Detecta mudança e reinicia o timer de debounce
  if (leituraAtual != portaEstavel) {
    portaEstavel       = leituraAtual;
    ultimaMudancaPorta = millis();
  }

  // Só age após o estado ficar estável por DEBOUNCE_MS
  if (millis() - ultimaMudancaPorta < DEBOUNCE_MS) return;

  bool portaAberta = portaEstavel;

  if (portaAberta) {
    bool acabouDeAbrir = !portaAbertaAnterior;
    if (acabouDeAbrir) {
      portaAbertaAnterior = true;
      Serial.println("[EVENTO] Porta abriu.");
    }
    tratarPortaAberta(acabouDeAbrir);
  } else {
    bool acabouDeFechar = portaAbertaAnterior;
    if (acabouDeFechar) {
      portaAbertaAnterior = false;
      Serial.println("[EVENTO] Porta fechou.");
    }
    tratarPortaFechada(acabouDeFechar);
  }
}
