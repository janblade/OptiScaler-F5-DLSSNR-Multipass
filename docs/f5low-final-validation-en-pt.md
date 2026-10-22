# F5Low — functional motion candidate / candidata de movimento real

## Status

User approved delivery on 2026-10-21. **Installed in PCSX2 and MGR/Host64**, with the measured candidate hashes verified and all 16 existing configuration files preserved. The companion package/installation is separate from this OptiScaler-only proposal. Original pre-installation evidence is preserved separately. This is a test candidate, not a claim of finished temporal stability.

Entrega autorizada em 21/10/2026. **Instalada no PCSX2 e no MGR/Host64**, com hashes da candidata medida conferidos e os 16 arquivos de configuração preservados. O pacote e a instalação são entregas separadas desta proposta somente do OptiScaler. A evidência anterior à instalação foi preservada separadamente. É uma candidata para testes, sem alegação de estabilidade temporal concluída.

## Actual algorithm changes / Mudanças no algoritmo real

- Smooth now detects speed disagreement as well as angular disagreement in a coherent neighbourhood. Each regional proposal is still validated in the receiving pixel's original depth/luminance weighted photometric window.
- Independent movement can escape camera suppression with relative and absolute photometric improvement, spatial coherence and temporal confirmation of the pixel's pre-Smooth measured vector at its own reprojected position. Neighbour agreement alone is insufficient.
- Partial evidence gates are combined with a minimum instead of multiplying them toward zero, followed by a bounded recovery response. This changes the delivered XY vectors. It does not enlarge arrows or recolour confidence.
- Filter by vector evidence remains permanently on in native and shader production paths (flags 7), with no user toggle. It was already forced on in the installed build; this is verified, not presented as a new fix. Native inverse refinement and its functional UI toggle were also already present and verified.

- O Smooth passa a detectar divergência de velocidade além da divergência de direção numa vizinhança coerente. A proposta continua sendo validada na janela fotométrica original do próprio pixel, com seus pesos de profundidade e luminância.
- O movimento independente pode ser preservado contra a supressão da câmera quando há melhora fotométrica absoluta e relativa, coerência espacial e confirmação temporal do vetor medido desse pixel antes do Smooth, na sua própria reprojeção. Concordância entre vizinhos não basta.
- Os critérios parciais usam o mínimo em vez de serem multiplicados até quase zero, com uma recuperação limitada. Isso altera os vetores XY entregues. Não aumenta as setas nem muda as cores de confiança.
- O filtro por evidência continua obrigatório nas duas implementações (flags 7), sem toggle. Já estava obrigatório na instalação anterior. O refinamento inverso nativo e seu toggle funcional também já existiam e foram confirmados.

The shader is generated mechanically from the native source. Known-motion accuracy is measured in the native pipeline; actual DX11/Vulkan shader runs separately verify finite output, depth behaviour and exact Feeder XY delivery. These are different checks, not a claim of bit-identical native/API flow fields.

O shader é gerado mecanicamente do código nativo. O erro contra movimento conhecido é medido no pipeline nativo; execuções reais do shader em DX11/Vulkan verificam separadamente valores finitos, profundidade e entrega exata dos canais XY ao Feeder. Isso não afirma identidade bit a bit entre o campo nativo e o de cada API.

An API-selection bug was found in the generated shader: exact comparison with 0x20000 missed versioned Vulkan renderer identifiers. Both Vulkan guards now test the renderer family with `(__RENDERER__ & 0xF0000) == 0x20000`. This activates the intended workgroup-pool reuse and explicit native-compatible nearest-even rounding. The compiled SPIR-V was inspected to verify removal of the unwanted 800-byte workgroup object. Vulkan sample-sharing comparisons use a reference with the same corrected rounding and original per-pixel Trust source reads, separating the correctness fix from the optimization. In the normal-refinement capture on this RTX 3080, the corrected-rounding reference also matched the previous capture; no image-quality gain is attributed to that guard change alone.

Foi encontrado um erro na seleção de API do shader gerado: a igualdade com 0x20000 não reconhecia identificadores Vulkan com versão. As duas condições agora verificam a família com `(__RENDERER__ & 0xF0000) == 0x20000`. Isso ativa a reutilização pretendida do pool e o arredondamento explícito para o par mais próximo, compatível com o nativo. O SPIR-V compilado foi inspecionado para confirmar a remoção do objeto compartilhado extra de 800 bytes. A comparação da reutilização de amostras em Vulkan usa uma referência com o mesmo arredondamento corrigido e as leituras originais por pixel do Trust, separando correção e otimização. Na captura com refinamento normal desta RTX 3080, essa referência também coincidiu com a captura anterior; não atribuí ganho de qualidade de imagem apenas à mudança dessa condição.

## Single optimization / Única otimização

Trust stages identical fixed-coordinate LumaNow and DepthBefore source samples in an 8×8 group's 10×10 halo (800 bytes). Motion-dependent positions, residuals, candidates, confidence and decisions remain private. Matching/LK windows and sampling coordinates are unchanged. All lanes reach the barrier before padded pixels exit.

O Trust reutiliza amostras idênticas de LumaNow e DepthBefore em coordenadas fixas, num bloco 10×10 para um grupo 8×8 (800 bytes). Coordenadas dependentes de movimento, resíduos, candidatos, confiança e decisões continuam privados. As janelas e coordenadas de amostragem do matching/LK permanecem iguais. Todas as threads chegam à barreira antes da saída dos pixels de preenchimento.

DX11 uses a dedicated 200-float scratch allocation. Its first trial reused the general 1,152-float scratch and therefore reserved 4,608 bytes; compiled DX11 assembly confirmed both that waste and the corrected 800-byte allocation. A universal dedicated allocation was rejected: warm Vulkan tests regressed to 5.459 ms for the estimator versus 4.688 ms for the installed version. SPIR-V therefore reuses the existing 1,152-float workgroup pool, addressing the same 200 immutable sample slots, while DX11 uses the dedicated allocation. This changes storage only. All captured non-COLOR buffers remain bit-identical in all six API/refinement controls, including confidence and final Feeder flow.

DX11 usa uma alocação dedicada de 200 floats. O primeiro ensaio reutilizava o scratch geral de 1.152 floats e reservava 4.608 bytes; o assembly DX11 confirmou tanto esse desperdício quanto a alocação corrigida de 800 bytes. A alocação dedicada universal foi rejeitada: Vulkan aquecido piorou para 5,459 ms no estimador contra 4,688 ms da versão instalada. Portanto, SPIR-V reutiliza o pool existente de 1.152 floats, nos mesmos 200 slots de amostras imutáveis, enquanto DX11 usa a alocação dedicada. Muda apenas o armazenamento. Todos os buffers capturados, exceto COLOR, continuam idênticos bit a bit nos seis controles de API/refinamento, incluindo confiança e vetores finais do Feeder.

Isolated optimization checks used the same quality revision on both sides: zero differences in all four native size/refinement combinations and in 936 DX11, 1,379 Vulkan and 936 x86 DX11 capture buffers. Those isolation checks preceded the final quality selection; final API controls were rerun afterwards.

Na validação isolada da otimização, com a mesma revisão de qualidade dos dois lados, houve zero diferenças nas quatro combinações nativas de tamanho/refinamento e em 936 buffers DX11, 1.379 Vulkan e 936 DX11 x86. Essa comparação isolada antecedeu a seleção final de qualidade; os controles finais de API foram repetidos depois.

## Motion evidence / Evidências de movimento

Known advecting texture with changing illumination; 321×181, 24 frames, last 16 measured. dx also has dy = 0.35×dx. Vector magnitude and endpoint error (EPE) are in source-image pixels. Alignment means within 30° of known motion. This is a controlled reflection/illumination stress case, **not physical water/smoke ground truth**.

Textura com deslocamento conhecido e iluminação variável; 321×181, 24 quadros, últimos 16 medidos. Além de dx há dy = 0,35×dx. Magnitude e erro de posição (EPE) em pixels da imagem fonte; alinhamento significa até 30° da direção conhecida. É um teste controlado de iluminação/reflexo, **não uma medição física da água/fumaça dos jogos**.

| dx / contrast | Magnitude before → candidate | EPE before → candidate | Aligned before → candidate |
|---|---:|---:|---:|
| 0.4 / 0.04 | 0.0443 → 0.1008 | 0.3849 → 0.3488 | 99.35% → 99.24% |
| 0.8 / 0.04 | 0.4101 → 0.5658 | 0.4798 → 0.3595 | 99.92% → 99.96% |
| 0.4 / 0.65 | 0.4254 → 0.3884 | 0.1492 → 0.1517 | 91.07% → 93.84% |
| 0.8 / 0.65 | 0.8408 → 0.8242 | 0.1663 → 0.1658 | 99.61% → 99.80% |

### Temporal variation / Variação temporal

Mean frame-to-frame change in the actual XY vector at the same interior pixel, 15 intervals with constant expected motion. Both normal/inverse observers reproduced the prior EPE numbers within 0.000001 pixel, confirming that the added CPU measurement did not alter the GPU output. This is an absolute velocity-variation measure, not a debug colour or arrow metric.

Mudança média do vetor XY real entre quadros, no mesmo pixel interno, em 15 intervalos com movimento esperado constante. Os observadores normal/inverso reproduziram os EPE anteriores com diferença inferior a 0,000001 pixel, confirmando que essa medição na CPU não alterou a saída da GPU. Mede variação absoluta da velocidade, não cor ou amplificação do debug.

| dx / contrast, normal LK | Before px/frame | Candidate px/frame | Change |
|---|---:|---:|---:|
| 0.4 / 0.04 | 0.054852 | 0.126400 | +0.071548 |
| 0.8 / 0.04 | 0.302713 | 0.335500 | +0.032787 |
| 0.4 / 0.65 | 0.075950 | 0.091519 | +0.015568 |
| 0.8 / 0.65 | 0.093012 | 0.105777 | +0.012765 |

**The candidate recovers real movement and improves directional alignment in some textured cases, but it does not demonstrate improved temporal uniformity. All four representative cases above became more variable between frames.** Previously suppressed movement can have artificially low variation, so this metric must be read with EPE and recovered magnitude; nevertheless the increased variation is a real tradeoff, not a success to hide. The inverse mode shows the same trend. This candidate should not be described as a finished frame-generation flow solution.

**A candidata recupera movimento real e melhora o alinhamento de direção em alguns casos texturizados, mas não comprova melhora da uniformidade temporal. Os quatro casos representativos acima ficaram mais variáveis entre quadros.** Movimento suprimido pode apresentar variação artificialmente baixa; por isso a métrica precisa ser lida junto ao EPE e à magnitude recuperada. Ainda assim, o aumento é uma contrapartida real. O refinamento inverso apresenta a mesma tendência. A candidata não deve ser apresentada como uma solução de vetores pronta para frame generation.

High-contrast direction improves, but some high-contrast speed estimates are slightly attenuated. Across the 80 changing-illumination/static-grain cases (both LK modes), the largest EPE increase is 0.004469 pixel. The smallest 0.05-pixel motions remain mostly suppressed. Recovery is not complete.

A direção melhora no caso de alto contraste, mas algumas estimativas de velocidade de alto contraste ficam um pouco menores. Nos 80 casos com iluminação variável/controle estático (ambos os refinamentos), o maior aumento de EPE é 0.004469 pixel. Movimentos de 0,05 pixel continuam quase todos suprimidos. A recuperação não é completa.

Sky/grain worst control, normal LK: 0.016020 → 0.027816 pixel EPE (+0.011796). This **does not pass** the strict +0.01-pixel screening threshold. Inverse LK worst control: 0.022053 → 0.037735 pixel EPE (+0.015682). The clean static sky remains static. No claim of noise-free game output is made before the user's in-game review.

Pior controle de céu/granulado, LK normal: 0.016020 → 0.027816 pixel de EPE (+0.011796). Isso **não passa** no critério rigoroso de +0,01 pixel usado na triagem. Pior controle com LK inverso: 0.022053 → 0.037735 pixel EPE (+0.015682). O céu estático limpo permanece estático. O resultado nos jogos ainda precisa da revisão do usuário.

## Synthetic GPU cost / Custo sintético de GPU

RTX 3080, 3413×1920, normal LK. Native: full estimator + trust/disocclusion + full-resolution guide, 64 warmup + 160 measured frames, four counterbalanced repeats, median of run medians. Excludes NR, FG, display/debug, readback and IPC. Original reference is pristine OptiScaler 0.1.31 commit aeb439482c13dd7364ee2bef16fa41b902be6f98. Before is the installed modified build.

RTX 3080, 3413×1920, LK normal. Nativo: estimador completo + confiança/desoclusão + guia em resolução final; 64 quadros de aquecimento + 160 medidos, quatro repetições em ordem alternada, mediana das medianas. Exclui NR, FG, debug, leitura para CPU e IPC. Referência original: OptiScaler 0.1.31 sem modificações no commit acima. “Before” é a versão modificada instalada.

| Native case | Original 0.1.31 ms | Before ms | Candidate ms | Change vs before ms |
|---|---:|---:|---:|---:|
| Moving / Movimento | 3.5313 | 3.0817 | 3.0566 | -0.0251 |
| Stationary / Estático | 3.5036 | 2.2190 | 2.2016 | -0.0174 |

ReShade: full motion technique + Feeder GPU technique, 320 frames/run, 60 warmup, eight counterbalanced warm-cache before/candidate runs per route (four per variant). For each measured frame the two GPU intervals are paired and added; each run's median total is calculated before taking the median across runs. Transport/Host/NR are disabled to isolate vector production. These totals do not measure end-to-end Host64 latency and are not a direct FPS prediction.

ReShade: técnica completa de vetores + técnica do Feeder na GPU, 320 quadros por execução, 60 de aquecimento, oito execuções alternadas com cache aquecido por rota (quatro por versão). Os dois intervalos de GPU são somados por quadro; calcula-se a mediana de cada execução e depois a mediana entre execuções. Transporte/Host/NR desligados para isolar a produção dos vetores. Não mede a latência total do Host64 nem prevê diretamente o FPS do jogo.

| Shader route | Before ms | Candidate ms | Change ms |
|---|---:|---:|---:|
| dx11 | 3.7092 | 3.6041 | -0.1050 |
| vulkan | 4.9439 | 5.0442 | +0.1004 |
| host64 | 3.8118 | 3.8281 | +0.0162 |

DX11 has substantial run-to-run spread: before 3.5886–3.8296 ms; candidate 3.6004–3.6623 ms. Only two of its four paired comparisons improved, so its median reduction is not a dependable general speedup. Vulkan and Host64 show a net cost increase in this sample. Native run-median ranges are preserved in native-total-spread.json. GPU clocks/activity are recorded per run; this is a desktop benchmark, not an isolated laboratory GPU.

DX11 apresentou variação relevante entre execuções: antes 3,5886–3,8296 ms; candidata 3,6004–3,6623 ms. Apenas duas das quatro comparações pareadas melhoraram; portanto, a redução da mediana não comprova ganho geral estável. Vulkan e Host64 tiveram aumento líquido de custo nesta amostra. As faixas nativas estão em native-total-spread.json. Frequência/atividade da GPU foram registradas por execução; é um benchmark de desktop, não uma GPU isolada em laboratório.

The isolated sample-reuse optimization previously saved about 0.0205 ms on the moving native control. The tables above include the quality changes too; a small isolated saving is not evidence of a whole-round or whole-game performance win. The earlier 0.7 ms reduction / 1.7 ms overhead target is not established by these measurements.

A reutilização isolada de amostras economizou cerca de 0,0205 ms no controle nativo em movimento. As tabelas acima incluem também as mudanças de qualidade; essa pequena economia isolada não comprova ganho na rodada completa ou no jogo. A meta anterior de reduzir 0,7 ms/chegar a 1,7 ms de diferença não foi comprovada por estas medições.

## Coverage and limits / Cobertura e limites

- Six actual shader controls: DX11, Vulkan and x86 DX11/Host64 producer, normal and inverse refinement. Verified depth conventions, mirrored source, point alignment, optional sky/far rejection, tiny positive depth, missing-depth fallback, validation transitions, seven finite debug views and original DisplayDepth parity. Feeder XY is bit-identical to the producer's consumer guide in these captures.
- Ten NR composition checks: native DX11/Vulkan and shader+Feeder DX11/Vulkan/Host64, model on/off. All passed: output differs when the model is applied and is identical with application disabled.
- For Host64, these before/after captures are taken in the consumer process. They verify model evaluation/composition; these captures alone do not assert that the client displayed the returned frame. The transport implementation was not changed this round.
- Full native AOT/source consistency, affine camera, debug and Trust GPU checks passed. No new game DX12 or FG-backend coverage is claimed. The known Vulkan menu-open FPS issue remains unresolved.
- Colour mapping and arrow gain are unchanged. Delivery is now authorized and the exact candidate is installed. In-game water/smoke/fog/shadow review is still needed; the latest delivery status is recorded separately.

- Seis controles reais do shader: DX11, Vulkan e produtor DX11 x86/Host64, com refinamento normal e inverso. Verificados convenção de profundidade, espelhamento, alinhamento point, rejeição opcional do céu/distância, profundidade positiva mínima, fallback sem depth, transições de validação, sete debugs finitos e paridade com DisplayDepth. O XY do Feeder é idêntico bit a bit ao guia final do produtor nessas capturas.
- Dez verificações de composição NR: nativo DX11/Vulkan e shader+Feeder DX11/Vulkan/Host64, com aplicação do modelo ligada/desligada. Todas passaram: a saída muda com o modelo aplicado e é idêntica com sua aplicação desligada.
- No Host64, essas capturas de antes/depois são feitas no consumidor. Comprovam avaliação/composição do modelo; isoladamente não comprovam a apresentação do quadro devolvido no cliente. O código de transporte não foi alterado nesta rodada.
- Passaram a consistência completa entre fonte/AOT nativo e os testes de câmera afim, debug e Trust. Não se afirma nova cobertura de jogos DX12 ou backends de FG. O problema conhecido de FPS ao abrir o menu em Vulkan permanece sem solução comprovada.
- Cores e amplificação das setas preservadas. A entrega foi autorizada e a candidata exata está instalada. Ainda falta avaliação visual de água/fumaça/fog/sombras nos jogos; o estado da entrega é registrado separadamente.

## Candidate parameters / Parâmetros da candidata

| Gate / critério | Range / faixa |
|---|---|
| Relative speed disagreement / divergência relativa de velocidade | 0.25–0.75 |
| Coherence / coerência | 0.75–0.95 |
| Relative photo gain / melhora fotométrica relativa | 0.08–0.30 |
| Absolute photo gain / melhora fotométrica absoluta, original cost scale | 0.0001–0.00065 |
| Temporal direction agreement / concordância temporal (cosine) | 0.80–0.98 |
| Relative temporal speed change / variação relativa temporal | 0.30–0.90 |
| Recovery response / resposta de recuperação | smoothstep(0.05, 0.30, evidence) |

### Rounding reference / Referência de arredondamento

The older SPIR-V module contains GLSL.std.450 Round (instruction 1), whose halfway direction is implementation-selected; the corrected helper constructs nearest-even behaviour explicitly. The specification distinguishes this from RoundEven (instruction 2). This is a portability/correctness reason for keeping the explicit helper, not a measured image-quality gain on this RTX 3080. [Khronos GLSL.std.450 specification](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html).

O módulo SPIR-V anterior contém GLSL.std.450 Round (instrução 1), cuja direção nos empates depende da implementação; o auxiliar corrigido constrói explicitamente o arredondamento para o par mais próximo. A especificação distingue essa operação de RoundEven (instrução 2). Isso justifica o auxiliar por portabilidade/correção, sem atribuir ganho visual medido nesta RTX 3080. [Especificação Khronos GLSL.std.450](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html).
