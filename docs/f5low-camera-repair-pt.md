# Correção do movimento de câmera do F5Low — auditoria

## Alterações

- Mantém a arquitetura de seis níveis Match → Lucas–Kanade → Median → Smooth → Trust/Uncover/Guide. Acrescenta um único cálculo de câmera com 128 threads entre Median e Smooth, inteiramente na GPU, sem leitura pela CPU ou média temporal do vetor final.
- Ajusta o modelo a partir dos vetores finos do quadro atual, com amostragem espacial fixa, pesos limitados de estrutura ao quadrado, preferência por profundidade distante válida, mediana inicial e três iterações robustas.
- Usa um modelo afim regularizado. Quando as observações se concentram numa faixa estreita, usa ajuste conjunto de rotação, zoom e translação. Verifica cobertura ampla, concordância, erro residual e valores finitos. A regularização foi ajustada para preservar a rotação em cenas dominadas pelo céu.
- Aplica **fusão geométrica seletiva** onde a evidência local é fraca. Cada pixel valida sua própria janela original de 16 amostras e seus próprios pesos de profundidade/luminância, com deslocamento independente no quadro anterior. Protege silhuetas próximas e movimento local quase nulo confirmado por amostras exatamente estáticas.
- A confiança continua **0 = baixa, 1 = alta**. `.z` e `F5Low::Confidence` continuam medindo estrutura de textura em dois eixos. O debug laranja/verde também considera concordância com o modelo validado e limita o suporte pelo ajuste fotométrico e histórico aceito. O RGB mantém os sinais separados. A máscara distinta de rejeição usa **1 = rejeitado**.
- Mantém sete debugs com legenda: setas, profundidade, fluxo final, confiança laranja/verde, confiança RGB, rejeição de histórico e still age. Escolher o debug não muda os vetores. Trace ligado/desligado preservou exatamente 58,565,808 bytes.

## Otimização sem alterar os dados

Compartilha somente amostras idênticas das imagens: luminância atual, profundidade bruta e quadro anterior sem deslocamento. Usa bloco 14×14 no LK normal e conserva 16×16 no inverso. Cada pixel conserva janela, pesos, candidatos, resíduos, confiança e coordenadas móveis do LK. As propostas de cache em Guide/Median foram descartadas pelas medições. As comparações normal e inversa preservaram, cada uma, **274.159.339 bytes sem diferença**, incluindo modelo, níveis de matching, trace e guia final. A fusão geométrica é uma alteração de qualidade validada separadamente; a preservação exata se refere ao cache de amostras.

## Evidência na GPU com entradas iguais

Erro em pixels da resolução final, medido no guia consumido.

| Cena | Erro da base instalada | Erro novo | Redução |
|---|---:|---:|---:|
| pan-horizon-no-depth | 0.329526 | 0.031613 | 90.4% |
| creep-horizon | 0.171029 | 0.032964 | 80.7% |
| static-horizon-grain | 0.904223 | 0.080578 | 91.1% |
| fast-horizon | 0.529381 | 0.088109 | 83.4% |
| texturizada | 0.064298 | 0.064249 | 0.1% |

Com 80% de céu e câmera em rotação, o erro cai de **0,7970 para 0,2144 px**; as direções dentro de 30° passam de **0,12% para 99,9996%**. Com 40% de céu, os erros de rotação/zoom/combinação caem de 0,6874/0,5781/0,5376 para 0,0749/0,0583/0,0533 px. A borda do objeto grande com movimento independente e profundidade melhora de 0,3391 para 0,2734 px. O erro do HUD parado nos testes de rotação/zoom fica abaixo de 0,0001 px. Partida, parada e inversão são verificadas sem acrescentar média temporal ao fluxo final.

Duas verificações internas de HDR continuam indicando FAIL ao comparar luma perceptual com a luma antiga recém-melhorada. O log original foi preservado. As demais verificações antigas passam. Comparando o mesmo modo, o erro com ruído melhora de 0,374 para 0,295 px em scRGB e de 0,370 para 0,284 px em PQ. Nos pans escuros sem ruído, o erro médio aumenta aproximadamente 0,012 px; todas as amostras avaliadas continuam dentro de 0,5 px.

## Integração e custo

- Os sete debugs passam na comparação nativo/ReShade: 231 comparações por modo. Match, Median, modelo, Smooth, guia final e XY entregue pelo Feeder são idênticos; aceita-se um nível de quantização na máscara R8 e arredondamento na prévia. Profundidade normal e invertida foram verificadas.
- A conferência dos 46 shaders nativos passa. Os 41 pontos de entrada do shader compilam em DXBC e SPIR-V, em 641×361 e 1920×1080.
- Avaliação/composição real do NR funciona com ApplyModel ligado/desligado em DX11 nativo, Vulkan nativo e Feeder Vulkan x64. A rota D3D11 x86 → Host64 é coberta pela comparação. O provider 5 conserva os XY recebidos; verificações opcionais afetam a rejeição de histórico. O ABI e o binário do add-on Feeder permanecem compatíveis.
- Medição de GPU em 3412×1920, debug desligado, duas rodadas intercaladas, com máscaras/guias e sem inferência NR:

| Cena | Medianas das rodadas da base (ms) | Medianas finais (ms) |
|---|---:|---:|
| texturizada | 3.0597–3.0618 | 2.9819–3.0044 |
| textura fraca | 2.8129–2.9440 | 2.8488–2.8488 |

A cena texturizada melhora cerca de 2%; o custo com textura fraca fica dentro da variação das rodadas. O ajuste de câmera custa aproximadamente 0,070 ms. Estas medições não determinam ganho de FPS no jogo contra o Lumenite.

## Escopo

O fonte modificado está no projeto local do OptiScaler e no ReShadeMotionVectors. O núcleo FX é gerado mecanicamente a partir dos kernels nativos. Entradas e parâmetros iguais são necessários para igualdade. A rota de vetores nativos de jogos com DLSS não foi alterada nem exercitada numa nova sessão de jogo nesta rodada. Falta sua confirmação visual nas cenas reais do MGR e PCSX2. Movimento invisível ou completamente ambíguo não tem solução única a partir de imagens. Pacote e PR aguardam essa confirmação.

## Configuração instalada e identificação

O MGR estava com experimentos internos, filtro de evidência e guarda do refinamento ligados, além de ZeroMargin=0,01 e StillFrames=6. A atualização restaura o perfil validado de LK normal, desliga esses experimentos, usa ZeroMargin=0 e StillFrames=8 e preserva a aparência das setas. Movimento/entrada nativos ficam ligados no PCSX2 e desligados no host do MGR para cada instalação usar um único produtor. A avaliação/composição NR real x86→Host64 também passou com ApplyModel ligado e desligado.

SHA-256 do DLL x64 compilado: `5c6d02d0a4885f9f60ca0400a520fd355af4a0b40c37e71d11ba8ef54103162a`.
