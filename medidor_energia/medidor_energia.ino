// =====================================================================
//  Grana Amiga — Medidor de energia IoT (ESP32 + framework Arduino)
//  Global Solution 2026/2 · CitizenTech · FIAP
//  Disruptive Architectures: IoT, IoB & Generative IA
//
//  O que faz:
//   1. Lê a corrente da casa (sensor SCT-013 ou, no Wokwi, um potenciômetro)
//   2. Calcula potência (W), kWh acumulado no mês e custo estimado em R$
//   3. Compara o custo com a meta mensal da família -> VERDE / AMARELO / VERMELHO
//   4. Mostra tudo no LCD e sinaliza a faixa no LED RGB e no buzzer (funciona sem internet)
//   5. Publica as leituras via MQTT (Wi-Fi) para o Node-RED e a API do app
//   6. Sem internet, guarda as leituras num buffer e reenvia quando a conexão volta
//
//  Bibliotecas: PubSubClient, ArduinoJson (v7), LiquidCrystal I2C
// =====================================================================
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

// =====================================================================
//  CONFIGURAÇÃO (pinos, constantes e tipos)
//  Credenciais reais NUNCA ficam aqui: no protótipo físico, copie
//  secrets.example.h para secrets.h (que está no .gitignore).
// =====================================================================

// ---------------------------------------------------------------------
//  Modo de operação
//  1 = Wokwi / demonstração: potenciômetro simula a corrente do SCT-013
//      e o tempo é acelerado (1 s real = 1 h de consumo)
//  0 = protótipo físico com sensor SCT-013-030, tempo real
// ---------------------------------------------------------------------
#define MODO_SIMULACAO 1

// ---------------------------------------------------------------------
//  Identificação do medidor (= coluna dispositivo.codigo no banco)
// ---------------------------------------------------------------------
#define CODIGO_DISPOSITIVO "GA-ESP32-001"

// ---------------------------------------------------------------------
//  Rede (valores padrão, sobrescritos por secrets.h se ele existir)
//  "Wokwi-GUEST" é a rede Wi-Fi virtual do simulador, sem senha.
// ---------------------------------------------------------------------
#define WIFI_SSID_PADRAO     "Wokwi-GUEST"
#define WIFI_SENHA_PADRAO    ""
#define MQTT_HOST_PADRAO     "broker.hivemq.com"
#define MQTT_PORTA_PADRAO    1883
#define MQTT_USUARIO_PADRAO  ""
#define MQTT_SENHA_PADRAO    ""

// Tópicos: granaamiga/casa/<codigo>/<energia|status|alerta|config|...>
#define TOPICO_BASE "granaamiga/casa/" CODIGO_DISPOSITIVO

// ---------------------------------------------------------------------
//  Pinos do ESP32 DevKit
// ---------------------------------------------------------------------
const int PINO_SENSOR        = 34;  // ADC1_CH6. ADC2 não funciona com o Wi-Fi ligado
const int PINO_LED_R         = 25;
const int PINO_LED_G         = 26;
const int PINO_LED_B         = 27;
const int PINO_BUZZER        = 13;
const int PINO_BOTAO         = 4;   // INPUT_PULLUP: troca a tela / silencia o alarme
const int PINO_CHAVE_OFFLINE = 14;  // INPUT_PULLUP: chave de teste "sem internet"

const uint8_t ENDERECO_LCD = 0x27;  // LCD 16x2 com módulo I2C (PCF8574)

// ---------------------------------------------------------------------
//  Parâmetros elétricos e de custo (ajustáveis em tempo real via MQTT)
// ---------------------------------------------------------------------
const float TENSAO_PADRAO_V        = 127.0;   // 127 V ou 220 V
const float TARIFA_PADRAO_KWH      = 0.80;    // R$/kWh, tarifa residencial de referência com impostos
const float ADICIONAL_BANDEIRA_KWH = 0.01885; // R$/kWh, bandeira amarela (referência ANEEL)
const float META_PADRAO_REAIS      = 120.0;   // meta mensal da família, em R$

// Faixas do termômetro de energia (custo do mês / meta)
const float LIMITE_AMARELO_PCT  = 70.0;   // < 70%  -> VERDE
const float LIMITE_VERMELHO_PCT = 100.0;  // <= 100% -> AMARELO, > 100% -> VERMELHO

// ---------------------------------------------------------------------
//  Sensor de corrente
// ---------------------------------------------------------------------
// SCT-013-030: 30 A no primário geram 1 V RMS na saída (resistor de carga interno)
const float SCT_AMPERES_POR_VOLT = 30.0;
const int   AMOSTRAS_RMS         = 2000;  // ~2 ciclos de 60 Hz no ESP32
// No Wokwi o potenciômetro representa 0 a 10 A (0 a 1,27 kW em 127 V),
// faixa típica de uma casa e mais fácil de ajustar na demonstração
const float CORRENTE_MAX_SIMULADA_A = 10.0;
// Abaixo disso a leitura é tratada como zero (ruído do sensor/ADC)
const float CORRENTE_RUIDO_A = 0.15;

// ---------------------------------------------------------------------
//  Tempo
// ---------------------------------------------------------------------
#if MODO_SIMULACAO
const float FATOR_TEMPO = 3600.0;  // 1 s real = 1 h de consumo (mês inteiro em 12 min)
#else
const float FATOR_TEMPO = 1.0;
#endif
const float HORAS_MES = 720.0;     // ciclo de faturamento de 30 dias

const unsigned long INTERVALO_LEITURA_MS    = 1000;  // mede e integra o kWh
const unsigned long INTERVALO_PUBLICACAO_MS = 5000;  // envia via MQTT (ou guarda no buffer)
const unsigned long INTERVALO_RECONEXAO_MS  = 5000;  // nova tentativa de conexão ao broker
const unsigned long INTERVALO_LCD_MS        = 500;
const unsigned long INTERVALO_BIP_MS        = 3000;  // alarme sonoro na faixa VERMELHA

// Leituras guardadas enquanto não há internet (60 x 5 s = 5 min de autonomia;
// no modo físico, sem aceleração, aumente o intervalo de publicação)
const int TAMANHO_BUFFER = 60;

// ---------------------------------------------------------------------
//  Tipos
// ---------------------------------------------------------------------
enum Faixa : uint8_t { VERDE, AMARELO, VERMELHO };

// Uma leitura publicada (ou guardada no buffer offline)
struct Leitura {
  uint32_t seq;          // número sequencial: permite medir perda de mensagens
  time_t   epoch;        // hora real da leitura (NTP); 0 = relógio não sincronizado
  float    irms;         // corrente RMS (A)
  float    tensao;       // V
  float    potencia;     // W
  float    kwh;          // kWh acumulado no mês
  float    custo;        // R$ acumulado no mês
  float    tarifa;       // R$/kWh já com bandeira
  float    meta;         // R$
  float    percentual;   // custo / meta (%)
  float    projecao;     // R$ previsto para o fim do mês
  uint8_t  faixa;
  bool     alertaProjecao;
  uint8_t  dia;          // dia do ciclo (1 a 30)
};

// Credenciais do protótipo físico (arquivo fora do Git). No Wokwi ele não existe.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID WIFI_SSID_PADRAO
#endif
#ifndef WIFI_SENHA
#define WIFI_SENHA WIFI_SENHA_PADRAO
#endif
#ifndef MQTT_HOST
#define MQTT_HOST MQTT_HOST_PADRAO
#endif
#ifndef MQTT_PORTA
#define MQTT_PORTA MQTT_PORTA_PADRAO
#endif
#ifndef MQTT_USUARIO
#define MQTT_USUARIO MQTT_USUARIO_PADRAO
#endif
#ifndef MQTT_SENHA
#define MQTT_SENHA MQTT_SENHA_PADRAO
#endif

// ---------------- Objetos ----------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
LiquidCrystal_I2C lcd(ENDERECO_LCD, 16, 2);

// ---------------- Configuração ajustável via MQTT ----------------
float tensaoV     = TENSAO_PADRAO_V;
float tarifaKwh   = TARIFA_PADRAO_KWH;
float bandeiraKwh = ADICIONAL_BANDEIRA_KWH;
float metaReais   = META_PADRAO_REAIS;

// ---------------- Medições do ciclo atual ----------------
float irmsA = 0, potenciaW = 0;
float kwhMes = 0, custoMes = 0, horasMes = 0;
float projecaoMes = 0, percentualMeta = 0;
Faixa faixaAtual = VERDE;
bool alertaProjecao = false;
uint32_t seq = 0;

// ---------------- Buffer offline (fila circular) ----------------
Leitura filaOffline[TAMANHO_BUFFER];
int bufInicio = 0;
int bufQtd = 0;
uint32_t descartadas = 0;

// ---------------- Controle de tempo (millis, sem delay no loop) ----------------
unsigned long ultimaLeitura = 0, ultimaPublicacao = 0, ultimaTentativaMqtt = 0;
unsigned long ultimoLcd = 0, ultimoBip = 0, ultimoClique = 0;

// ---------------- Interface ----------------
int telaLcd = 0;
const int TOTAL_TELAS = 4;
bool alarmeSilenciado = false;
int estadoBotaoAnterior = HIGH;
bool modoOffline = false;   // chave de teste "sem internet"
bool wifiConectadoAntes = false;

// ---------------- Tópicos MQTT ----------------
char topicoEnergia[64], topicoStatus[64], topicoAlerta[64];
char topicoConfig[64], topicoConfigAck[64], topicoFechamento[64];

// Caracteres personalizados do LCD: antena (online) e X (sem internet)
byte iconeOnline[8]  = { B00000, B01110, B10001, B00100, B01010, B00000, B00100, B00000 };
byte iconeOffline[8] = { B00000, B10001, B01010, B00100, B01010, B10001, B00000, B00000 };

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);

  pinMode(PINO_LED_R, OUTPUT);
  pinMode(PINO_LED_G, OUTPUT);
  pinMode(PINO_LED_B, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  pinMode(PINO_CHAVE_OFFLINE, INPUT_PULLUP);
  analogReadResolution(12);  // 0 a 4095

  lcd.init();
  lcd.backlight();
  lcd.createChar(0, iconeOnline);
  lcd.createChar(1, iconeOffline);
  escreverLinha(0, "  Grana Amiga");
  escreverLinha(1, "Medidor de luz");

  testeInicialAtuadores();

  snprintf(topicoEnergia,    sizeof(topicoEnergia),    "%s/energia",    TOPICO_BASE);
  snprintf(topicoStatus,     sizeof(topicoStatus),     "%s/status",     TOPICO_BASE);
  snprintf(topicoAlerta,     sizeof(topicoAlerta),     "%s/alerta",     TOPICO_BASE);
  snprintf(topicoConfig,     sizeof(topicoConfig),     "%s/config",     TOPICO_BASE);
  snprintf(topicoConfigAck,  sizeof(topicoConfigAck),  "%s/config/ack", TOPICO_BASE);
  snprintf(topicoFechamento, sizeof(topicoFechamento), "%s/fechamento", TOPICO_BASE);

  Serial.println();
  Serial.println("==============================================");
  Serial.println(" Grana Amiga - Medidor de energia IoT");
  Serial.printf (" Dispositivo: %s\r\n", CODIGO_DISPOSITIVO);
  Serial.printf (" Modo: %s\r\n", MODO_SIMULACAO ? "SIMULACAO (Wokwi, 1 s = 1 h)" : "FISICO (SCT-013)");
  Serial.printf (" Tensao: %.0f V | Tarifa: R$ %.4f/kWh | Meta: R$ %.2f\r\n",
                 tensaoV, tarifaKwh + bandeiraKwh, metaReais);
  Serial.printf (" Broker: %s:%d | Topico: %s/#\r\n", MQTT_HOST, MQTT_PORTA, TOPICO_BASE);
  Serial.println("==============================================");

  // Wi-Fi conecta em segundo plano: o medidor funciona mesmo sem rede.
  // O relógio (NTP) só é iniciado depois que o Wi-Fi conecta (ver gerenciarRede).
  WiFi.mode(WIFI_STA);
  if (strcmp(WIFI_SSID, "Wokwi-GUEST") == 0) {
    WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);  // canal 6: o Wokwi conecta sem varrer todos os canais
  } else {
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setCallback(aoReceberMensagem);
  mqtt.setBufferSize(512);   // o JSON da leitura passa do limite padrão de 256 bytes
  mqtt.setKeepAlive(60);     // sem sinal por ~90 s, o broker publica o Last Will "offline"
  mqtt.setSocketTimeout(3);  // não trava o medidor se o broker estiver fora

  // Os dois relógios começam juntos: a 1ª leitura só é registrada depois da 1ª medição
  ultimaLeitura = ultimaPublicacao = millis();
}

// =====================================================================
//  LOOP (não bloqueante: cada tarefa roda no seu intervalo)
// =====================================================================
void loop() {
  unsigned long agora = millis();

  lerBotao();
  lerChaveOffline();
  gerenciarRede(agora);

  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    medir(agora);
  }
  if (agora - ultimaPublicacao >= INTERVALO_PUBLICACAO_MS) {
    ultimaPublicacao = agora;
    registrarLeitura();
  }
  descarregarBuffer();
  atualizarAtuadores(agora);

  if (agora - ultimoLcd >= INTERVALO_LCD_MS) {
    ultimoLcd = agora;
    atualizarLcd();
  }
}

// =====================================================================
//  MEDIÇÃO E CÁLCULO
// =====================================================================

// Wokwi: o potenciômetro (0 a 4095) representa 0 a 10 A
float lerIrmsSimulado() {
  int bruto = analogRead(PINO_SENSOR);
  return bruto / 4095.0 * CORRENTE_MAX_SIMULADA_A;
}

// Físico: SCT-013-030 polarizado em 1,65 V (divisor resistivo + capacitor).
// Calcula o valor RMS da onda: raiz da média dos quadrados das amostras,
// depois de remover o nível DC com um filtro passa-baixa (mesma ideia da EmonLib).
float lerIrmsSensor() {
  static float offset = 2048.0;  // nível DC estimado (metade da escala do ADC)
  double somaQuadrados = 0;
  for (int i = 0; i < AMOSTRAS_RMS; i++) {
    int bruto = analogRead(PINO_SENSOR);
    offset += (bruto - offset) / 1024.0;
    float filtrado = bruto - offset;
    somaQuadrados += filtrado * filtrado;
  }
  float rmsContagens = sqrt(somaQuadrados / AMOSTRAS_RMS);
  float rmsVolts = rmsContagens * 3.3 / 4095.0;
  return rmsVolts * SCT_AMPERES_POR_VOLT;
}

Faixa classificar(float percentual) {
  if (percentual < LIMITE_AMARELO_PCT) return VERDE;
  if (percentual <= LIMITE_VERMELHO_PCT) return AMARELO;
  return VERMELHO;
}

const char* nomeFaixa(uint8_t faixa) {
  switch (faixa) {
    case VERDE:   return "VERDE";
    case AMARELO: return "AMARELO";
    default:      return "VERMELHO";
  }
}

void medir(unsigned long agora) {
  // Horas de consumo desde a última medição (aceleradas no modo simulação)
  float dtHoras = (agora - ultimaLeitura) / 3600000.0 * FATOR_TEMPO;
  ultimaLeitura = agora;

  irmsA = MODO_SIMULACAO ? lerIrmsSimulado() : lerIrmsSensor();
  if (irmsA < CORRENTE_RUIDO_A) irmsA = 0;

  potenciaW = irmsA * tensaoV;                    // P = I x V
  kwhMes   += potenciaW / 1000.0 * dtHoras;       // energia = P x tempo
  horasMes += dtHoras;
  custoMes  = kwhMes * (tarifaKwh + bandeiraKwh); // R$ = kWh x tarifa
  percentualMeta = metaReais > 0 ? custoMes / metaReais * 100.0 : 0;

  // Projeção do fim do mês pelo ritmo atual (só depois de 1 dia de dados)
  projecaoMes = horasMes >= 24 ? custoMes / horasMes * HORAS_MES : custoMes;

  Faixa nova = classificar(percentualMeta);
  // Aviso antecipado: ainda dentro da meta, mas no ritmo atual a conta vai estourar
  alertaProjecao = (nova != VERMELHO) && horasMes >= 24 && projecaoMes > metaReais;

  if (nova != faixaAtual) {
    aoMudarFaixa(faixaAtual, nova);
    faixaAtual = nova;
  }

  Serial.printf("[MEDICAO] I=%.2f A | P=%.0f W | %.3f kWh | R$ %.2f | %.1f%% da meta | %s | dia %d | proj. R$ %.2f%s\r\n",
                irmsA, potenciaW, kwhMes, custoMes, percentualMeta, nomeFaixa(faixaAtual),
                diaDoCiclo(), projecaoMes, alertaProjecao ? " (vai estourar)" : "");

  if (horasMes >= HORAS_MES) {
    fecharMes();
  }
}

int diaDoCiclo() {
  int dia = (int)(horasMes / 24.0) + 1;
  return dia > 30 ? 30 : dia;
}

void aoMudarFaixa(Faixa antiga, Faixa nova) {
  Serial.printf("[ALERTA] Faixa mudou: %s -> %s (%.1f%% da meta)\r\n",
                nomeFaixa(antiga), nomeFaixa(nova), percentualMeta);

  if (nova == VERMELHO) alarmeSilenciado = false;     // novo estouro: alarme volta a tocar
  if (nova == AMARELO && antiga == VERDE) tone(PINO_BUZZER, 1500, 150);  // aviso curto

  if (podePublicar()) {
    JsonDocument doc;
    doc["codigo"] = CODIGO_DISPOSITIVO;
    doc["faixa_anterior"] = nomeFaixa(antiga);
    doc["faixa"] = nomeFaixa(nova);
    doc["percentual_meta"] = serialized(String(percentualMeta, 1));
    doc["custo_estimado"] = serialized(String(custoMes, 2));
    doc["meta_mensal_reais"] = serialized(String(metaReais, 2));
    publicarJson(topicoAlerta, doc, false);
  }
}

void zerarCiclo() {
  kwhMes = 0;
  custoMes = 0;
  horasMes = 0;
  projecaoMes = 0;
  percentualMeta = 0;
  alertaProjecao = false;
  faixaAtual = VERDE;
  alarmeSilenciado = false;
}

// Fim do ciclo de 30 dias: publica o resumo do mês e recomeça
void fecharMes() {
  Serial.printf("[FECHAMENTO] Mes encerrado: %.2f kWh | R$ %.2f | meta R$ %.2f | %s\r\n",
                kwhMes, custoMes, metaReais, custoMes <= metaReais ? "DENTRO da meta" : "ACIMA da meta");
  if (podePublicar()) {
    JsonDocument doc;
    doc["codigo"] = CODIGO_DISPOSITIVO;
    doc["kwh"] = serialized(String(kwhMes, 3));
    doc["custo_estimado"] = serialized(String(custoMes, 2));
    doc["meta_mensal_reais"] = serialized(String(metaReais, 2));
    doc["faixa_final"] = nomeFaixa(faixaAtual);
    doc["dentro_da_meta"] = custoMes <= metaReais;
    publicarJson(topicoFechamento, doc, false);
  }
  zerarCiclo();
}

// =====================================================================
//  PUBLICAÇÃO E BUFFER OFFLINE
// =====================================================================

bool podePublicar() {
  return !modoOffline && mqtt.connected();
}

// A cada 5 s: tira um "retrato" das medições e envia ou guarda
void registrarLeitura() {
  Leitura l;
  l.seq = ++seq;
  l.epoch = relogioSincronizado() ? time(nullptr) : 0;
  l.irms = irmsA;
  l.tensao = tensaoV;
  l.potencia = potenciaW;
  l.kwh = kwhMes;
  l.custo = custoMes;
  l.tarifa = tarifaKwh + bandeiraKwh;
  l.meta = metaReais;
  l.percentual = percentualMeta;
  l.projecao = projecaoMes;
  l.faixa = faixaAtual;
  l.alertaProjecao = alertaProjecao;
  l.dia = diaDoCiclo();

  // Se ainda há leituras antigas na fila, a nova entra atrás delas (mantém a ordem)
  if (podePublicar() && bufQtd == 0) {
    if (publicarLeitura(l, false)) return;
  }
  enfileirar(l);
}

void enfileirar(const Leitura& l) {
  if (bufQtd == TAMANHO_BUFFER) {   // fila cheia: descarta a mais antiga
    bufInicio = (bufInicio + 1) % TAMANHO_BUFFER;
    bufQtd--;
    descartadas++;
  }
  filaOffline[(bufInicio + bufQtd) % TAMANHO_BUFFER] = l;
  bufQtd++;
  Serial.printf("[OFFLINE] Leitura #%lu guardada no buffer (%d/%d)\r\n",
                (unsigned long)l.seq, bufQtd, TAMANHO_BUFFER);
}

// Quando a conexão volta, reenvia a fila (até 5 por volta do loop)
void descarregarBuffer() {
  for (int i = 0; i < 5 && bufQtd > 0 && podePublicar(); i++) {
    if (!publicarLeitura(filaOffline[bufInicio], true)) break;
    bufInicio = (bufInicio + 1) % TAMANHO_BUFFER;
    bufQtd--;
    if (bufQtd == 0) Serial.println("[OFFLINE] Buffer descarregado: todas as leituras reenviadas");
  }
}

bool publicarLeitura(const Leitura& l, bool reenvio) {
  JsonDocument doc;
  // Nomes iguais às colunas de dispositivo/leitura_energia no banco do grupo
  doc["codigo"] = CODIGO_DISPOSITIVO;
  doc["seq"] = l.seq;
  char dt[32];
  if (formatarDataHora(l.epoch, dt, sizeof(dt))) doc["dt_leitura"] = dt;
  else doc["dt_leitura"] = nullptr;
  doc["irms_a"] = serialized(String(l.irms, 2));
  doc["tensao_v"] = serialized(String(l.tensao, 0));
  doc["potencia_w"] = serialized(String(l.potencia, 1));
  doc["kwh"] = serialized(String(l.kwh, 3));
  doc["custo_estimado"] = serialized(String(l.custo, 2));
  doc["tarifa_kwh"] = serialized(String(l.tarifa, 5));
  doc["meta_mensal_reais"] = serialized(String(l.meta, 2));
  doc["percentual_meta"] = serialized(String(l.percentual, 1));
  doc["faixa"] = nomeFaixa(l.faixa);
  doc["projecao_mes"] = serialized(String(l.projecao, 2));
  doc["alerta_projecao"] = l.alertaProjecao;
  doc["dia_ciclo"] = l.dia;
  doc["buffer_pendente"] = reenvio ? bufQtd - 1 : bufQtd;  // quantas ainda faltam depois desta
  doc["descartadas"] = descartadas;
  doc["reenvio"] = reenvio;
  doc["origem"] = "IOT";
  return publicarJson(topicoEnergia, doc, false);
}

bool publicarJson(const char* topico, JsonDocument& doc, bool retain) {
  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  bool ok = mqtt.publish(topico, (const uint8_t*)payload, n, retain);
  Serial.printf("[MQTT] %s %s %s\r\n", ok ? "->" : "FALHOU", topico, payload);
  return ok;
}

// =====================================================================
//  REDE: Wi-Fi + MQTT
// =====================================================================

void gerenciarRede(unsigned long agora) {
  // Chave de teste ligada: derruba a conexão de forma abrupta (como uma queda real).
  // O broker percebe pelo keep-alive e publica o Last Will "offline".
  if (modoOffline) {
    if (mqtt.connected()) wifiClient.stop();
    return;
  }

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  if (wifiOk != wifiConectadoAntes) {
    wifiConectadoAntes = wifiOk;
    if (wifiOk) {
      Serial.printf("[WIFI] Conectado a \"%s\" | IP %s | RSSI %d dBm\r\n",
                    WIFI_SSID, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      // Iniciar o SNTP antes do Wi-Fi subir derrubava o ESP32 (assert do lwIP no core 3.x)
      static bool ntpIniciado = false;
      if (!ntpIniciado) {
        configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");  // horário de Brasília
        ntpIniciado = true;
      }
    } else {
      Serial.println("[WIFI] Sem conexao (o medidor continua funcionando)");
    }
  }
  if (!wifiOk) return;

  if (mqtt.connected()) {
    mqtt.loop();
  } else if (agora - ultimaTentativaMqtt >= INTERVALO_RECONEXAO_MS || ultimaTentativaMqtt == 0) {
    ultimaTentativaMqtt = agora;
    conectarMqtt();
  }
}

void conectarMqtt() {
  // Client ID fixo (sem sufixo aleatório): ao reconectar depois de um reboot, o broker
  // derruba na hora a sessão antiga com o mesmo ID. Com ID aleatório, o Last Will da
  // sessão morta chegava ~90 s depois do "online" novo e deixava o status "offline" retido.
  String clientId = String("granaamiga-") + CODIGO_DISPOSITIVO;
  const char* usuario = strlen(MQTT_USUARIO) > 0 ? MQTT_USUARIO : nullptr;
  const char* senha   = strlen(MQTT_SENHA)   > 0 ? MQTT_SENHA   : nullptr;

  Serial.printf("[MQTT] Conectando a %s:%d ...\r\n", MQTT_HOST, MQTT_PORTA);
  // Last Will: se o medidor sumir sem avisar, o broker publica "offline" (retido)
  if (mqtt.connect(clientId.c_str(), usuario, senha, topicoStatus, 1, true, "offline")) {
    mqtt.publish(topicoStatus, "online", true);
    mqtt.subscribe(topicoConfig, 1);
    Serial.printf("[MQTT] Conectado como %s | assinando %s\r\n", clientId.c_str(), topicoConfig);
  } else {
    Serial.printf("[MQTT] Falha (rc=%d). Nova tentativa em %lu s\r\n",
                  mqtt.state(), INTERVALO_RECONEXAO_MS / 1000);
  }
}

// Comandos recebidos em granaamiga/casa/<codigo>/config, por exemplo:
// {"meta_mensal_reais": 150} | {"tarifa_kwh": 0.85} | {"tensao_v": 220} | {"comando": "zerar_mes"}
void aoReceberMensagem(char* topico, byte* payload, unsigned int tamanho) {
  JsonDocument doc;
  DeserializationError erro = deserializeJson(doc, payload, tamanho);
  if (erro) {
    Serial.printf("[CONFIG] JSON invalido em %s: %s\r\n", topico, erro.c_str());
    return;
  }

  int aplicados = 0;
  JsonDocument ack;
  JsonArray rejeitados = ack["rejeitados"].to<JsonArray>();

  if (!doc["meta_mensal_reais"].isNull()) {
    float v = doc["meta_mensal_reais"].as<float>();
    if (v > 0 && v <= 10000) { metaReais = v; aplicados++; } else rejeitados.add("meta_mensal_reais");
  }
  if (!doc["tarifa_kwh"].isNull()) {
    float v = doc["tarifa_kwh"].as<float>();
    if (v > 0 && v < 5) { tarifaKwh = v; aplicados++; } else rejeitados.add("tarifa_kwh");
  }
  if (!doc["bandeira_kwh"].isNull()) {
    float v = doc["bandeira_kwh"].as<float>();
    if (v >= 0 && v < 1) { bandeiraKwh = v; aplicados++; } else rejeitados.add("bandeira_kwh");
  }
  if (!doc["tensao_v"].isNull()) {
    int v = doc["tensao_v"].as<int>();
    if (v == 127 || v == 220) { tensaoV = v; aplicados++; } else rejeitados.add("tensao_v");
  }
  if (doc["comando"] == "zerar_mes") {
    zerarCiclo();
    aplicados++;
    Serial.println("[CONFIG] Ciclo zerado por comando remoto");
  }

  Serial.printf("[CONFIG] %d ajuste(s) aplicado(s) | meta R$ %.2f | tarifa R$ %.5f/kWh | %.0f V\r\n",
                aplicados, metaReais, tarifaKwh + bandeiraKwh, tensaoV);

  ack["codigo"] = CODIGO_DISPOSITIVO;
  ack["aplicados"] = aplicados;
  ack["meta_mensal_reais"] = serialized(String(metaReais, 2));
  ack["tarifa_kwh"] = serialized(String(tarifaKwh + bandeiraKwh, 5));
  ack["tensao_v"] = serialized(String(tensaoV, 0));
  publicarJson(topicoConfigAck, ack, false);
}

bool relogioSincronizado() {
  return time(nullptr) > 1700000000;  // depois de nov/2023 = NTP respondeu
}

bool formatarDataHora(time_t epoch, char* saida, size_t tamanho) {
  if (epoch == 0) return false;
  struct tm local;
  localtime_r(&epoch, &local);
  strftime(saida, tamanho, "%Y-%m-%dT%H:%M:%S-03:00", &local);
  return true;
}

// =====================================================================
//  ENTRADAS: botão e chave de teste
// =====================================================================

void lerBotao() {
  int leitura = digitalRead(PINO_BOTAO);
  if (leitura == LOW && estadoBotaoAnterior == HIGH && millis() - ultimoClique > 200) {
    ultimoClique = millis();
    if (faixaAtual == VERMELHO && !alarmeSilenciado) {
      alarmeSilenciado = true;     // com alarme tocando, o toque só silencia
      noTone(PINO_BUZZER);
      Serial.println("[BOTAO] Alarme silenciado");
    } else {
      telaLcd = (telaLcd + 1) % TOTAL_TELAS;
      atualizarLcd();
    }
  }
  estadoBotaoAnterior = leitura;
}

void lerChaveOffline() {
  bool ligada = digitalRead(PINO_CHAVE_OFFLINE) == LOW;
  if (ligada != modoOffline) {
    modoOffline = ligada;
    Serial.println(ligada ? "[TESTE] Chave offline LIGADA: simulando queda da internet"
                          : "[TESTE] Chave offline DESLIGADA: reconectando");
    if (!ligada) ultimaTentativaMqtt = 0;  // tenta reconectar imediatamente
  }
}

// =====================================================================
//  SAÍDAS: LED RGB, buzzer e LCD
// =====================================================================

void acenderLed(bool r, bool g, bool b) {
  digitalWrite(PINO_LED_R, r);
  digitalWrite(PINO_LED_G, g);
  digitalWrite(PINO_LED_B, b);
}

void testeInicialAtuadores() {
  acenderLed(1, 0, 0); delay(250);
  acenderLed(1, 1, 0); delay(250);
  acenderLed(0, 1, 0); delay(250);
  acenderLed(0, 0, 1); delay(250);
  tone(PINO_BUZZER, 2000, 100);
  delay(400);
}

void atualizarAtuadores(unsigned long agora) {
  switch (faixaAtual) {
    case VERDE:   acenderLed(0, 1, 0); break;
    case AMARELO: acenderLed(1, 1, 0); break;  // vermelho + verde = amarelo
    default:      acenderLed(1, 0, 0); break;
  }
  if (faixaAtual == VERMELHO && !alarmeSilenciado && agora - ultimoBip >= INTERVALO_BIP_MS) {
    ultimoBip = agora;
    tone(PINO_BUZZER, 2000, 400);
  }
}

void escreverLinha(int linha, const char* texto) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", texto);
  lcd.setCursor(0, linha);
  lcd.print(buf);
}

// 1234.5 -> "1234,50" (formato brasileiro)
void formatarReais(float valor, char* saida, size_t tamanho) {
  snprintf(saida, tamanho, "%.2f", valor);
  for (char* p = saida; *p; p++) if (*p == '.') *p = ',';
}

void atualizarLcd() {
  char l1[17], l2[17], r1[12], r2[12];
  bool online = podePublicar();

  switch (telaLcd) {
    case 0:  // agora: potência, custo do mês e faixa
      formatarReais(custoMes, r1, sizeof(r1));
      snprintf(l1, sizeof(l1), "%4.0fW R$%s", potenciaW, r1);
      snprintf(l2, sizeof(l2), "%-8s %3d%%", nomeFaixa(faixaAtual),
               (int)min(percentualMeta, 999.0f));
      escreverLinha(0, l1);
      escreverLinha(1, l2);
      lcd.setCursor(15, 1);
      lcd.write(byte(online ? 0 : 1));
      break;

    case 1:  // energia do mês e projeção
      formatarReais(kwhMes, r1, sizeof(r1));
      formatarReais(projecaoMes, r2, sizeof(r2));
      snprintf(l1, sizeof(l1), "Mes %s kWh", r1);
      snprintf(l2, sizeof(l2), "Prev R$%s%s", r2, alertaProjecao ? "!" : "");
      escreverLinha(0, l1);
      escreverLinha(1, l2);
      break;

    case 2:  // meta e dia do ciclo
      formatarReais(metaReais, r1, sizeof(r1));
      snprintf(l1, sizeof(l1), "Meta R$%s", r1);
      snprintf(l2, sizeof(l2), "Dia %d de 30", diaDoCiclo());
      escreverLinha(0, l1);
      escreverLinha(1, l2);
      break;

    default:  // rede
      snprintf(l1, sizeof(l1), "MQTT %s Buf %d", online ? "ON " : "OFF", bufQtd);
      if (modoOffline) snprintf(l2, sizeof(l2), "Sem internet");
      else if (WiFi.status() == WL_CONNECTED) snprintf(l2, sizeof(l2), "%s", WiFi.localIP().toString().c_str());
      else snprintf(l2, sizeof(l2), "Wi-Fi...");
      escreverLinha(0, l1);
      escreverLinha(1, l2);
      break;
  }
}
