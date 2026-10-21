# Revisão das rotas F5Low — 19/10/2026

## Alterações aplicadas

- O estimador corrigido passa a ser o único caminho de produção. Removidos baseline, saída legacy, internal experiments e seletor de sample sharing. Reutilização de custo, aceitação individual do LK e filtragem por evidência ficam automáticas. Permanecem os controles úteis de câmera, histórico, profundidade e bloqueio do céu.
- O compartilhamento embutido reutiliza somente amostras idênticas das imagens. Não compartilha candidatos, resíduos, decisões de confiança nem coordenadas deslocadas de LK. A remoção isolada do seletor preservou os 46 binários DXBC, antes da correção fotométrica separada.
- Shader e Feeder passam a ler profundidade bruta com amostragem pontual/clamp e transformação de espelhamento. Isso evita profundidades interpoladas entre superfícies. A seleção e as transformações de profundidade do usuário são preservadas.
- A aceitação fotométrica nas bordas agora usa as mesmas coordenadas da janela com clamp usadas pelo LK convencional/inverso. O matching inteiro não foi alterado.
- O shader gerado reutiliza a alocação de memória de grupo entre entry points separados. Arrays usados simultaneamente continuam em intervalos distintos. Não há compartilhamento de conteúdo entre dispatches. Isso reduz a alocação excessiva no Vulkan causada por declarações retidas de funções não utilizadas.
- Sete debugs públicos com legenda: setas, profundidade bruta, optical flow final, confiança/rejeição laranja/verde, canais RGB de confiança, history rejection e still age. Trace é captura de desenvolvimento, excluída da compilação normal por padrão.

## Correção de direção: o que está implementado

O matching continua decidindo por pixel. As correções de candidato/LK são distintas da regularização por câmera/região na filtragem. Cada pixel valida essa proposta na sua própria janela fotométrica, com seus pesos de profundidade/luminância. Não é apenas histerese de confiança no Match. A recomendação anterior de comparar os estágios era uma investigação, não uma demonstração de que Smooth era a causa dominante. A medição por estágio está em final-stage-attribution.log. Na faixa superior do pan sintético sem depth, Match tem erro médio 0,481 px, 70,3% de direções alinhadas em até 30 graus e oscilação 0,413 px; Smooth chega a 0,040 px, 100% e 0,054 px, respectivamente. Nesse caso, a inconsistência já existe antes da filtragem e ela a reduz. Isso não prova a causa de toda cena real. Vetores visualmente alinhados não garantem movimento correto em fumaça, HUD ou objetos independentes.

## Resultados e limites

- **DX11:** 208 comparações com o núcleo nativo, sem diferenças nos estágios cobertos, inclusive o guide entregue.
- **Otimização Vulkan:** 24 entradas congeladas idênticas e 551 buffers válidos idênticos byte a byte antes/depois. Profundidade não inicializada, primeiro histórico e preview desativado foram excluídos explicitamente.
- **Profundidade, céu, controles e sete debugs:** testes controlados passaram em DX11, Vulkan e x86/Host64, incluindo profundidade normal/invertida, espelhamento, resolução diferente e transição sem depth. Feeder provider 5 preserva XY; validação opcional altera somente a máscara.
- **NR x64 nativo e Feeder, DX11 e Vulkan:** oito pares por estado e rota confirmam saída alterada com modelo ligado e entrada exata com bypass. Isso comprova composição, não a exatidão dos vetores.
- **Host64:** resultados atuais em final-host64-nr.log e depth-controls-host64-normal-mirror/result.json.
- **Trace:** os vetores finais ficaram idênticos com trace ligado/desligado.
- **Paridade numérica Vulkan/nativo:** ainda NÃO passou: 443 comparações, 133 estágios diferentes na captura anterior à reutilização. Pequenas diferenças aritméticas podem mudar decisões discretas posteriores. A pior diferença de máscara em bytes não é magnitude de vetor. A otimização preserva o resultado Vulkan, mas não resolve essa divergência entre APIs.
- **Suíte de qualidade:** não está completamente verde (exit 1). Dois critérios HDR já falhavam; o critério de ganho relativo na borda também falha porque o resultado sem depth melhorou e tornou o ganho adicional exigido inalcançável. Valores completos em final-quality.log; os testes não foram afrouxados.
- **MGR real:** falta captura da cena problemática. Bloqueio do céu usa profundidade limpa; não é classificação semântica de céu. Um quad desenhado em outra profundidade pode escapar. O teste sintético não encerra o defeito relatado.
- **PCSX2 Vulkan/menu:** o aumento de FPS continua sem causa confirmada. Na candidata atual, o intervalo CPU/Present estável ficou em 45,24 ms com menu fechado e 45,78 ms aberto; o trecho inicial fechado/aquecendo foi 49,19 ms. O NR compôs em ambos os estados. Esse teste não reproduziu o ganho grande e não comprova correção no PCSX2 real. Os tempos de GPU separados estão em vulkan-timing-candidate-nr1-menu1-route-review-final-menu/result.json.

## Custo sintético obrigatório contra o F5Low original

RTX 3080, 3413×1920, 64 quadros de aquecimento e 160 medidos por execução, repetições alternadas. Original sem modificações no commit aeb439482c13dd7364ee2bef16fa41b902be6f98 (0.1.31). Custo nativo completo de luminância, pirâmide, matching/LK, mediana, filtragem/câmera/global, confiança e reconstrução do guide. Exclui NR, debug, leitura pela CPU e transporte entre APIs. Medianas, sem promessa de FPS em jogo.

| Scene / Cena | Original (ms) | Current / Atual (ms) | Delta |
|---|---:|---:|---:|
| stationary | 3.501 | 2.257 | -1.244 (-35.5%) |
| moving | 3.517 | 2.970 | -0.547 (-15.6%) |

## Ganho específico no shader Vulkan

Reutilização da alocação de memória: **12.772 → 4.818 ms**, **-7.954 ms (-62.3%)**, duas repetições alternadas por versão no trecho final estável. NR, transporte, debug e menu desligados. Feeder medido separadamente. Esta comparação antes/depois é distinta da comparação contra o original acima.

## Captura da cena real

No MGR, o addon temporário somente de leitura captura três quadros ao pressionar F10 com o produtor F5Low ligado. A pasta F5Low-route-capture fica na instalação do jogo. Ela contém depth selecionado, depth do guide, matching/mediana/filtragem, câmera, luma/histórico, confiança, guide final e entradas do Feeder. A leitura pela CPU causa pausa somente nos três quadros solicitados. Não altera uniformes, profundidade ou vetores. Remover o addon após a análise.

Sem pacote, PR, publicação, backup ou mudanças no Requiem nesta revisão. Instalação e hashes ficam registrados separadamente em installations.json. A confirmação da cena real permanece pendente.
