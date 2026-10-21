# Auditoria da integração F5Low e da prévia de profundidade — 20/10/2026

## Correção mais recente: profundidade branca e normais/profundidade

Visualise() configurava corretamente o multiplicador e o plano distante da visualização, mas Pass() sobrescrevia ambos com constantes do matching. Isso deixava brancos os valores pequenos de profundidade reversa do PS2 durante a linearização. Agora as constantes da visualização são preservadas; os passes do estimador continuam recebendo suas constantes anteriores.

O debug Depth passou a **Normals / depth**: normais reconstruídas à esquerda e profundidade à direita, mantendo as coordenadas da cena. Uma linha separa os lados. Ausência de profundidade aparece como um xadrez magenta. São normais reconstruídas pela profundidade com projeção aproximada, não normais fornecidas pelo jogo. A reconstrução escolhe a menor diferença de profundidade para reduzir cruzamento de superfícies. Os sete debugs públicos continuam disponíveis.

A correção foi aplicada ao OptiScaler nativo e ao shader ReShade gerado do mesmo código. Linearização, multiplicador e plano distante da prévia só alteram o diagnóstico. NR e estimador recebem profundidade bruta alinhada. O núcleo do estimador FX permaneceu com SHA256 324ee1e706413e12a2eacb44126ba4c3b8ae52427bc6162bf35d9f61bcfea0a0.

### Evidências

- Teste GPU do caminho real Visualise → Pass → AOT: a reprodução do erro anterior falhou em 4029/4029 pontos de profundidade; a versão corrigida falhou em 0/4029, com tolerância RGBA8 de 1,51/255. Foram produzidos 3876 pixels de normais coloridos. Controles de profundidade bruta/linear e xadrez de ausência passaram.
- Vetores e profundidade do modelo permaneceram idênticos byte a byte antes/depois do debug.
- Compilação nova dos 46 kernels AOT e da DLL Release concluída.
- Testes de 55 quadros passaram em DX11, Vulkan e cliente x86/Host64: orientação/tamanho da profundidade, controle do céu, ausência de profundidade, transições da validação, igualdade XY com Feeder e sete visualizações válidas.
- A nova prévia foi validada em GPU sintética; a aparência na cena real de Silent Hill 3 ainda precisa de confirmação. As capturas reais anteriores de profundidade/NR precedem esta última correção de visualização.

## Outras correções concluídas nesta rodada

1. **NR com Feeder:** a identificação completa do produtor externo (versão do contrato, ID, modo Host e cadência) permite aplicar NR na avaliação da imagem pronta do Feeder. Antes esperava uma apresentação que não ocorre no dispositivo privado D3D12. Identificações inválidas/ausentes mantêm o caminho nativo anterior. Onze casos de identificação e testes reais com modelo ligado/desligado passaram em DX11, Vulkan e Host64; desligado, a saída permaneceu idêntica.
2. **Seleção de profundidade quadrada:** a heurística de proporção do ReShade rejeitava os buffers quadrados do PCSX2 após reiniciar. Foi desativada nessa instalação, preservando o alinhamento manual. O OptiScaler recebeu seleção por dimensões e alinhamento por escala/deslocamento com amostragem pontual, compartilhados entre adaptadores DX11/DX12/Vulkan. A seleção explícita só ignora o filtro de proporção, preservando verificações de validade. Alinhamento identidade não adiciona passe GPU.
3. **Identidade da profundidade:** o provider 5 do Feeder usa a mesma conta de centro de pixel do provider de vetores. Capturas reais do PCSX2 confirmam profundidades brutas idênticas byte a byte, XY exato e disponibilidade da profundidade. Capturas do caminho nativo confirmam seleção 2048×2048 e uso contínuo de profundidade/NR.
4. **Instrumentação do Feeder:** leituras/hashes diagnósticos de quadros antigos deixaram de executar durante uso normal; continuam disponíveis no modo de rastreamento existente. Não foi demonstrado ganho GPU adicional consistente nessa alteração.

## Custo sintético total dos vetores contra o original

Original sem modificações: aeb439482c13dd7364ee2bef16fa41b902be6f98. RTX 3080, 3413×1920, mesmas entradas/configurações, 64 quadros de aquecimento + 160 medidos, alternância original/atual duas vezes por cenário. Valores agregam medianas por execução. Sem jogos ou outros testes GPU simultâneos.

Inclui luminância/pirâmide, Match/Lucas–Kanade, mediana, filtragem/câmera/global, confiança e reconstrução completa do guia. Exclui inferência NR, debug, leitura CPU e transporte shader/Host. São tempos sintéticos D3D12 dos vetores, não FPS de jogo nem custo completo Vulkan/Host.

| Cena | Original | Atual | Diferença |
|---|---:|---:|---:|
| Parada | 3.497 ms | 2.256 ms | -1.241 ms (-35.49%) |
| Movimento | 3.500 ms | 3.041 ms | -0.459 ms (-13.11%) |

### Incluindo o novo alinhamento de profundidade por quadro

Ambos recebem a mesma profundidade alinhada. No original a cópia é pré-calculada fora da medição; a versão atual mapeia o buffer 2048×2048 para a imagem dentro da medição, com escala Y 1,143 e deslocamentos −4,+117.

| Cena | Original | Atual | Diferença |
|---|---:|---:|---:|
| Parada | 3.497 ms | 2.303 ms | -1.194 ms (-34.14%) |
| Movimento | 3.500 ms | 3.075 ms | -0.425 ms (-12.14%) |

Os ganhos acumulados do estimador antecedem esta correção de visualização, que não mudou a matemática dos vetores. O teste separado de transporte Vulkan/Feeder não demonstrou ganho GPU adicional. O aumento de FPS ao abrir o menu do OptiScaler no PCSX2 continua sem solução comprovada.

## Instalação e limites

DLL x64 atualizada em PCSX2/winmm.dll e MGR/host64/winmm.dll. Provider, núcleo compartilhado, debug e shader Feeder atualizados nos dois jogos e nas árvores de código modificado. Hashes e arquiteturas PE verificados. INIs/presets preservados nesta instalação final. Add-ons temporários de captura e solicitações pendentes removidos. Sem alterações no Requiem, PR ou pacote.

A calibração do PCSX2 é específica deste Silent Hill 3: 2048 quadrado, escala Y 1,143, deslocamentos −4,+117, profundidade reversa, multiplicador de exibição 4500 e plano distante 1000. Outros jogos podem exigir valores diferentes. O corte de céu identifica profundidade distante, não conteúdo semântico; céu/fumaça que compartilham profundidade não são separáveis com segurança só por ela. Não foi demonstrada igualdade bit a bit do estimador entre todas as APIs; funcionamento das rotas e transferência do Feeder têm testes próprios. Limitações anteriores de qualidade continuam registradas na auditoria precedente.

Evidências: depth-normal-debug/production-visualizer-regression.json, route-controls.json, estimator-core-unchanged.json, installation-verification.json, synthetic-total-summary.json, calibrated-total-summary.json, native-depth-debug-gpu.log. Na pasta superior: selected-routes.json, host64-finished-picture/result.json, pcsx2-final-shader-result.json, pcsx2-native-result.json, calibration-aot-check.json e depth-regressions.log.
