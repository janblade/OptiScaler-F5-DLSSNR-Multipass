# Auditoria da integração e otimização F5Low x64

## Escopo

OptiScaler F5Low e shader ReShade gerado mecanicamente a partir de seus kernels de estimativa, modelo de câmera, refinamento, reconstrução e confiança. PCSX2 usa Vulkan x64 sem Host; Metal Gear Rising usa cliente Feeder x86 e Host x64. Requiem fica fora desta atualização. Pacote e PR dependem da confirmação visual nos jogos.

## Correções

- Preservar profundidade reversa muito pequena, mas diferente de zero. O piso antigo de 1e-6 achatava a geometria do PCSX2. Só o valor real de limpeza é excluído do ajuste de câmera. A profundidade enviada ao NR continua sendo a original do jogo.
- Evitar vetores não finitos em pequenos buracos de profundidade quando os quatro texels vizinhos representam geometria próxima. Os pesos normais são preservados; a alternativa só atua se a soma original for inválida ou zero. O caso reproduzido passou de 230.400 saídas não finitas para zero em 925.604 saídas.
- Recuperar refinamento LK de baixo contraste somente com condicionamento suficiente, resíduo temporal explicado e melhora comprovada no objetivo fotométrico original daquele pixel. Isso é uma alteração deliberada de qualidade, separada das otimizações que preservam dados.
- Validar propostas regionais e de câmera por pixel, com separação por profundidade e aparência. Proteger elementos exatamente parados e movimentos independentes de baixo contraste confirmados pela imagem, como fumaça. Uma região escura também pode ter movimento real.
- Separar suporte do vetor e rejeição de histórico. Suporte vai de 0, fraco, a 1, forte. A máscara de rejeição aumenta até 1 para histórico rejeitado. Não são probabilidades calibradas.
- Preservar arredondamento de empate para o inteiro par nos candidatos Vulkan e alinhar a aritmética da pirâmide de luma SDR à implementação nativa.
- No provider 5, o Feeder exporta XY em pixels diretamente. Suas verificações extras acrescentam máscara sem trocar os vetores por decisões próprias de câmera ou imobilidade. A máscara do F5Low continua obrigatória. Uma marca de continuidade impede usar histórico antigo ao reativar as verificações extras.

## Otimizações com preservação dos dados

- Reutilizar endpoints idênticos de gradientes LK dentro do próprio pixel, verificando igualdade das coordenadas calculadas. Não há janela deslocada comum a pixels vizinhos.
- Reutilizar custos de candidatos duplicados dentro de cada pixel, mantendo ordem e desempates.
- Encerrar apenas em ponto fixo exato de ponto flutuante, preservando confiança e contadores de trace.
- Somar os pesos invariantes uma vez por pixel, na ordem original, e reutilizar a soma nos custos e refinamentos daquele pixel.
- Expor divisores inteiros constantes para os mesmos endereços de imagem compartilhados. Remover uma matriz LDS de suporte cujos valores só eram usados pelo próprio pixel de saída.
- Omitir cálculo de profundidade redundante quando as quatro profundidades da reconstrução são iguais, mantendo pesos bilineares, piso e ordem de soma.
- Fundir passes ReShade compatíveis dentro do limite de UAVs do DX11. Omitir publicação UV legada não utilizada pelo provider 5 e cópias de histórico de validação do Feeder quando essas verificações extras estão desligadas.

Compartilhar amostras brutas idênticas não autoriza compartilhar decisões, candidatos, resíduos, confiança, pesos ou coordenadas LK dependentes de movimento. O modelo regional/de câmera é uma regularização de qualidade avaliada separadamente.

## Desempenho e limites

RTX 3080, aproximadamente 3412/3413 x 1920, cenas sintéticas. Valores são medianas de tempo de GPU; não prometem FPS nos jogos. Medições nativas excluem o modelo NR. Shader + Feeder foram medidos sem transporte/NR para isolar o custo dos shaders.

| Comparação | Antes | Depois | Economia |
|---|---:|---:|---:|
| Nativo, cena parada | 2,781 ms | 2,076 ms | 0,705 ms |
| Nativo, movimento | 3,016 ms | 2,791 ms | 0,225 ms |
| Shader DX11 + Feeder, movimento | 3,921 ms | 3,560 ms | 0,360 ms |
| Shader Vulkan + Feeder, movimento, repetição recente 1 | 13,893 ms | 13,054 ms | 0,839 ms |
| Shader Vulkan + Feeder, movimento, repetição recente 2 | 14,035 ms | 13,097 ms | 0,939 ms |

Comparações anteriores no Vulkan economizaram cerca de 0,675 ms em cenas paradas, mas apresentaram regressão de aproximadamente 0,475 ms no fluxo com movimento. As últimas repetições alternadas melhoraram, porém essa variação impede afirmar ganho universal em movimento no Vulkan. A atividade gráfica externa e o escalonamento afetam a medição; os relatórios individuais e a telemetria foram preservados.

Comparações de otimização verificaram 274.159.339 bytes sem diferenças, incluindo refinamento convencional e inverso nos casos cobertos. Isso não significa que as correções deliberadas de qualidade mantenham a saída antiga. A comparação DX11 entre nativo e shader passou em 208 casos antes da última limpeza equivalente de índices; a repetição final tem relatório próprio.

Vulkan funciona e o Feeder preserva seu XY exatamente. A comparação estrita, bit a bit, entre SPIR-V Vulkan e DXBC nativo NÃO passou: diferenças de ponto flutuante podem mudar decisões discretas posteriores e provocar discrepâncias maiores em casos isolados. Código compartilhado e mesma intenção de amostragem não comprovam identidade entre APIs.

Há verificações de GPU para sete debugs, exclusão de céu, ausência de depth, profundidade pequena válida, validação OFF/ON/OFF e independência do trace. A suíte geral apresenta duas falhas nos critérios HDR perceptual versus legado tanto na versão anterior quanto na atual, além de pequenas regressões em algumas métricas sintéticas de linhas fracas/pans lentos. Esses resultados não foram classificados como aprovados.

O aumento de FPS ao abrir o menu do OptiScaler no PCSX2 continua sem causa comprovada. O NR permaneceu compondo nos dois estados. O teste isolado não reproduziu o aumento grande. O menu pode entrar na imagem recebida pelo ReShade; nenhuma mudança especulativa na ordem do renderizador foi aplicada.

## Controles e debugs

F5LOW_INTERNAL_EXPERIMENTS não é necessário. Reutilização de custos, guarda de refinamento, filtro por evidência e refinamento inverso têm controles diretos. As correções são automáticas. Trace registra diagnóstico e não deve mudar os vetores.

Sete vistas públicas: setas, profundidade, fluxo final, confiança/rejeição, canais de confiança, rejeição de histórico e still age. Todas têm legenda. Confiança clássica: verde forte, laranja fraco ou rejeitado. Canais RGB: vermelho estrutura, verde ajuste fotométrico, azul histórico aceito. Rejeição de histórico: branco rejeitado, preto aceito.

A opção de céu do shader exclui o valor exato de limpeza do depth da saída de movimento/confiança e rejeita seu histórico. Precisa do depth correto. Fumaça e cenários que compartilham esse valor também serão excluídos; não é segmentação semântica de céu. Padrão: desligada.

## Evidências

Nesta pasta: selected-full-build.log, tile-index-snapshot.log, tile-index-inverse-after.log, pixel-weight-total-snapshot.log, paused-browser-confirm.json e telemetria, clean-performance-vulkan.json, final-core-checks.json, final-guide-extreme.log, final-quality.log, sky-validation-final/result.json e relatórios individuais dos contratos DX11/Vulkan. A entrega e as rotas finais recebem registros separados após compilação/instalação.

Referências primárias de arredondamento: [HLSL round](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-round), [GLSL.std.450 Round/RoundEven](https://registry.khronos.org/SPIR-V/specs/1.0/GLSL.std.450.html).

## Verificação final desta candidata

- As 46 entradas de bytecode nativo foram regeneradas a partir dos fontes selecionados. Os kernels padrão/trace medidos são idênticos aos finais (`selected-source-bytecode-audit.json`).
- Transplante DX11 final: 208 comparações, zero etapas diferentes, com NR aplicado (`contract-candidate-641x361-nr1-selected-final`).
- Comparação estrita Vulkan final: 443 comparações, 132 etapas diferentes; NR aplicado e XY do Feeder preservado. A paridade exata falhou; não foi classificada como aprovada (`vulkan-contract-candidate-641x361-nr1-selected-final`). O maior erro informado inclui máscaras em bytes; não representa magnitude de movimento em pixels.
- Quatro rotas x64 aprovadas: nativa DX11, Feeder DX11, nativa Vulkan e Feeder Vulkan. Cada uma capturou oito pares de entrada/saída com ApplyModel ligado e desligado: ligado alterou a imagem; desligado preservou-a exatamente (`selected-routes.json`).
- Cliente x86 / Host64 também avaliou e compôs NR com a DLL final (`selected-route-x86-Host64/result.json`). O aplicativo privado de teste foi corrigido para aguardar a primeira compilação dos shaders antes de expirar; esse aplicativo não é instalado nos jogos.
- Sete verificações centrais passaram, incluindo profundidade extrema e identidade binária dos vetores com trace ligado/desligado. Exclusão de céu, depth pequeno válido, ausência de depth, transições da validação, XY preservado e sete debugs finitos passaram novamente com a DLL final.
- O desempenho nativo final usa duas repetições alternadas por cena (`selected-native-performance.json`). O ganho parado foi aproximadamente 0,705 ms; em movimento, aproximadamente 0,225 ms. As medições do shader mantêm a variabilidade descrita acima.
