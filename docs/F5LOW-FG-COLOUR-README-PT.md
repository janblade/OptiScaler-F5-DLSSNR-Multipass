# Correção do debug F5Low / FG — versão instalada

Instalada no PCSX2 e no Host64 do MGR em 20/10/2026. Abra o menu do OptiScaler pelo seu atalho configurado e entre em **Neural Rendering → Optical F5Low → Advanced → Show the motion and trust pictures**. Escolha o diagnóstico; Enlarge preview e Fullscreen preview usam o mesmo transporte corrigido. Escape sai da prévia ampliada/em tela cheia. Normals/depth usa o buffer original selecionado, com normais à esquerda e profundidade à direita.

Para a saída Vulkan SDR testada no Silent Hill 3 / PCSX2, use **Colour Encoding: Auto** (resolve para tone-mapped sRGB). Outra escolha altera a interpretação da entrada do NR; seu histórico temporal agora é reiniciado automaticamente quando muda o domínio efetivo.

Presets, calibração de depth e opção anterior de FG foram preservados. Ative o backend de FG que quiser comparar. O usuário confirmou a prévia e a tela cheia com XeFG. Runtimes nativos DX11 e Vulkan também exercitaram XeFG/FSR FG; DLSSG não estava disponível neste teste na RTX 3080. O AUDIT-PT.md detalha o escopo, os limites de validação e o custo sintético completo.
