# Testes, evidências e limitações — Medidor de energia Grana Amiga

Este documento reúne os casos de teste, os resultados obtidos, as métricas e a análise das limitações do protótipo IoT.
Os testes foram executados em **12/11/2026** em três ambientes:

| Ambiente | O que foi testado | Onde estão as evidências |
| --- | --- | --- |
| **Wokwi** (ESP32 simulado + Wi-Fi virtual `Wokwi-GUEST`) | Firmware completo: leitura, cálculo, LCD, LED, buzzer, Wi-Fi, MQTT, buffer, comando remoto | `evidencias/serial-wokwi.txt`, `evidencias/serial-wokwi-reteste.txt`, `evidencias/01-*.jpg`, `evidencias/02-*.jpg`, `evidencias/05-*.png` |
| **Broker público HiveMQ** + assinante independente (`mosquitto_sub`) | O que realmente trafegou na rede, com horário de chegada | `evidencias/mqtt-log.txt`, `evidencias/mqtt-log-reconexao.txt` |
| **Node-RED** em container Docker + `tools/simulador_medidor.py` | Validação, métricas de entrega, perda, reenvio, dashboard, CSV | `evidencias/leituras-node-red.csv`, `evidencias/03-*.jpg`, `evidencias/04-*.jpg` |

> O simulador em Python usa **as mesmas fórmulas e o mesmo JSON** do firmware. Ele existe para testar a parte de rede e nuvem sem depender do Wokwi, que roda mais devagar que o tempo real (ver Limitações).

---

## 1. Critérios adotados

| Critério | Como é medido | Meta |
| --- | --- | --- |
| **Exatidão do cálculo** | Valor publicado × valor calculado à mão (P = I × V; kWh = P × t; R$ = kWh × tarifa) | Diferença ≤ R$ 0,01 (arredondamento) |
| **Classificação da faixa** | Faixa publicada × regra (< 70% verde · ≤ 100% amarelo · > 100% vermelho) | 100% de acerto |
| **Taxa de entrega** | Leituras únicas recebidas ÷ maior `seq` recebido (Node-RED) | ≥ 99% em rede estável; perdas sempre **detectadas** |
| **Recuperação após queda** | Leituras guardadas no buffer e reenviadas com `"reenvio": true`, na ordem, sem duplicar | 100% reenviadas enquanto o buffer (60) não estoura |
| **Atraso de entrega** | Hora de chegada no Node-RED − `dt_leitura` (relógio NTP do ESP32) | < 2 s ao vivo (resolução de 1 s) |
| **Detecção de medidor offline** | Last Will publicado pelo broker quando o medidor some sem avisar | Em até 1,5 × keep-alive (90 s) |
| **Robustez a entrada inválida** | Payload sem campos obrigatórios ou comando fora da faixa | Rejeitado e registrado; nada é gravado |

---

## 2. Casos de teste e resultados

| # | Caso de teste | Entrada / passos | Resultado esperado | Resultado obtido | Status |
| --- | --- | --- | --- | --- | --- |
| CT-01 | Leitura do sensor e potência | Potenciômetro em ~49% no Wokwi | I ≈ 4,9 A; P = I × 127 V | `I=4.93 A \| P=626 W` (4,93 × 127 = 626,1) | ✅ |
| CT-02 | Integração de energia e custo | Mesma carga por ~1,5 h simuladas | kWh × R$ 0,81885 | `0.971 kWh \| R$ 0.80` (0,971 × 0,81885 = 0,795) | ✅ |
| CT-03 | Conexão Wi-Fi + MQTT e status | Iniciar a simulação | Conecta em `Wokwi-GUEST`, publica `online` (retido) em `…/status` | `[WIFI] Conectado … IP 10.10.0.2` e `status online` no assinante externo | ✅ |
| CT-04 | Buffer offline na inicialização | Primeira leitura sai antes de o Wi-Fi conectar | Leitura guardada e reenviada ao conectar | `[OFFLINE] Leitura #1 guardada` → publicada com `"reenvio":true` → `Buffer descarregado` (nessa execução a leitura saiu zerada; ver CT-15) | ✅ |
| CT-05 | Comando remoto (nuvem → ESP32) | `mosquitto_pub … /config -m '{"meta_mensal_reais": 1.5}'` | Meta aplicada e confirmada em `…/config/ack` | `[CONFIG] 1 ajuste(s) aplicado(s) \| meta R$ 1.50` + ack com `"rejeitados":[]` | ✅ |
| CT-06 | Mudança de faixa e atuadores | Custo R$ 1,82 com meta R$ 1,50 | 121,3% → VERMELHO, LED vermelho, buzzer, alerta MQTT | `[ALERTA] Faixa mudou: VERDE -> VERMELHO (121.3% da meta)`, LED vermelho e buzzer (`02-wokwi-vermelho-alarme.jpg`) | ✅ |
| CT-07 | Last Will (medidor some) | Simulação parou de responder ao broker | Broker publica `offline` sozinho | `status offline` recebido 22 s depois da última mensagem (keep-alive de 15 s na época) | ✅ |
| CT-08 | Payload inválido | Simulador com `--invalida` (JSON sem campos) | Node-RED rejeita e não grava no CSV | Mensagem com `"seq": 0` chegou ao broker (log), **não** aparece no CSV | ✅ |
| CT-09 | Detecção de perda | Simulador descarta 1 a cada 10 (`--perder-a-cada 10`), 144 leituras | Node-RED aponta 12 perdidas | Dashboard: **132 recebidas · 12 perdidas · entrega 91,7%** (132 ÷ 144) | ✅ |
| CT-10 | Reenvio após queda | Simulador retém as leituras 40 a 55 (`--offline 40-55`) | 16 reenviadas na ordem, sem duplicar | CSV: **16 linhas com `reenvio=true`**, 0 duplicadas | ✅ |
| CT-11 | Fechamento do mês | 144 leituras × 5 h = 720 h a 230 W | 165,6 kWh e R$ 135,60 (acima da meta) | `fechamento {"kwh": 165.6, "custo_estimado": 135.6, …}` e o mesmo valor no dashboard | ✅ |
| CT-12 | Aviso antecipado (projeção) | Dia 3, 12% da meta, ritmo de 230 W | Ainda VERDE, mas prevê estouro | Dashboard: `VERDE` com `Previsão R$ 135,60 ⚠ vai passar da meta` (`03-dashboard-previsao.jpg`) | ✅ |
| CT-13 | Boot do ESP32 | Reiniciar a simulação várias vezes | Nenhum travamento | 1ª rodada: **1º boot travou** (`assert failed` no lwIP ao conectar no MQTT). Causa: SNTP iniciado antes do Wi-Fi. Corrigido: `configTime()` só depois do Wi-Fi conectar. Reteste (4 boots): todos limpos, `[WIFI] Conectado` → `[MQTT] Conectado` (`serial-wokwi-reteste.txt`) | ✅ |
| CT-14 | Chave "sem internet" | Mover a chave deslizante com o medidor conectado | `[TESTE] Chave offline LIGADA`, ícone X no LCD, leituras no buffer; ao voltar, reenvio | Leituras #6 a #10 no buffer (5/60), ícone X no LCD (`05-wokwi-offline-buffer.png`), broker publicou `offline` na hora. Ao desligar a chave: reconexão e reenvio de `seq` 6 a 10 **na ordem**, `"reenvio":true`, `buffer_pendente` 4 → 0; `seq` 11 volta ao fluxo normal | ✅ |
| CT-15 | Primeira leitura após o boot | Observar a `seq` 1 no reteste | Valores reais de corrente, kWh e custo | **Bug encontrado:** a `seq` 1 saía com tudo zerado, porque era registrada no 1º `loop()`, antes da 1ª medição (`ultimaPublicacao` começava em 0). Corrigido: os relógios de leitura e publicação começam juntos no `setup()`. Depois: `seq` 1 com `2.71 A`, `1.555 kWh`, `R$ 1.27` | ✅ corrigido e verificado |
| CT-16 | Status depois de um reinício rápido (luz pisca) | Parar a simulação e religar em poucos segundos; assinante externo por 100 s | Status retido termina `online` | **Bug encontrado:** com client ID aleatório, o Last Will da sessão morta chegava ~90 s **depois** do `online` novo e o status ficava `offline` com o medidor funcionando. Corrigido: client ID fixo `granaamiga-GA-ESP32-001`; o broker derruba a sessão antiga na hora. Depois: `offline` 22:26:59 → `online` 22:27:00, nenhum `offline` atrasado, status retido `online` (`mqtt-log-reconexao.txt`) | ✅ corrigido e verificado |

### Como reproduzir os testes

```bash
# CT-05: comando remoto (qualquer cliente MQTT serve; aqui com Docker)
docker run --rm eclipse-mosquitto:2.0.20 mosquitto_pub -h broker.hivemq.com -t granaamiga/casa/GA-ESP32-001/config -m '{"meta_mensal_reais": 1.5}'

# Assinante independente (o que trafega na rede)
docker run --rm eclipse-mosquitto:2.0.20 mosquitto_sub -h broker.hivemq.com -t 'granaamiga/casa/#' -v

# CT-08 a CT-12: simulador contra o Node-RED
python tools/simulador_medidor.py --potencia 230 --perder-a-cada 10 --offline 40-55 --invalida
```

---

## 3. Métricas obtidas

| Métrica | Valor | Observação |
| --- | --- | --- |
| Erro de cálculo (potência, kWh, R$) | **≤ R$ 0,01** | Diferença só de arredondamento (CT-01, CT-02, CT-11) |
| Acerto da faixa | **100%** (todas as leituras do CSV) | Regra conferida linha a linha pelo percentual publicado |
| Taxa de entrega com perda induzida de 10% | **91,7%** (132/144) | O sistema **detectou todas as 12 perdas** pelo `seq` |
| Reenvio após queda | **16 de 16** (100%) | Na ordem e sem duplicatas |
| Atraso médio ao vivo (rede doméstica → HiveMQ → Node-RED) | **0,6 a 0,9 s** | `dt_leitura` tem resolução de 1 s |
| Detecção de medidor offline | **≈ 22 s** com keep-alive 15 s | Keep-alive final de 60 s → detecção em até 90 s |
| Tamanho da mensagem de leitura | ~390 bytes (JSON) | A cada 5 s ≈ 6,7 MB/mês por casa, viável até em 3G/4G |
| Autonomia do buffer offline | 60 leituras = 5 min (simulação) | No modo físico, com envio a cada 60 s, equivale a 1 h |

---

## 4. Limitações e como seriam tratadas

| Limitação | Impacto | Evolução proposta |
| --- | --- | --- |
| **Potenciômetro no lugar do SCT-013** (o Wokwi não tem sensor de corrente AC) | A simulação lê um valor DC já "em ampères"; o cálculo RMS (`lerIrmsSensor`) só roda no protótipo físico | Montar com SCT-013-030 + divisor de 1,65 V e calibrar com um multímetro alicate |
| **Tensão fixa (127/220 V) e fator de potência = 1** | Mede potência aparente; com motores (geladeira, ventilador) superestima um pouco | Medir também a tensão (ZMPT101B) ou usar módulo PZEM-004T |
| **Tarifa simplificada** (valor único + bandeira) | Não considera tarifa social, faixas de consumo nem impostos variáveis | Tarifa vinda da API por distribuidora, ajustável pelo app (já existe o comando `tarifa_kwh`) |
| **Broker público sem TLS nem senha** | Qualquer pessoa que souber o tópico pode ler ou publicar | Broker próprio (Mosquitto na VM Azure) ou HiveMQ Cloud com TLS na porta 8883, usuário por medidor e ACL por tópico; credenciais em `secrets.h` |
| **PubSubClient só publica com QoS 0** | O broker não confirma a entrega da leitura | Compensado pelo `seq` (detecta perda) + buffer; evolução: biblioteca com QoS 1 |
| **Buffer em RAM** | Se faltar energia, as leituras não enviadas se perdem | Guardar a fila na flash (Preferences/LittleFS) |
| **Sem bateria** | Queda de energia na casa desliga o medidor | Supercapacitor/bateria pequena para registrar a queda |
| **Mede o quadro inteiro** | Não sabe qual aparelho gasta mais | Desagregação de carga com IA a partir do perfil de consumo (evolução IoB) |
| **Simulação mais lenta que o tempo real no Wokwi** (3% a 30%) | O broker conta o keep-alive em tempo real e derruba a sessão; por isso o keep-alive subiu para 60 s | No físico não acontece; no Wokwi, manter a aba em primeiro plano |
| **Ponte HTTP para a API Java desativada** | As leituras ainda não chegam ao PostgreSQL | Ativar o nó quando a API publicar `POST /api/leituras` (contrato já montado no nó "Montar leitura_energia") |

### Segurança elétrica
O SCT-013 é um **alicate não invasivo**: abraça um dos fios do quadro sem cortar nem desencapar. Nunca usar sensores em série com a rede (como o ACS712) para medir a casa inteira. A instalação real deve ser feita por eletricista.

---

## 5. Situações que o professor pode propor (respostas preparadas)

| Situação | O que acontece |
| --- | --- |
| A internet da casa cai | O medidor continua medindo, mostrando no LCD e acionando LED/buzzer. As leituras vão para o buffer e são reenviadas com `reenvio: true` quando a conexão volta. O broker publica `offline` (Last Will) e o dashboard avisa. |
| A luz pisca e o medidor reinicia | Reconecta sozinho com o mesmo client ID; o broker encerra a sessão antiga e o status volta a `online` em segundos (CT-16). As leituras não enviadas que estavam na RAM se perdem (ver Limitações: buffer em RAM). |
| O broker sai do ar | Mesmo comportamento: tentativas de reconexão a cada 5 s sem travar o medidor (`setSocketTimeout(3)`). |
| Duas casas com medidor | Cada uma tem seu `CODIGO_DISPOSITIVO`; os tópicos ficam `granaamiga/casa/<codigo>/…` e o Node-RED assina `granaamiga/casa/+/…`. |
| A casa é 220 V | Comando `{"tensao_v": 220}` em `…/config`, sem regravar o firmware. |
| Alguém publica dados falsos | Hoje é possível no broker público. Em produção: TLS + usuário por medidor + ACL que só deixa cada medidor publicar no próprio tópico. |
| Escalar para 10 mil casas | 10 mil × 1 msg/60 s ≈ 170 msg/s: tranquilo para um broker em cluster. O Node-RED vira só ponte; a API grava em lote. |
| A família não tem smartphone | O LCD, o LED e o buzzer funcionam sozinhos, sem app e sem internet. |
