# F5Low — auditoria do debug com frame generation e colour encoding

Data: 20/10/2026. Versão modificada x64 instalada. Escopo: transporte do diagnóstico nativo do OptiScaler e troca de domínio de cor no histórico do NR.

## Defeito confirmado e correção

Reproduzi no PCSX2 o F5Low gerando vetores e imagem de diagnóstico enquanto o menu permanecia em “Waiting”. O produtor recebia um proxy D3D12 do ReShade; o apresentador de FG expunha o dispositivo original. A comparação antiga de ponteiros recusava esse par. Uma execução controlada sem esse proxy não havia reproduzido o defeito.

`native/NativeDebugDevice.h` compara as identidades COM `IUnknown` e, para o par proxy/original, os dispositivos de alocação obtidos dos fences de submissão do produtor e de conclusão do menu. Um dispositivo de alocação realmente diferente continua sendo recusado. Estar na mesma placa de vídeo não é suficiente.

A correção cobre o lease nativo DX12 e os leases DX11/Vulkan apresentados por D3D12. O menu entrega seu fence de conclusão a esses leases. As esperas da GPU, os fences de leitura, as referências às texturas e a proteção de descritores/alocadores continuam ativos. A correção não acrescenta cópia de textura ou passe na GPU.

O DLL final não contém instrumentação privada, abertura forçada do menu, página forçada ou modo de debug forçado. O usuário confirmou setas e normais/depth com XeFG, na prévia e em tela cheia; a imagem foi preservada em `user-confirmed-normals-depth-xefg.png`.

## Colour encoding

Passaram tanto o teste do shader fonte em WARP quanto um teste separado do **bytecode AOT distribuído no DLL**, executado na RTX. Nos casos de ida e volta em 8/10 bits, gamma 2.2/PQ apresentaram erro de zero níveis de código. O controle propositalmente incorreto, guardando PQ em 10 bits, apresentou erro de 329 níveis e comprovou a sensibilidade do teste. O resultado do modelo é simulado por seu próprio input nesses testes; isso verifica a conversão sem exigir edições neurais idênticas para interpretações diferentes da entrada.

sRGB e gamma 2.2 são curvas diferentes, especialmente nos tons escuros. Trocar a interpretação da entrada altera o que o NR recebe e pode alterar sua edição. Uma diferença persistente na aparência, por si só, não comprova erro na conversão inversa.

Havia outro defeito: trocar o encoding resolvido não invalidava o histórico temporal do NR. `DlssNrColourEncoding::HistoryState` agora acompanha encoding efetivo, conversão do shader e domínio linear/HDR. Uma mudança de domínio reinicia os históricos do modelo/reuso e o detector de corte de cena. Um domínio efetivo igual, inclusive Auto para sRGB forçado, não provoca reinício adicional. A correção atua no dispatch comum das rotas nativa, Feeder e Host64.

Para a saída Vulkan SDR testada no PCSX2, use **Auto**, que resolve para tone-mapped sRGB. O swapchain real anuncia `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, em B8G8R8A8_UNORM. Essa indicação segue o domínio de apresentação declarado; não é uma medição da curva interna do Silent Hill 3 no PS2. Referência Vulkan: https://docs.vulkan.org/refpages/latest/refpages/source/VkColorSpaceKHR.html

## Verificações e limites

| Verificação | Resultado / escopo |
|---|---|
| Identidade do dispositivo | 13 casos passaram: identidade direta, proxies em um e nos dois lados, dispositivo distinto, nulos e referências COM. |
| Encoding / histórico | Passou, incluindo mudanças e domínio efetivo inalterado. |
| Shader de cor fonte / AOT do DLL | Ambos passaram; fonte em WARP e AOT na RTX. Logs `colour-roundtrip.log` e `colour-aot-roundtrip.log`. |
| Runtime nativo DX11 | Sem FG, XeFG desligado, XeFG ligado e FSR FG ligado executaram até encerramento normal com flow válido. 600 quadros submetidos pela aplicação por caso; a contagem da bridge pode diferir. |
| Runtime nativo Vulkan | Sem FG, XeFG desligado, XeFG ligado e FSR FG ligado executaram até encerramento normal com flow válido. 400 quadros da aplicação por caso. |
| PCSX2 real / XeFG | DLL final, modelo NR ativo e preset do ReShade sem técnicas ligadas. Capturei setas em tela cheia. O usuário confirmou normais/depth na prévia e em tela cheia. |
| Shader DX11 / Feeder | 55 quadros controlados passaram: XY exato do Feeder, depth pontual, exclusão do céu, geometria pequena, fallback sem depth, trocas de validação e sete debugs finitos. |
| Cliente DX11 x86 / Host64 | Os mesmos controles de 55 quadros passaram pelo cliente/host real com o DLL x64 atualizado. |
| Shader Vulkan / Feeder | Os mesmos controles de 55 quadros passaram. |
| Referência DisplayDepth | Comparação RGB independente com o DisplayDepth original passou nas três rotas: 617.283 canais por rota e nenhum canal acima de 1/255 de erro. |
| Tentativa de DLSSG | O Streamline recusou a feature DLSSG nesta RTX 3080; houve fallback para swapchain comum. Encerramento normal do processo **não** equivale a DLSSG validado. |

Os runtimes controlados exercitam a entrada OptiFG pelo upscaler. Entradas de FG fornecidas por jogos DLSS/FSR reais, geração DLSSG/MFG e qualidade visual dos quadros interpolados não são certificadas por esses testes. Os logs sintéticos de FG ainda contêm avisos de IDs de quadros repetidos/saltados; a correção de identidade dos recursos do menu não resolve esses avisos. Os controles de Host64 verificam entrega e consumo dos inputs do shader, sem certificar exibição de quadros interpolados do Host na janela de um jogo de 32 bits.

## Custo sintético total dos vetores

RTX 3080, 3413×1920. Commit original intacto: `aeb439482c13dd7364ee2bef16fa41b902be6f98`. Aquecimento separado da GPU, quatro execuções alternadas por variante/cena, 64 quadros de aquecimento e 160 medidos em cada execução. A tabela agrega as medianas das execuções. Jogos e Host ficaram fechados; telemetria e hashes dos executáveis constam dos arquivos JSON/CSV.

| Escopo / cena | Original (ms) | Atual (ms) | Diferença (ms) | Diferença (%) |
|---|---:|---:|---:|---:|
| Núcleo / parada | 3.500032 | 2.258944 | -1.241088 | -35.459% |
| Núcleo / movimento | 3.504128 | 3.048960 | -0.455168 | -12.989% |
| Núcleo + alinhamento de depth / parada | 3.498496 | 2.293760 | -1.204736 | -34.436% |
| Núcleo + alinhamento de depth / movimento | 3.502080 | 3.088384 | -0.413696 | -11.813% |

O intervalo inclui luma/pirâmide, matching, refinamento Lucas–Kanade, mediana/filtragem/câmera quando presentes, máscara de confiança e reconstrução dos guias. O segundo par inclui também o alinhamento atual de depth bruto a cada quadro, a partir de 2048×2048, escala Y 1,143 e offsets −4/+117; o original recebe a mesma entrada já alinhada fora do intervalo. Inferência NR, renderização do debug, leitura de volta pela CPU e transporte de API/Feeder/Host ficam excluídos.

São resultados acumulados da versão atual contra a original intacta. Esta correção muda a identificação do dispositivo do menu e a invalidação do histórico do NR; os 15 arquivos de implementação dos vetores comparados coincidem com a candidata anterior. Ela não acrescenta decisão de movimento, confiança, peso, janela de matching ou coordenada de amostragem LK por pixel. As medições não representam FPS do jogo nem a latência completa da rota shader/Host.

## Instalação

O mesmo DLL x64 final foi instalado em `F:/pcsx2-v2.3.25_nightly/winmm.dll` e `F:/Games/Metal Gear Rising - Revengeance/host64/winmm.dll`. Binários dos shaders/Feeder e presets foram preservados. As opções temporárias do teste no PCSX2 foram restauradas aos bytes exatos do OptiScaler.ini anterior. A seleção automática e a calibração de profundidade do usuário continuam disponíveis.

SHA-256 do DLL: `ace6dd2b48ae9efe720d03e734ca440bec74b218ad3aaccd62ab3d8ad416b7cd`.

Evidências: `installation-verification.json`, `device-colour-regression.log`, `fg-matrix-summary.json`, `dx11-fg-matrix-summary.json`, `depth-controls-*`, `user-confirmation.json`, `synthetic-total-summary.json`, `calibrated-total-summary.json` e execuções/telemetria correspondentes. Não foi criado PR nem pacote.
