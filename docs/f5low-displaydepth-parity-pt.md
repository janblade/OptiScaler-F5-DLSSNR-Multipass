# Paridade com DisplayDepth e interpretação da profundidade — 20/10/2026



## Diagnóstico



O estimador e o NR recebem **profundidade bruta alinhada**. O alinhamento altera coordenadas/tamanho, não valores de profundidade. Multiplicador da prévia (4500 neste SH3), plano distante e linearização só afetam o diagnóstico. O F5Low também calcula internamente um proxy recíproco para separar superfícies; esse proxy não é o buffer bruto entregue ao NR. O seletor identifica esta cena como profundidade reversa. Os valores emulados do GS e os ajustes de exibição não comprovam distância métrica nem uma projeção fisicamente calibrada; isso não pode ser deduzido da captura de tela.



A seleção automática foi preservada com Source size em 0,0. No SH3 atual, 2048×2048 filtra as dimensões elegíveis, mantendo a escolha automática entre candidatos válidos. A heurística automática de proporção pode rejeitar buffers quadrados; as dimensões explícitas evitam isso neste jogo.



Minha reconstrução anterior de normais não era idêntica ao DisplayDepth: usava projeção ajustada pela proporção, escolhia a menor diferença de profundidade por eixo e lia a cópia já redimensionada por amostragem pontual. Um vizinho repetido podia zerar a inclinação local. A reprodução CPU de um plano gerou faixas e teve 75,28% das normais internas com erro acima de 10 graus. Isso demonstra falha no diagnóstico, não corrupção da profundidade recebida pelo NR. Também analisei uma captura real anterior de SH3; ela não corresponde à nova imagem anexada.



## Correção



- Cálculo original do DisplayDepth: centro/norte/leste, coordenadas, ordem do produto vetorial e dithering.

- Amostragem linear do **buffer original selecionado**, com o mesmo alinhamento por escala/deslocamento. O estimador e o modelo mantêm a profundidade bruta alinhada por amostragem pontual.

- Prévia de profundidade na resolução completa da imagem no OptiScaler e no FX. Os demais debugs mantêm a resolução anterior. Alvos nativos separados e retidos permitem alternar as visualizações sem substituir um recurso em uso. O custo extra só existe para esse diagnóstico; ele pode pesar mais enquanto estiver ligado.

- Legenda e xadrez de ausência de profundidade preservados. Normais degeneradas viram preto antes de intermediários de ponto flutuante, reproduzindo a conversão para preto da saída UNORM do original.

- DisplayDepth.fx original e seu arquivo de tradução restaurados nos dois jogos, com hashes registrados. Efeito e add-on usados como referência nos testes não foram instalados nos jogos.

- Linearização do diagnóstico ligada por padrão. Valores salvos, calibração e escolha de rota do usuário preservados na instalação.



## Validação GPU



O teste nativo independente usa as funções originais de normais/dithering e compara Visualise → Pass → AOT com uma referência compilada separadamente. Quatro casos cobrem profundidade normal/reversa, prévia bruta/linear, dimensões diferentes, escala/deslocamento e áreas de profundidade limpa. Também verifica vetores e profundidade do modelo idênticos byte a byte, alternância entre as duas resoluções, ausência de profundidade e integridade do dispositivo. Resultado em native-reference-gpu.log.



Nas três rotas ReShade, o PS original do DisplayDepth desenha em um alvo RGBA8 independente na resolução completa. Comparamos RGB fora das linhas 0..39 ocupadas pela nossa legenda:



- dx11: 617283 checked RGB channels, maximum error 0/255.
- vulkan: 617283 checked RGB channels, maximum error 1/255.
- host64: 617283 checked RGB channels, maximum error 0/255.



Tolerância de 1/255. Alfa foi excluído: o PS float3 original não define o alfa desse alvo de teste, enquanto nosso overlay permanece opaco. Isso verifica aparência de referência nas entradas controladas, não igualdade bit a bit do estimador entre APIs. Os testes de 55 quadros dos vetores/Feeder também passaram nas três rotas: dimensões/orientação da profundidade, controle de céu, ausência de profundidade, validação de histórico, sete debugs válidos e transferência XY exata.



O usuário confirmou artefatos também no DisplayDepth original desta cena. Geometria ausente, buracos de profundidade limpa ou quantização da fonte continuam visíveis; a correção não inventa profundidade para escondê-los. A aparência da instalação nova ainda precisa de confirmação nessa cena.



## Custo sintético completo contra o F5Low original



Referência original aeb439482c13dd7364ee2bef16fa41b902be6f98; RTX 3080; 3413×1920; mesmas entradas/configurações; duas alternâncias original/atual por cena parada/com movimento; 64 quadros de aquecimento e 160 medidos por execução. Sem jogos ou outros testes GPU durante a medição. Inclui luminância/pirâmide, Match/LK, mediana, filtragem/câmera/global, confiança e guia completo. Exclui NR, debug, leitura CPU e transporte shader/Host. São tempos sintéticos D3D12 dos vetores, não FPS de jogo nem custo de debug/transporte.



| Cena | Original | Atual | Diferença |
|---|---:|---:|---:|
| Parada | 3.500 ms | 2.269 ms | -1.230 ms (-35.16%) |
| Movimento | 3.500 ms | 3.043 ms | -0.457 ms (-13.05%) |



Incluindo o alinhamento de profundidade por quadro na versão atual: o original recebe a mesma cópia já alinhada fora de sua medição; a atual mapeia o buffer 2048 quadrado com escala Y 1,143 e deslocamentos −4,+117 dentro da medição.



| Cena | Original | Atual | Diferença |
|---|---:|---:|---:|
| Parada | 3.497 ms | 2.292 ms | -1.205 ms (-34.46%) |
| Movimento | 3.502 ms | 3.082 ms | -0.420 ms (-11.99%) |



SHA256 do estimador FX continua 324ee1e706413e12a2eacb44126ba4c3b8ae52427bc6162bf35d9f61bcfea0a0. Esta correção visual não adiciona uma nova otimização do estimador; os ganhos acumulados contra o original já existiam. O problema de FPS/menu Vulkan continua sem solução comprovada. Frame generation continua experimental até verificarmos os quadros interpolados em desoclusões, objetos independentes, partículas/transparências e HUD. Sem PR ou pacote.

