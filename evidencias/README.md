# Evidencias de validacao

Execucao em 4 de novembro de 2026. Apenas sinais simulados, sem dados pessoais.

## Firmware ESP32 no Wokwi

Projeto publico: https://wokwi.com/projects/479795189911514113.

| Arquivo | Evidencia |
| --- | --- |
| [wokwi-online.png](wokwi-online.png) | Firmware compilado e executado, DHT22/MPU6050 lidos e pulso de 75 bpm |
| [wokwi-offline.png](wokwi-offline.png) | Coleta continua, sem MQTT, ate formar fila de tres leituras |
| [wokwi-sincronizado.png](wokwi-sincronizado.png) | Reconexao e envio FIFO das sequencias 3, 4 e 5; fila zerada |
| [wokwi-alerta.png](wokwi-alerta.png) | `pulso 130` ativa taquicardia no proprio ESP32 |
| [dashboard-esp32.png](dashboard-esp32.png) | Node-RED recebe temperatura, pulso e status do ESP32 |
| [dashboard-esp32-alerta.png](dashboard-esp32-alerta.png) | Alerta de taquicardia recebido via HiveMQ do ESP32 |

Trecho observado no Serial da versao final, boot 44014:

```text
[LEITURA] seq=3 temp=36.5C umid=50% bpm=75 mov=1.00g alertas=0x00 | OFFLINE | fila=1/4320
[LEITURA] seq=4 temp=36.5C umid=50% bpm=75 mov=1.00g alertas=0x00 | OFFLINE | fila=2/4320
[LEITURA] seq=5 temp=36.5C umid=50% bpm=75 mov=1.00g alertas=0x00 | OFFLINE | fila=3/4320
[WIFI] CONECTADO via comando serial | fila: 3 leituras
[SYNC] concluida: 3 leituras enviadas, fila local apagada
[ALERTA] taquicardia ATIVADO (seq 8)
```

A mensagem de conclusao acima teve acentuacao normalizada apenas na transcricao; os prints preservam a saida original. O offline e simulado por booleano e desconexao MQTT, sem derrubar o enlace Wi-Fi do gateway Wokwi.

## Cenarios auxiliares via Node.js

Os arquivos [dashboard-normal.png](dashboard-normal.png), [dashboard-alerta.png](dashboard-alerta.png) e [dashboard-offline.png](dashboard-offline.png) foram capturados no painel real com mensagens publicadas por `tests/test_mqtt_integration.mjs`. Nos cenarios normal/alerta, os sinais sao inteiramente sintetizados por Node.js, **nao pelo ESP32**. No cenario offline, o gerador publica apenas o status e a fila de 12 registros; o grafico, gauge e alerta conservam a ultima telemetria recebida, que nesta captura veio da demonstracao anterior do ESP32. O gerador marca a origem como `teste-integracao` e publica sem retain. Nao execute esse gerador junto da demonstracao do ESP32.

## Testes automatizados

- [testes-cpp.txt](testes-cpp.txt): 64 verificacoes C++17, zero falhas, compiladas com `-Wall -Wextra -Werror`.
- `npm test`: quatro testes de roteamento do dashboard aprovados.
- `npm run test:mqtt -- normal`: ida e volta pelo broker publico validada; exige internet.
- `npm run validate:delivery`: referencias do fluxo, links, sensores, PNGs e PDFs verificados.
- [Contagem dos PDFs](../docs/validacao-relatorios.json): Edge com 2 paginas; MQTT com 3 paginas na geracao final. O JSON registra a contagem obtida pelo parser PDF.

SPIFFS fisico nao foi ensaiado e permanece opcional. Publicacao QoS 0 nao equivale a confirmacao de recebimento da aplicacao; os prints do painel demonstram o recebimento durante este ensaio.