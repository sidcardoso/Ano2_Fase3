# Fase 3 - Monitoramento IoT

Protótipo acadêmico de monitoramento contínuo com ESP32, DHT22, MPU6050, botão de pulso, processamento local, fila offline, MQTT e painel Node-RED. Leituras e limites são demonstrativos; o sistema não se destina a diagnóstico, triagem ou decisão clínica.

## Links para entrega

- [Projeto público no Wokwi](https://wokwi.com/projects/479795189911514113).
- [Repositório da fase 3](https://github.com/sidcardoso/Ano2_Fase3).
- [Parte 1: relatório Edge, 2 páginas](docs/relatorio-edge.pdf).
- [Parte 2: relatório MQTT/dashboard, 3 páginas](docs/relatorio-mqtt.pdf).
- [Export do dashboard Node-RED](node-red/flow.json).
- [Evidências e resultados de validação](evidencias/README.md).

## Arquivos

- `firmware/sketch.ino`: aplicação Arduino para ESP32 e Wokwi.
- `firmware/edge_core.h`: fila, BPM, alertas e serialização, independentes do hardware.
- `firmware/diagram.json`: circuito Wokwi com sensores, botão, chave, LEDs e buzzer.
- `firmware/libraries.txt`: bibliotecas usadas no simulador.
- `node-red/flow.json`: export importável do dashboard Node-RED.
- `relatorio.md`: relatório da lógica Edge, MQTT, visualização, segurança e limitações.

## Circuito e sinais

| Componente | GPIO / barramento | Uso |
| --- | --- | --- |
| DHT22 | GPIO 15 | Temperatura e umidade, amostradas a cada 5 s |
| Botão de pulso | GPIO 4 | Cada pressão representa um batimento; debounce de 20 ms |
| Chave de conectividade | GPIO 27 | Simula online/offline; comandos Serial também estão disponíveis |
| MPU6050 | SDA 21, SCL 22 | Movimento a 10 Hz e detecção demonstrativa de impacto |
| LED online | GPIO 26 | Wi-Fi simulado e MQTT conectados |
| LED de alerta | GPIO 25 | Alerta local; crítico fixo, atenção intermitente |
| LED de pulso | GPIO 33 | Indica batimento aceito |
| Buzzer | GPIO 32 | Alerta crítico local |

## Executar no Wokwi

Abra o projeto público acima, inicie a simulação e aguarde a conexão MQTT. O circuito usa ESP32 DevKit C v4. No Monitor Serial (115200 baud), envie `pulso 75` para simular batimentos regulares ou pressione o botão verde. A chave em GPIO 27 simula conectividade; os comandos `offline`, `online`, `auto`, `status`, `limpar` e `pulso 0` também estão disponíveis.

Para recriar o projeto, adicione os quatro arquivos de `firmware/` (`sketch.ino`, `edge_core.h`, `diagram.json` e `libraries.txt`) a um projeto ESP32 novo. O projeto foi salvo publicamente pelo próprio Wokwi sem login; uma conta é recomendada para manter a propriedade e editar o projeto futuramente.

## Dashboard Node-RED

Com Node.js 20 ou superior, na pasta que contém este README:

```powershell
npm ci
npm start
```

Abra `http://127.0.0.1:1880/ui`. O fluxo já é carregado automaticamente; o editor fica em `http://127.0.0.1:1880`. Se a porta estiver ocupada, defina `$env:PORT=1881` antes de iniciar. Inicie o ESP32 no Wokwi com a chave online: ambos usam `broker.hivemq.com:1883` e os tópicos `fiap/cardioia/PAC-001/`.

Para usar outra instalação Node-RED, instale `node-red-dashboard@3.6.5`, importe `node-red/flow.json` e faça Deploy. O dashboard clássico é legado e foi mantido por compatibilidade; o runtime é restrito a localhost. Não exponha o editor na internet.

O painel mostra frequência cardíaca em gráfico, temperatura em gauge, texto de alerta e estado/fila do dispositivo. O broker é público e sem TLS neste protótipo; não transmita dados reais ou identificáveis. Há capturas do painel e do ESP32 em `evidencias/`, com a origem de cada cenário explicitada.

## Testes e validação

Na pasta que contém este README:

```powershell
npm test
npm run validate:delivery
npm run test:mqtt -- normal
```

Os testes C++ da lógica de Edge podem ser executados com um compilador g++:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror .\tests\test_edge_core.cpp -o $env:TEMP\test_edge_core.exe
& $env:TEMP\test_edge_core.exe
```

Resultado verificado: 64 verificações C++ sem falhas e quatro testes Node.js aprovados. A compilação ESP32 foi feita pelo Wokwi; a comunicação com HiveMQ e o recebimento no Node-RED foram exercitados ao vivo. O teste `test:mqtt` publica dados sintéticos de origem Node.js e não deve ser confundido com o firmware. Não execute esse gerador durante a demonstração do ESP32, pois ambos usam o mesmo tópico didático. O broker público pode sofrer interferência de outros usuários.

Os PDFs podem ser regenerados com `npm run build:report` (Chrome/Edge instalado ou `CHROME_PATH` configurado); a contagem real de páginas fica em `docs/validacao-relatorios.json`.

O armazenamento em RAM tem capacidade para 4.320 amostras de 16 bytes (aproximadamente 6 horas a cada 5 s). Se `USE_SPIFFS` for ativado em hardware real, o arquivo é compactado de forma amortizada e fica limitado a 4.576 registros. No Wokwi, a fila RAM não sobrevive ao encerramento/reinício da simulação; a persistência SPIFFS é opcional e não é considerada no critério do simulador.
