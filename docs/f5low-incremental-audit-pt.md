# Auditoria F5Low incremental (Português)

## Escopo e decisão

O modelo de câmera/homografia/selective geometric fusion cancelado e os patches posteriores do estimador foram retirados. A base é F5Low 0.1.31, commit `aeb439482c13dd7364ee2bef16fa41b902be6f98`. Foram preservados integração com Feeder, correções locais de submissão/vida útil do NR, transporte de prévias e debugs relevantes. As demais alterações locais do repositório foram mantidas.

A referência está separada do candidato; SHA-256 do header original: `6f501d350ee6c2504115e726224b45342ba189f8c17f41aafa53a91d9bb0cb90`. AeonSR 1.3.0 foi inspecionado no commit `d381f138b572bfd397c62fc57e552c354871a6ed` para ideias de aceitação fotométrica. Nenhum modelo geométrico dele foi transplantado.

**Padrão instalado:** reutilização de custos LIGADA; aceitação subpixel, filtro por evidência e refinamento inverso DESLIGADOS. Não há pacote nem PR até sua confirmação. Isto não comprova que todo ruído no céu/NR do MGR desapareceu.

## Unidades e transplante

Matching em seis níveis, com nível final em ceil(largura/2) × ceil(altura/2), sem reduzir resolução. O matching usa pixels de cada nível; a mediana multiplica por 2. O campo filtrado é RGBA16F em meia resolução, com deslocamentos em pixels da resolução total e direção atual→anterior. As setas invertem esse sinal para mostrar o movimento visível.

O shader inclui os kernels nativos completos **DepthProxy, GuideDepth, HiddenBefore, Uncover e GuideMotion**, além de luma, pirâmide, matching, mediana, Smooth, candidato global e cortes de cena. A reconstrução final não é apenas ampliação bilinear. O guia completo é RGBA16F em resolução total. Feeder lê `F5Low::PixelMotionVectors` e exporta XY diretamente, sem arredondamento intermediário por UV.

`.z` preserva a estrutura ponderada original do LK. `.w` experimental combina estrutura, erro fotométrico normalizado, separação dos custos e acordo com o preditor histórico válido na mesma posição; não equivale a consistência completa frente/verso. Nenhum vira automaticamente rejeição de histórico.

Profundidade normal/invertida usa inverso da proximidade. ReShade aplica suas correções de coordenadas ao buffer selecionado. O shader recebe um buffer; o coletor nativo com múltiplos buffers continua separado. Não foi adicionado jitter de projeção ao fluxo da imagem final, nem substituída a rota de vetores DLSS nativos do jogo.

Smooth usa aritmética `precise` em ambas as versões para reproduzir arredondamentos entre APIs. Portanto, “increments off” significa algoritmo base com esse contrato numérico, não identidade literal de todo float ao 0.1.31 intocado. Na comparação completa, três linhas impressas mudaram: sparse arredondou 99,2%→99,3% dentro de 1 px, com erro médio igual, e um corte não detectado apenas reportado mudou 138,890→138,891 px. Todos os critérios existentes passaram.

## Mudanças isoladas

1. **Reutilização de custos — aceita, ligada.** Evita recalcular candidatos inteiros iguais ao centro conhecido em preditores global/histórico/células e na busca central. Preserva desempates, penalidade e iterações LK. Seis cenas × sete quadros × matching/mediana/Smooth produziram **58.565.808 bytes idênticos** entre flags 0 e 1. A suíte completa passou.
2. **Aceitação fotométrica do LK — experimental, desligada.** Compara o subpixel final ao candidato inteiro com o mesmo objetivo ponderado de 16 amostras e remoção de média; rejeita apenas piora acima de 1e-6. Corrige o uso de custo antigo quando zeroMargin trocou o vencedor. Melhora bordas sem depth 0,7445→0,7553 e objetos pequenos sem depth 0,6424→0,6500, mas piora grão 0,8164→0,8135, objeto pequeno com depth 0,8746→0,8695 e banda fina lenta 0,5511→0,5448. Erro de pan 0,0441→0,0443 px. Não justifica ativação padrão.
3. **Evidência do vetor — experimental, desligada.** Canal separado `.w`, com filtragem opcional no Smooth e piso 0,05. Reaproveita estrutura/custos e calcula erro/contraste necessários. Melhora algumas bordas, mas o teste sparse piora **0,9981→0,7829 dentro de 1 px**, além de aumentar custo. A combinação 1+2+4 mantém essa regressão. Falhou no critério de qualidade/desempenho; não é melhoria final aprovada.

Os PSOs nativos são especializados e pré-compilados: recursos desligados não carregam o custo de registradores/branches da versão habilitada. Foram regenerados/verificados 37 shaders, sem adicionar compilação HLSL durante a apresentação.

No shader, calcular evidência para debug tem opção explícita, desligada por padrão; deixar um item selecionado não mantém esse cálculo caro escondido. Magenta indica cálculo desligado. No nativo, esse cálculo ocorre enquanto a prévia correspondente é solicitada. Evidência é preservada com raio de suavização zero e pode ser inspecionada com incrementos desligados.

## Zero, histórico e refinamento inverso

`zeroReach` usa pixels do matching fino, portanto meia resolução, limitado a 64. Os padrões de margem/alcance continuam zero. O still-age original condiciona o candidato global após histórico estável; não bloqueia todos os candidatos nem toda correção LK. Não foi imposto zero a céu, baixa textura ou baixa confiança. Um protótipo de zero final também mudava margem/alcance e não serviu de comparação isolada; foi descartado da entrega.

O refinamento inverso já existia e reutiliza gradientes com parada por convergência. Em três pares de medições: mediana 0,986816 ms com reutilização/refinamento comum versus 0,784597 ms com inverso. Na comparação numérica válida do protótipo, linhas finas dentro de 1 px caíram 1,0000→0,0084. Continua desligado por perda de qualidade.

**Correção direcional foi adiada pelo critério solicitado:** confiança/aceitação ainda não passaram no conjunto de qualidade e custo. Não foi apresentada como concluída e não se inventou componente tangencial nem suporte geométrico ausente.

## Desempenho medido do campo nativo

RTX 3080, entrada 1920×1080, 24 quadros de aquecimento + 240 medidos por rodada, três rodadas em ordem alternada. Mediana das médias, timestamps D3D12 por etapa. Inclui luma/pirâmide, corte, matching, mediana, Smooth e candidato global; **não** inclui NR completo, tempo do jogo, guia consumidor ou cópias/transporte do ReShade. Dados por etapa e condições da GPU ficam em `production-performance-raw.json`.

| Configuração | Bits | Mediana ms | Variação contra base modificada |
|---|---|---:|---:|
| upstream | None | 1.083123 | -0.40% |
| base | 0 | 1.087467 | +0.00% |
| reuse | 1 | 1.039134 | -4.44% |
| refinement | 2 | 1.143881 | +5.19% |
| evidence | 4 | 1.160576 | +6.72% |
| combined | 7 | 1.133875 | +4.27% |

A reutilização ficou aproximadamente 4,1% mais rápida que o original e 4,4% que a base modificada neste teste. Uma rodada foi mais lenta; não é garantia de ganho universal. Medições anteriores com Zenless usando cerca de 60% da GPU estão invalidadas em `invalid-contended-timings`. As válidas terminaram antes da abertura posterior do MGR; a comparação do inverso ocorreu após fechar os jogos.

## Feeder

Foram reproduzidos dois erros no provider 5 antigo: máscara quase 0,98 em regiões de baixa estrutura mesmo com validação desligada; hipótese estática substituindo (4,-2) px por zero, com erro de 4,47214 px. O provider 5 agora mantém os vetores completos e ignora seu segundo fitting/zeramento estático. Outros providers mantêm o comportamento anterior.

Validação opcional por profundidade/luma/consistência só escreve máscara de rejeição de histórico. `MV_VALIDATE=0` e `MASK_STRENGTH=0` funcionam. O shader Guides compilado passou em **768 combinações no WARP**: erro vetorial zero, máscara indevida 0,98→0, rejeição opcional ainda chega a 1. A integração de máscara IPC e NR externo foi preservada nos binários modificados existentes; este incremento altera o shader de integração.

## Verificações do transplante

O gerador portátil faz tradução mecânica dos kernels. Adapta bindings/intrínsecas FX, namespaces dos níveis, idade R32U com valores 0–255, histórico por marcador de quadro e um depth buffer. As cópias exigidas pelo agendamento FX não substituem etapas por aproximações.

Uma resolução ímpar revelou diferença entre dimensões constantes dobradas pelo compilador FX e dimensões recebidas pelo constant buffer nativo. Uma fonte de dimensões em tempo de execução, com zero significando tamanho automático da imagem, preserva a aritmética no modo normal recomendado do ReShade, sem gravar essas dimensões no preset. Corrigiu a divergência observada em 641×361. O modo Performance do ReShade deve ficar DESLIGADO: a especialização de parâmetros alterou o matching num teste separado; ambos os efeitos do provider 5 recusam explicitamente esse modo.

**928 comparações em 8 casos passaram sem diferenças nos campos armazenados**: céu/grão em movimento, objeto independente/HUD, textura parada, incrementos combinados, profundidade real com borda móvel, resolução ímpar com/sem profundidade real e profundidade normal/invertida. Cada caso captura 24 entradas consecutivas reais do ReShade e reproduz pelas classes D3D12 de produção. Compara luma, matching, mediana, Smooth e guia completo, além de XY do Feeder bit por bit. O caso de profundidade confirma entrada real ativa. A igualdade direta foi medida FX D3D11 versus nativo D3D12; Vulkan recebe verificações de compilação, execução e transporte, não a mesma prova byte por byte em todo hardware.

Compilação final: DXBC/SPIR-V em 641×361 e 1920×1080, 148 verificações; entradas do Feeder também compiladas nas duas APIs. Testes das setas confirmam direção do movimento conhecido e os 11 debugs preservam os campos. Transporte real D3D12→D3D11/Vulkan passou com tamanhos ímpares/redimensionamento. A camada opcional de debug D3D12 do Windows não está disponível; não foi alegada certificação dessa camada.

## Entrega e pendências

Fontes modificadas nos diretórios OptiScaler, ReShadeMotionVectors e DLSS5-Feeder-v1.19.0. Instruções manuais em README-EN.md/README-PT.md. PCSX2 usa F5Low nativo; MGR substitui Turkey-X/NoFxK pela rota shader→Feeder32→Host64→OptiScaler modificado. Sem backup, pacote ou PR.

Falta sua confirmação visual nos jogos: céu, câmera lenta, limites de objetos e resultado NR. Regiões ambíguas, aliasing e fraquezas originais de HUD em intervalos curtos continuam limitações. Referências primárias: [base F5Low](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/tree/aeb439482c13dd7364ee2bef16fa41b902be6f98), [AeonSR fixado](https://github.com/BarbatosAWLS/AeonSR/blob/d381f138b572bfd397c62fc57e552c354871a6ed/src/motion/optical_flow_shaders.inl).

## Verificações reais do NR e do produtor

A rota Feeder x64 foi executada pelo dispositivo D3D12 privado em D3D11 e Vulkan, com avaliação/composição reais do modelo e encerramento normal. Oito pares antes/depois do mesmo quadro por rodada mudaram com ApplyModel ligado e ficaram idênticos byte por byte com ele desligado nas duas APIs. Isso verifica entrega/aplicação, não qualidade visual subjetiva. Os testes atuais do produtor nativo também passaram em vida útil dos recursos de entrada, drenagem e redimensionamento DXGI. Os resultados estão em `validation-summary.json`.
