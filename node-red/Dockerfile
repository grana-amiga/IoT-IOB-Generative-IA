# Node-RED com o dashboard e o fluxo do Grana Amiga já carregados
FROM nodered/node-red:4.0.9

# Dashboard (/ui): gauge, gráfico, textos, formulário e avisos
RUN npm install --no-audit --no-fund --omit=dev node-red-dashboard@3.6.6

# Fluxo versionado no repositório. Fica FORA de /data de propósito: /data é um volume,
# e o volume guardaria o fluxo da primeira subida, ignorando as versões novas do build.
COPY --chown=node-red:node-red flows.json /usr/src/node-red/fluxo/flows.json

# Valores padrão (sobrescritos pelo docker-compose / .env)
ENV FLOWS=/usr/src/node-red/fluxo/flows.json \
    MQTT_HOST=broker.hivemq.com \
    MQTT_PORT=1883 \
    DISPOSITIVO=GA-ESP32-001 \
    API_URL=http://host.docker.internal:8080 \
    TZ=America/Sao_Paulo

EXPOSE 1880
