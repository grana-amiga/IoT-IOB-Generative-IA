"""
Simulador do medidor Grana Amiga (sem hardware e sem Wokwi).

Publica no broker MQTT o MESMO JSON que o ESP32 publica, usando as mesmas
fórmulas do firmware. Serve para testar o Node-RED e gerar métricas
(perda de mensagens, reenvio após queda, rejeição de payload inválido).

Uso:
    pip install -r tools/requirements.txt
    python tools/simulador_medidor.py                      # mês completo, 250 W
    python tools/simulador_medidor.py --potencia 600       # passa da meta
    python tools/simulador_medidor.py --perder-a-cada 10   # descarta 1 em cada 10 (teste de perda)
    python tools/simulador_medidor.py --offline 20-35      # leituras 20 a 35 ficam "presas" e são reenviadas
    python tools/simulador_medidor.py --invalida           # envia também um payload inválido
"""
import argparse
import json
import time
from datetime import datetime, timedelta, timezone

import paho.mqtt.client as mqtt

# Mesmas constantes do firmware (medidor_energia.ino)
TENSAO_V = 127.0
TARIFA_KWH = 0.80 + 0.01885      # tarifa + bandeira amarela
META_REAIS = 120.0
HORAS_MES = 720.0
LIMITE_AMARELO = 70.0
LIMITE_VERMELHO = 100.0
BRT = timezone(timedelta(hours=-3))


def faixa(percentual):
    if percentual < LIMITE_AMARELO:
        return "VERDE"
    if percentual <= LIMITE_VERMELHO:
        return "AMARELO"
    return "VERMELHO"


def leitura(seq, kwh, horas, potencia, meta, epoch, reenvio=False, pendente=0):
    custo = kwh * TARIFA_KWH
    pct = custo / meta * 100
    proj = custo / horas * HORAS_MES if horas >= 24 else custo
    f = faixa(pct)
    return {
        "codigo": ARGS.codigo, "seq": seq,
        "dt_leitura": datetime.fromtimestamp(epoch, BRT).strftime("%Y-%m-%dT%H:%M:%S-03:00"),
        "irms_a": round(potencia / TENSAO_V, 2), "tensao_v": int(TENSAO_V),
        "potencia_w": round(potencia, 1), "kwh": round(kwh, 3), "custo_estimado": round(custo, 2),
        "tarifa_kwh": round(TARIFA_KWH, 5), "meta_mensal_reais": round(meta, 2),
        "percentual_meta": round(pct, 1), "faixa": f, "projecao_mes": round(proj, 2),
        "alerta_projecao": f != "VERMELHO" and horas >= 24 and proj > meta,
        "dia_ciclo": min(int(horas / 24) + 1, 30), "buffer_pendente": pendente,
        "descartadas": 0, "reenvio": reenvio, "origem": "IOT",
    }


def main():
    base = f"granaamiga/casa/{ARGS.codigo}"
    cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"granaamiga-sim-{int(time.time())}")
    cli.will_set(f"{base}/status", "offline", qos=1, retain=True)
    cli.connect(ARGS.host, ARGS.porta, keepalive=60)
    cli.loop_start()
    cli.publish(f"{base}/status", "online", qos=1, retain=True)

    off_ini, off_fim = (map(int, ARGS.offline.split("-")) if ARGS.offline else (0, -1))
    presas = []
    kwh = horas = 0.0
    faixa_ant = "VERDE"
    stats = {"enviadas": 0, "descartadas_de_proposito": 0, "presas_e_reenviadas": 0}
    n = int(HORAS_MES / ARGS.horas_por_envio)

    if ARGS.invalida:
        cli.publish(f"{base}/energia", json.dumps({"codigo": ARGS.codigo, "seq": 0}), qos=1)
        print("[TESTE] payload inválido enviado (faltam campos)")

    for seq in range(1, n + 1):
        horas += ARGS.horas_por_envio
        kwh += ARGS.potencia / 1000 * ARGS.horas_por_envio
        msg = leitura(seq, kwh, horas, ARGS.potencia, ARGS.meta, time.time())
        topico = f"{base}/energia"

        if msg["faixa"] != faixa_ant:
            cli.publish(f"{base}/alerta", json.dumps({
                "codigo": ARGS.codigo, "faixa_anterior": faixa_ant, "faixa": msg["faixa"],
                "percentual_meta": msg["percentual_meta"], "custo_estimado": msg["custo_estimado"],
                "meta_mensal_reais": msg["meta_mensal_reais"]}), qos=1)
            faixa_ant = msg["faixa"]

        if off_ini <= seq <= off_fim:                      # "sem internet": guarda
            presas.append(msg)
            print(f"[OFFLINE] #{seq} guardada ({len(presas)})")
        elif ARGS.perder_a_cada and seq % ARGS.perder_a_cada == 0:
            stats["descartadas_de_proposito"] += 1          # simula pacote perdido
            print(f"[PERDA] #{seq} descartada de propósito")
        else:
            for i, p in enumerate(presas):                   # internet voltou: reenvia na ordem
                p["reenvio"] = True
                p["buffer_pendente"] = len(presas) - i - 1
                cli.publish(topico, json.dumps(p), qos=1)
                stats["presas_e_reenviadas"] += 1
            presas.clear()
            cli.publish(topico, json.dumps(msg), qos=1)
            stats["enviadas"] += 1
            print(f"[MQTT] #{seq} dia {msg['dia_ciclo']} {msg['potencia_w']} W R$ {msg['custo_estimado']} "
                  f"{msg['percentual_meta']}% {msg['faixa']}")
        time.sleep(ARGS.intervalo)

    custo = kwh * TARIFA_KWH
    cli.publish(f"{base}/fechamento", json.dumps({
        "codigo": ARGS.codigo, "kwh": round(kwh, 3), "custo_estimado": round(custo, 2),
        "meta_mensal_reais": ARGS.meta, "faixa_final": faixa_ant, "dentro_da_meta": custo <= ARGS.meta}), qos=1)
    time.sleep(1)
    cli.loop_stop()
    cli.disconnect()
    print("\nResumo:", json.dumps(stats, ensure_ascii=False))
    print(f"Esperado no Node-RED: {n} leituras numeradas, perdidas = {stats['descartadas_de_proposito']}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Simulador do medidor Grana Amiga")
    ap.add_argument("--host", default="broker.hivemq.com")
    ap.add_argument("--porta", type=int, default=1883)
    ap.add_argument("--codigo", default="GA-SIM-001", help="código do medidor simulado")
    ap.add_argument("--potencia", type=float, default=250.0, help="potência média da casa (W)")
    ap.add_argument("--meta", type=float, default=META_REAIS)
    ap.add_argument("--horas-por-envio", type=float, default=5.0, help="horas de consumo por mensagem (firmware: 5)")
    ap.add_argument("--intervalo", type=float, default=0.5, help="segundos reais entre mensagens")
    ap.add_argument("--perder-a-cada", type=int, default=0)
    ap.add_argument("--offline", default="", help="faixa de seq retida, ex.: 20-35")
    ap.add_argument("--invalida", action="store_true")
    ARGS = ap.parse_args()
    main()
