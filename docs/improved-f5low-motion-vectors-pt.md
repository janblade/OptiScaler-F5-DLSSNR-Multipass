# improved F5Low motion vectors — relatório técnico

## Escopo

Mudanças do OptiScaler baseadas no F5Low 0.1.31 do Janblade
(`aeb439482c13dd7364ee2bef16fa41b902be6f98`), integradas à versão 0.1.32
(`fadc3bf67f6c8548e94914baec3f2e551ba8d8a4`). A main avançou depois para
0.1.33; os resultados descrevem esta branch testada, não uma integração ainda
não testada da 0.1.33. O PR não publica uma versão nem substitui a main.

Objetivo: vetores coerentes e baratos para NR e frame generation, preservando
movimentos independentes reais de água, fumaça e neblina. Os testes sintéticos
medem o estimador com movimento conhecido; não certificam todos os quadros
interpolados de todos os jogos. A meta de desempenho pendente é declarada
separadamente.

## O que mudou no estimador

- Preservados os seis níveis da pirâmide, candidatos por pixel, pesos de
  profundidade/luminância, Median, Smooth, candidato global, still age e scene cut.
- XY é deslocamento em pixels da imagem atual para a anterior. Depois de Median,
  usa pixels da resolução completa. Z é a estrutura de textura original:
  baixo = evidência fraca, alto = forte. W é suporte separado ao vetor escolhido,
  sem significado de probabilidade calibrada. Rejeição de histórico é outra
  quantidade: 1 = rejeitar, 0 = aceitar.
- Custos de candidatos repetidos são reutilizados somente dentro do mesmo pixel,
  preservando ordem e desempates. A janela e os pesos continuam individuais.
- A bacia fracionária validada do candidato herdado deixa de ser perdida ao
  arredondá-lo para o mesmo deslocamento inteiro.
- O menor autovalor da estrutura, quando ligeiramente negativo por erro numérico,
  é limitado a zero. Ele não pode produzir confiança/suporte negativo.
- Refinamento LK com pouca textura exige resíduo temporal útil e melhora do
  objetivo fotométrico ponderado daquele pixel. Passos subpixel são comparados
  com a referência correta do mesmo sampler fracionário; diferenças entre
  leitura inteira e filtrada nas bordas não bastam para aprovar uma correção.
- Mantidas as separações entre superfícies e a proteção de elementos exatamente
  estáticos. Escuridão, por si só, não determina rejeição de movimento.

## Modelo de câmera e selective geometric fusion

O modelo atual é um prior afim de movimento em espaço de tela, regularizado e
ajustado a observações contemporâneas em 128 regiões, com reponderação robusta.
Não é matriz real da câmera do motor, pose 3D, homografia projetiva nem a primeira
implementação com RANSAC que foi abandonada.

Uma região ambígua pode propor movimento regional coerente ou o modelo da
câmera. Cada pixel valida a proposta na sua própria janela fotométrica com pesos
de profundidade/luminância. Movimento independente confiável e bordas de primeiro
plano são protegidos. Não se impõe uma direção única à imagem inteira: rotação,
paralaxe e objetos independentes exigem direções distintas.

A correção final de baixo contraste preserva advecção independente quando ela
explica a imagem melhor que a câmera, mesmo com pouca estrutura de textura.
Exige ganho fotométrico absoluto e relativo, coerência local e confirmação no
histórico bruto do matching daquele pixel, na sua posição reprojetada. Usar
esse histórico anterior à fusão evita que uma supressão passada bloqueie para
sempre movimento real. Uma proteção de janelas com contraste alto reduz a
confusão em bordas de horizonte. Não há aumento global arbitrário de confiança.

No teste controlado de 8 bits, uma região movendo 0,8 pixel/quadro sobre fundo
parado, contraste 0,04, teve erro final de aproximadamente 0,814 para 0,261 pixel;
em contraste 0,12, de 0,544 para 0,118. Com pan de 0,7 e movimento independente
de 0,8 pixel/quadro, contraste 0,12, de 0,474 para 0,083. Isso não significa que
toda fumaça será recuperada. Movimento de apenas 0,05 pixel/quadro em RGBA8 com
pouco contraste pode já faltar no matching, antes da fusão; não é considerado
resolvido.

## Otimizações que preservam dados

- Compartilhamento de amostras imutáveis idênticas da imagem/depth nas mesmas
  coordenadas, incluindo as mesmas bordas limitadas e barreiras para todas as lanes.
- Reutilização de extremos de gradiente LK somente dentro do próprio pixel e
  quando as coordenadas construídas são idênticas. Não existe janela comum de
  LK para pixels com offsets diferentes.
- Soma dos pesos invariantes uma vez por pixel, na ordem original; parada somente
  em ponto fixo exato de ponto flutuante; divisores inteiros constantes nas mesmas
  posições do cache; reaproveitamento de memória compartilhada entre entry points
  distintos, sem sobrepor dados simultaneamente vivos.
- Cache do registro de entrada já existente de Smooth e da luminância guia nas
  mesmas posições. Copiar o W imutável desse registro é uma exceção explicitamente
  validada à regra de compartilhamento: não se compartilha uma nova decisão ou
  confiança calculada. Permanecem stencil, leitura do centro, pesos e ordem de soma.
- Cache de descritores somente para texturas internas e slots conhecidos. Entradas
  externas de cor/depth continuam reescritas. Identidade, formato e tipo SRV/UAV
  fazem parte da chave; o cache é limpo antes de reciclar recursos. Cada quadro
  avança de banco considerando também as alocações opcionais de debug.

Caches de imagem anterior, grupo maior de matching, cache de amostragem LK,
redução fundida da pirâmide e uma fusão anterior de luma/depth foram rejeitados:
alguns preservaram os dados, mas pioraram tempo de GPU, especialmente em Vulkan.
Menos leituras não implica automaticamente menor custo.

A proposta de normalizar confiança por `16 / peso de depth sobrevivente` não é
apresentada como implementada/aprovada. A hipótese de confiança nas silhuetas é
distinta dos erros de amostragem/refinamento e da supressão pela câmera tratados
aqui.

## Profundidade, debug e integração

- Seleção automática nativa de depth preservada; dimensões zero significam auto.
  Seleção manual por tamanho, escala UV e offsets continua opcional.
- Valores pequenos positivos de depth não são colapsados no plano de limpeza.
  Matching e NR recebem depth bruto alinhado. A reconstrução tem fallback finito
  quando buracos de clear depth deixam a interpolação sem peso válido.
- Normais usam o depth selecionado original com filtragem linear e o cálculo do
  DisplayDepth. Normais à esquerda, depth à direita, nas coordenadas da cena.
  Linearização, far plane e multiplicador da prévia não alteram o depth do NR.
- Bloqueio opcional do céu/depth distante existe e começa desligado. Zera XY,
  suporte publicado e rejeita histórico juntos. Sem depth, não suprime a cena.
  É classificação por profundidade: fumaça/outros elementos podem compartilhar
  essa profundidade; não é detecção semântica do céu.
- Recursos de prévia, estados e sincronização são preservados entre dispositivos
  do produtor, renderer e FG. Identidade COM canônica evita tratar aliases do
  mesmo dispositivo como dispositivos diferentes. Debug não muda o estimador.
- Reset, resize, mudança de encoding e quadros ausentes invalidam históricos.
  Encoding pode mudar legitimamente a saída neural; histórico incompatível não
  deve ser mantido.

Há sete debugs públicos com legenda: setas, normais/depth, optical flow final,
confiança/rejeição laranja/verde, canais RGB de confiança (textura/fotometria/
histórico), history rejection e still age. Trace é diagnóstico opcional de
compilação, desligado no pacote, não um modo de qualidade. Não há opção pública
baseline, legacy output nem chave geral de internal experiments.

O shader ReShade é gerado mecanicamente dos kernels nativos por
`tools/generate_native_fx.py`, incluindo matching completo, câmera, refinamento,
reconstrução e debug; não entrega apenas vetores de câmera. Adapta recursos e
intrínsecos, arredondamento nearest-even no Vulkan e dimensões de execução para
preservar o comportamento em tamanhos ímpares.

Provider 5 entrega XY da guia de resolução completa diretamente ao Feeder, sem
ida e volta por UV nem segunda decisão de câmera/estático. A validação extra só
adiciona rejeição de histórico; desligá-la mantém a máscara própria do produtor.
Metadados e continuidade impedem uso de resultados antigos. O cliente x86 DX11
usa Feeder32 → Host64; DX11/Vulkan x64 dispensam esse processo separado.
Fontes/binários do Feeder, ReShade e Host64 ficam na entrega complementar, fora
do PR somente do OptiScaler.

Instalação Vulkan limpa usa a layer Vulkan e não injeta simultaneamente o proxy
DXGI do ReShade na raiz. O pacote inicia com FG `nofg`, evitando inicializar uma
ponte XeFG por uma configuração aparentemente desligada.

## Verificações e limitações

Matriz: OptiScaler nativo DX11/Vulkan, shader+Feeder x64 DX11/Vulkan e cliente
x86 DX11+Host64. Testes controlados cobrem depth normal/invertido, espelhamento,
fonte de depth menor, céu/clear/far depth, geometria com depth pequeno positivo,
fallback sem depth, continuidade, XY no Feeder, sete debugs finitos e referência
original do DisplayDepth. Capturas com NR ligado/desligado verificam composição
real, separadamente de logs que apenas dizem que entradas foram recebidas.

XeFG e FSR FG têm verificações controladas DX11/Vulkan; o usuário também confirmou
setas e normais/depth, em prévia e fullscreen, com XeFG no jogo. DLSS-G não pode
ser validado nesta RTX 3080: fallback sem FG não conta como aprovação. DX12 nos
jogos ficou fora do escopo solicitado; o backend interno D3D12 continua necessário.

O aumento de FPS ao abrir o menu no PCSX2 Vulkan ainda não foi conclusivamente
reproduzido e resolvido nos testes sintéticos. NR composto e instalação Vulkan
limpa, isoladamente, não provam sua correção. Água/neblina nas cenas reais ainda
precisam de conferência visual do usuário na versão final.

## Medições

Os resultados finais são anexados abaixo. O custo nativo total inclui estimador,
trust, desoclusão e guia em resolução completa; exclui NR, debug, readback e IPC.
O custo das técnicas ReShade e Feeder é listado separadamente. Comparações usam
quadros parados e em movimento, mesma GPU, resolução, entradas e configurações,
com aquecimento e repetições. Variações de FPS do jogo não são apresentadas como
economia medida isoladamente nos vetores.
