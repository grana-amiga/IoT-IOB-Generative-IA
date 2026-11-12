# Node-RED com o dashboard e o fluxo do Grana Amiga já carregados
FROM nodered/node-red:4.0.9

# Dashboard (/ui): gauge, gráfico, textos, formulário e avisos
RUN npm install --no-audit --no-fund --omit=dev node-red-dashboard@3.6.6

# Fluxo versionado no repositório
COPY flows.json /data/flows.json

# Valores padrão (sobrescritos pelo docker-compose / .env)
ENV MQTT_HOST=broker.hivemq.com \
    MQTT_PORT=1883 \
    DISPOSITIVO=GA-ESP32-001 \
    API_URL=http://localhost:8080 \
    TZ=America/Sao_Paulo

EXPOSE 1880
