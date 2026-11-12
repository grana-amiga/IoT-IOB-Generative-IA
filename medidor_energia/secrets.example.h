// =====================================================================
//  Modelo de credenciais para o protótipo FÍSICO.
//  1. Copie este arquivo para "secrets.h" na mesma pasta.
//  2. Preencha com os dados reais.
//  secrets.h está no .gitignore: as credenciais nunca vão para o GitHub.
//
//  No Wokwi não é preciso: sem secrets.h o firmware usa a rede
//  "Wokwi-GUEST" e o broker público definidos no início do .ino.
// =====================================================================
#pragma once

#define WIFI_SSID     "nome-da-sua-rede"
#define WIFI_SENHA    "senha-da-sua-rede"

#define MQTT_HOST     "broker.hivemq.com"
#define MQTT_PORTA    1883
#define MQTT_USUARIO  ""   // preencher ao usar um broker com autenticação
#define MQTT_SENHA    ""
