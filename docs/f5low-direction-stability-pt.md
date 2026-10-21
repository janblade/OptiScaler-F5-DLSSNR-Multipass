# Estabilidade de direção do F5Low — auditoria, 18/10/2026

## Alterações

- Corrigida a seleção inicial do MGR: `PresetPath` e `StartupPresetPath` agora apontam para `F5Low-Corrected.ini`. A chave de inicialização antiga sobrepunha o preset corrigido anunciado anteriormente. O preset de comparação foi preservado.
- O shader de produção especializa o mesmo caminho LK normal/reutilização de custos do nativo. Filtragem experimental por evidência, refinamento inverso e aceitação fotométrica genérica, anteriormente rejeitados, não podem ser reativados silenciosamente pelos valores de um preset antigo.
- Em textura ambígua, Smooth amplia moderadamente o apoio de movimento dos vizinhos e aplica um piso de tolerância de luminância perto do preto. Isso reduz a dependência de um vetor central possivelmente arbitrário. Com evidência forte, mantém os intervalos originais. Não adiciona EMA global, zeragem de céu/áreas escuras, ajuste geométrico de câmera ou SGF.
- O apoio ampliado respeita superfícies de profundidade, com o piso existente de 0,02. A guarda adicional só é aplicada onde a ambiguidade amplia o filtro. Profundidades iguais dispensam a divisão. A penalidade foi incorporada à exponencial existente.
- LK normal deixa de repetir duas vezes o mesmo tensor incapaz de produzir uma atualização. Reutiliza a janela atual já carregada. Custos do candidato zero e de candidatos grossos consecutivos repetidos também são reutilizados, preservando ordem e desempate.
- Smooth usa grupos cooperativos de 16×16 sobre um bloco de 24×24 com margem, em vez de 8×8 sobre 16×16. São 2,25 entradas do bloco por saída, antes 4. Isso não significa uma redução de 44% no tempo de GPU.
- O desenho das setas evita raízes fora da área de desenho e cálculos de cor em pixels sem seta. O shader ganhou um sampler linear separado para acompanhar a visualização nativa; a exportação dos vetores continua pontual.
- Permanecem sete vistas públicas: setas, profundidade, optical flow final, confiança verde/laranja, canais RGB de confiança, history rejection e still age. Todas têm legenda automática na imagem; as janelas nativas também mostram explicações.
- Still age lê o contador existente sem ativar três buffers de trace. Corrigida a indicação de validade do contador atual no shader, que o ocultava por um frame.
- Removido desse menu o seletor de componentes da máscara nativa: ele alterava a mesma opção de TrustMask utilizada pelo consumidor. A produção nativa agora usa explicitamente a máscara final combinada. Selecionar os novos debugs só escreve a prévia.

## Como ler a confiança

Verde/laranja mostra o menor valor entre estrutura da textura, ajuste fotométrico e aceitação do histórico F5Low. Verde indica apoio forte; laranja indica apoio fraco ou rejeição. Essa visualização não passa a rejeitar nem zerar vetores. A vista RGB separa os sinais: vermelho = textura; verde = ajuste fotométrico; azul = histórico aceito. History rejection mostra a máscara F5Low: branco rejeita, preto aceita, cinza representa rejeição parcial. Validações extras opcionais do Feeder podem aumentar a rejeição final; elas permanecem desligadas no preset instalado. Isso não mede o histórico privado do modelo neural nem uma probabilidade calibrada de o vetor estar correto.

## Verificações

- Leitura de GPU comparando nativo e shader DX11: sete vistas, 208 comparações por vista, sem etapas divergentes após corrigir still age. Match, median, smooth, guia final e XY exportado pelo Feeder coincidem exatamente. A máscara aceita diferença de uma unidade R8; a prévia aceita a quantização R8/half.
- Com trace realmente ligado/desligado, 58.565.808 bytes de campos de movimento são idênticos. A otimização do desvio de profundidade constante também preserva esses 58.565.808 bytes. A alocação dos buffers foi verificada.
- Passaram as verificações nativas de debug/age, valores finitos com knee zero, confiança, etapas e início/parada/inversão/HUD. A camada de debug D3D12 não está disponível nesta máquina; os testes executaram e leram a GPU real.
- Compilação DXBC e SPIR-V aprovada. As rotas nativa DX11, nativa Vulkan e Feeder Vulkan avaliaram e compuseram NR com ApplyModel ligado e desligado. A rota cliente x86 DX11 → Host64 está nas capturas de paridade.
- O provider 5 do Feeder continua exportando XY em pixels sem alterar os vetores com escala/sinal unitários. Sua validação opcional só altera máscara. Nenhum binário do Feeder nem limiar de máscara foi modificado nesta rodada.

Resultado conhecido da suíte original: o executável completo de qualidade ainda retorna 1 por uma única comparação relativa de HDR, entre o modo perceptual e o modo legado da própria versão candidata. Todas as outras asserções originais passam. Comparando os mesmos modos com a versão instalada anterior, houve melhora: EPE do granulado scRGB perceptual 0,389043 → 0,374154 px; legado 0,370504 → 0,343469 px. A falha original não foi apagada: está em `quality-validated.log`; os quatro casos estão em `hdr-comparison.json`.

## Estabilidade medida

São cenas sintéticas com movimento conhecido, e não uma medição do vídeo do MGR. Na faixa superior de textura fraca, em pan de 0,7/0,3 px, jitter caiu de 0,142079 para 0,130352 px e direções dentro de 30° passaram de 96,88% para 98,03%. Em granulado estacionário, movimento falso caiu de 1,054968 para 0,904223 px; jitter de 1,541599 para 1,340926 px. Na borda de objeto com profundidade, EPE passou de 0,250962 para 0,229123 px. Regiões limpas paradas continuam zeradas. Os testes de início/parada/inversão preservam resposta. Interiores texturizados permanecem praticamente iguais; os arquivos registram as pequenas diferenças. Cenas esparsas ou ambíguas ainda não fornecem informação suficiente. Isso não prova que todos os vetores caóticos do MGR foram corrigidos.

## Desempenho

| Carga de GPU | Revisão anterior | Nova revisão |
|---|---:|---:|
| FX, 1920×1080, debug desligado, duas rodadas alternadas | 1.4510 ms | 1.4044 ms |
| Estimador nativo, cena texturizada, mediana de três rodadas | 0.9288 ms | 0.9564 ms |
| Estimador nativo, textura fraca, mediana de três rodadas | 0.9144 ms | 0.8858 ms |
| FX, 3412×1920, debug desligado, um par | 3.7028 ms | 3.5461 ms |
| Somente prévia de setas, 1080p, um par | 0.1270 ms | 0.1034 ms |

Tempos de GPU em RTX 3080, sem outro jogo disputando a GPU. Os clocks variam e estão registrados. Na rota nativa, a cena texturizada custa cerca de 0,028 ms a mais, enquanto a cena de textura fraca economiza aproximadamente 0,029 ms. Não é uma aceleração nativa universal. O Lumenite Kernel 2.0 modificado local, com busca em meia resolução, mediu 0,5787 ms em 1080p: F5Low continua mais pesado. Esses tempos excluem NR e transporte; não demonstram recuperação dos oito FPS relatados nem equivalência de qualidade ao Lumenite.

## Entrega

OptiScaler x64 modificado e shader gerado compartilhado nas duas instalações existentes, mantendo Feeder/Host compatíveis. Esta revisão não altera Requiem e não cria pacote, PR ou backup de jogo. A estabilidade e a máscara na cena real do MGR aguardam sua confirmação antes de pacote ou PR.

Tentativa adicional de otimização: reutilizar amostras coincidentes das derivadas do LK foi testado isoladamente e **não foi adotado**. A proposta alterou 1.054 dos 58.565.808 bytes capturados. Isso não prova regressão de qualidade, mas descumpre o critério de saída inalterada para essa otimização. Consulte `lk-reuse-equivalence.json`.
