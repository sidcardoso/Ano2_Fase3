# CardioIA - Fase 3: Monitoramento Contínuo com IoT

**Relatório técnico de implementação**

**Escopo:** processamento Edge, sincronização MQTT e visualização Node-RED

**Natureza dos dados:** exclusivamente simulados; protótipo acadêmico sem finalidade clínica

## 1. Objetivo e arquitetura

A fase implementa um protótipo vestível simulado para acompanhar sinais vitais e movimento. O fluxo é composto por sensores conectados a um ESP32, lógica local de aquisição e alerta, uma fila temporária de leituras, transporte MQTT e um painel Node-RED. A separação da lógica em `edge_core.h` permite testar fila, estimativa de pulso, avaliação de limites e serialização em computador sem depender das bibliotecas físicas do microcontrolador.

O DHT22 fornece temperatura e umidade. Um botão representa cada batimento, permitindo demonstrar a estimativa de frequência cardíaca sem sensor biomédico. O MPU6050 mede aceleração e permite classificar repouso, atividade e um pico de impacto. Assim, o projeto excede o mínimo de dois sensores distintos: DHT22 e MPU6050, além da entrada digital do botão. Nenhum desses sinais simulados equivale a uma medição clínica validada.

A aplicação captura uma amostra consolidada a cada cinco segundos. O MPU6050 é consultado a cada 100 ms para aumentar a chance de observar picos de movimento entre amostras. Aquisição, leitura serial, conectividade, sincronização e atualização de indicadores são tarefas temporizadas dentro do `loop()`, sem `delay()` explícito. As chamadas de conexão da biblioteca MQTT são síncronas e podem atrasar temporariamente o loop em caso de falha de rede; o protótipo não é um sistema de tempo real certificado.

## 2. Parte 1: sensores e processamento local

O DHT22 é ligado ao GPIO 15 e lido em intervalo compatível com o sensor. Temperatura é armazenada em décimos de grau e umidade em porcentagem inteira para reduzir o tamanho de cada registro. Se a leitura falhar ou retornar valor inválido, os bits de estado indicam falha e a serialização MQTT representa temperatura e umidade como `null`; uma leitura defeituosa não é tratada como uma medida válida.

O botão de pulso está no GPIO 4 com `INPUT_PULLUP`. O pressionamento é ativo em nível baixo e passa por debounce temporal de 20 ms, reduzindo múltiplas contagens causadas por oscilações mecânicas. A cada pressão aceita, o estimador registra o intervalo desde o batimento anterior. A frequência é calculada a partir da média móvel de até cinco intervalos. Intervalos inferiores a 250 ms são rejeitados como repiques ou valores acima da faixa demonstrativa; pausas superiores a três segundos reiniciam a janela. A ausência de batimento por mais de dez segundos é sinalizada como `sem_pulso`. Para facilitar testes no simulador, o comando Serial `pulso N` gera batimentos automáticos na frequência aproximada N, e `pulso 0` desliga essa função.

O MPU6050 usa I2C nos pinos SDA 21 e SCL 22 e é consultado a cada 100 ms. O firmware calcula o módulo da aceleração em unidades de gravidade e conserva o maior desvio em relação a 1 g até a próxima amostra de cinco segundos. O valor é usado para classificar atividade e gerar um alerta demonstrativo de impacto quando alcança 2,5 g. A detecção não é um algoritmo validado de queda: movimentos cotidianos podem gerar falsos positivos e a orientação do dispositivo altera a leitura.

Alertas são avaliados localmente e não dependem de internet. Os limites padrão são temperatura acima de 38 °C ou abaixo de 35 °C, frequência acima de 120 bpm ou abaixo de 50 bpm, ausência de pulso e umidade fora de 30% a 70%. Uma aceleração de impacto também gera alerta. A saída local usa LED de alerta no GPIO 25 e buzzer no GPIO 32; alertas críticos acionam o LED e o buzzer, enquanto atenção não crítica pisca. Os LEDs de conectividade e pulso usam os GPIOs 26 e 33. Os limites são parâmetros didáticos, não recomendações médicas.

## 3. Resiliência offline e capacidade de armazenamento

A chave deslizante no GPIO 27 e os comandos `online`/`offline` permitem simular indisponibilidade de rede. Mesmo offline, o ESP32 continua amostrando sensores, atualizando alertas locais e armazenando registros. O estado e o volume da fila podem ser consultados com `status`; o comando `limpar` descarta explicitamente a fila. A chave física volta a ser a fonte de conectividade após o comando `auto` ou ao movimentar a própria chave.

A indisponibilidade é simulada pelo booleano `wifiConectado`, conforme o enunciado. Em `offline`, a sessão MQTT é encerrada e nenhuma leitura é transmitida, mas o enlace Wi-Fi do simulador permanece associado. Essa separação evita derrubar o gateway público do Wokwi e permite demonstrar a retomada sem reiniciar a simulação. Em `online`, a sessão MQTT é reaberta e a fila é drenada em ordem.

A estratégia de capacidade é uma fila circular de 4.320 amostras. Com uma amostra a cada cinco segundos, ela cobre seis horas (`4.320 × 5 s = 21.600 s`). Cada estrutura compacta ocupa 16 bytes, totalizando 69.120 bytes para o vetor principal. Esse limite representa a hipótese de negócio do protótipo: até seis horas sem rede antes de descartar dados. Quando a fila enche, a implementação remove a amostra mais antiga e mantém as mais recentes; o contador `dropped` torna a perda visível. Essa política privilegia estado recente e não promete retenção ilimitada.

No Wokwi, o protótipo usa a RAM e a fila se perde ao encerrar ou reiniciar a simulação. O enunciado reconhece essa limitação e aceita o Monitor Serial para demonstrar a resiliência. Existe suporte opcional ao SPIFFS em `sketch.ino` (`USE_SPIFFS`): registros são acrescentados em CSV, restaurados no boot e marcados com idade desconhecida. A compactação é amortizada em blocos de 256 registros, limitando o arquivo a 4.576 linhas; essa margem não amplia a autonomia da fila RAM, que continua de seis horas. Ao esvaziar a fila sincronizada, o arquivo é removido. Reinícios durante a sincronização podem reenviar registros já publicados. O par `boot`/`seq` distingue mensagens na sessão atual, mas o identificador original de boot não é persistido no CSV; deduplicação entre reinícios não é garantida. A opção física não foi validada em hardware e requer teste de particionamento, falhas de energia e desgaste de flash antes de uso real.

## 4. Parte 2: MQTT, broker e formato dos dados

O ESP32 conecta-se à rede de simulação `Wokwi-GUEST` e ao broker público `broker.hivemq.com` na porta 1883. A conectividade do protótipo é controlada pela chave/Serial; quando ativada, o firmware associa Wi-Fi, tenta abrir a sessão MQTT e aplica backoff exponencial entre tentativas, de um segundo até 30 segundos. A aquisição e os alertas locais continuam funcionando enquanto a reconexão ocorre.

Os tópicos usam o identificador pseudônimo fixo `PAC-001`:

| Tópico | Conteúdo |
| --- | --- |
| `fiap/cardioia/PAC-001/vitals` | Leituras de temperatura, umidade, BPM, movimento, alertas, sequência, idade, fila e indicador de dados atrasados |
| `fiap/cardioia/PAC-001/alerts` | Mudanças de estado de cada alerta |
| `fiap/cardioia/PAC-001/status` | Estado online/offline, número de boot, fila, descartes e amostras enviadas |

A mensagem de sinais é JSON. Um exemplo reduzido é `{"device":"PAC-001","boot":7,"seq":42,"temp_c":36.5,"hum_pct":50,"bpm":76,"motion_g":1.0,"alerts":[],"buffered":false,"queue":0}`. Leituras recuperadas ou sincronizadas após interrupção carregam `buffered` e idade apropriados para não serem apresentadas como instantâneas. O firmware imprime as mensagens no Monitor Serial com o prefixo `[NUVEM]` para observação e publica no MQTT. A fila remove um registro após `publish()` retornar sucesso; a biblioteca PubSubClient utilizada trabalha com QoS 0, então esse retorno confirma escrita local do pacote, não uma confirmação de aplicação pelo broker.

O tópico de status é retained para que o consumidor receba o último estado ao se inscrever. A sessão também configura Last Will para publicar estado offline se o dispositivo cair sem desconexão graciosa. Eventos de alerta são enviados no tópico de alertas quando mudam; a mensagem de sinais inclui a lista completa de alertas ativos, permitindo que o painel reconstrua o estado atual.

## 5. Dashboard Node-RED

O arquivo `node-red/flow.json` é um export importável. Ele assina `fiap/cardioia/PAC-001/#`, converte o JSON e separa os dados em quatro saídas. O gráfico de linha acompanha BPM até 240; o gauge mostra temperatura e usa divisões visuais em 35 °C e 38 °C; um texto exibe `NORMAL` ou os alertas ativos; outro texto exibe estado do dispositivo, fila, descartes e se os dados são ao vivo ou sincronizados com atraso. O indicador de alerta depende da lista de alertas calculada pelo ESP32, portanto também funciona durante sincronização de leituras offline.

Para reproduzir, execute `npm ci` e `npm start` na pasta da fase 3, com Node.js 20 ou superior. As versões fixadas são Node-RED 4.1.17 e `node-red-dashboard` clássico 3.6.5; o fluxo é carregado automaticamente e o painel fica em `http://127.0.0.1:1880/ui`. Também é possível importar `node-red/flow.json` em outra instalação com o mesmo pacote de dashboard. O servidor está restrito a localhost, e o dashboard legado não deve ser exposto na internet. Inicie o circuito público, use `pulso 75`, envie `offline`, aguarde algumas amostras e retorne com `online`. As capturas reais estão em `evidencias/`, incluindo o alerta de taquicardia recebido do firmware. As capturas auxiliares com febre foram produzidas pelo gerador de teste Node.js e estão explicitamente identificadas.

## 6. Segurança, privacidade e limitações

O broker público e a porta 1883 sem TLS foram escolhidos para facilitar uma demonstração sem credenciais. O tópico pode ser observado por terceiros, o identificador é fixo e não existe autenticação nem ACL. Por isso, o protótipo transmite somente dados sintéticos e pseudônimos e não deve receber nome, documento, diagnóstico ou qualquer informação de saúde real. Um uso real exigiria broker privado, TLS, autenticação por dispositivo, ACL por tópico, rotação de credenciais fora do código, controle de retenção e auditoria. Também seriam necessários relógio confiável, QoS/acknowledgment adequado, deduplicação persistente, validação clínica e avaliação regulatória.

As medidas de eficiência são amostragem periódica, registro compacto, fila limitada, envio em lotes de dez mensagens e backoff de reconexão. As limitações são a ausência de persistência no simulador, a entrega MQTT QoS 0, a dependência de um broker de demonstração e a ausência de testes com sensores reais. Uma falha de broker não interrompe a aquisição nem os alertas locais, mas um desligamento do ESP32 no Wokwi elimina a fila em RAM.

## 7. Validação e critérios de aceite

A lógica independente de hardware possui testes C++ em `tests/test_edge_core.cpp` para ordem e descarte da fila, autonomia, cálculo de BPM, debounce, limites de alerta, serialização JSON e round-trip CSV. O dashboard tem testes Node.js em `tests/test_dashboard_flow.mjs` para assinatura dos tópicos, presença dos widgets, mapeamento de BPM/temperatura, indicação de alertas, dados atrasados e estado da fila. Os comandos de execução estão em `README.md`.

Em 4 de novembro de 2026, os testes C++ foram compilados em C++17 com `-Wall -Wextra -Werror`: 64 verificações, zero falhas. Os quatro testes Node.js passaram. O teste MQTT confirmou ida e volta pelo broker HiveMQ, e o Node-RED exibiu gráfico, gauge, alerta e status. O firmware ESP32 foi compilado e executado no Wokwi público: https://wokwi.com/projects/479795189911514113.

No teste integrado final (boot 44014), `pulso 75` produziu 75 bpm. Em offline, as sequências 3, 4 e 5 foram coletadas a cada cinco segundos, formando fila de três registros. Após `online`, o Serial mostrou a publicação das três sequências, com a fila diminuindo de 2 para 1 e depois 0, e a mensagem `[SYNC] concluída: 3 leituras enviadas, fila local apagada`. O comando `pulso 130` gerou o alerta de taquicardia na sequência 8, recebido pelo painel. O export, os PDFs e os prints estão no repositório de entrega https://github.com/sidcardoso/Ano2_Fase3. O validador `npm run validate:delivery` confere os arquivos, referências do fluxo, assinaturas PNG e número real de páginas dos PDFs. Não houve ensaio em sensores físicos ou SPIFFS real.

## 8. Conclusão

O protótipo demonstra a cadeia de aquisição, processamento na borda, armazenamento temporário, transmissão MQTT, visualização e sinalização de alertas. A fila de seis horas explicita um compromisso de negócio entre memória e retenção, e o modo offline mantém a coleta e os alertas locais. O fluxo Node-RED e os testes tornam a integração reproduzível. O resultado é apropriado para demonstração acadêmica com dados simulados, mas não constitui dispositivo médico nem sistema de monitoramento clínico.
