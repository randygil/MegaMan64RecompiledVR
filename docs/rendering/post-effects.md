# Post effects de la iluminación raster mejorada

Módulo `PostEffects` de RT64 (`lib/rt64/src/render/rt64_post_effects.{h,cpp}` y `lib/rt64/src/shaders/PostEffects*`):
bloom, light shafts (god rays) del sol, color grading, sharpening (CAS) y dithering sobre cada escena 3D iluminada.
Es **agnóstico al juego**: solo usa el color y la profundidad de la escena y los parámetros genéricos de la
iluminación (`interop::LightingParams`). No conoce direcciones de RAM, áreas ni nada de Mega Man 64.

Documentos relacionados: `HANDBOOK.md` (arquitectura de RT64), `techniques-research.md` (§8 god rays, §9 bloom,
§10 grading, §11 CAS, §14.2 dither, §18 licencias).

---

## 1. Dónde se ejecuta

`FramebufferRenderer::submitRasterScene` encuentra el marcador `InstanceDrawCall::Type::PostScene` de cada escena
iluminada (después de sus superficies translúcidas y antes de lo que se dibuja encima, como el HUD) y llama a
`LightingRenderer::recordPostEffects`, que copia el color con `copyColor` y llama a `PostEffects::record`.

Entradas (`PostEffectsSceneDesc`):

| Campo | Estado al entrar | Uso |
|---|---|---|
| `colorTarget` | `COLOR_WRITE`, posiblemente MSAA | Destino de la composición final (solo RGB). Se deja en `COLOR_WRITE`. |
| `depthTarget` | `DEPTH_READ`, posiblemente MSAA | Máscara del cielo para los rayos (muestra 0 si es MSAA). |
| `rect` | — | Región de la escena; todo se limita a ella (scissor y coordenadas clampeadas). |
| `lighting` | — | `viewProj`, `pixelToClip`, `depthToClip.z` (umbral de fondo), `sunDirection` (w = 1 si hay sol), `sunColor`. Su dirección identifica a la escena (ver §5). |
| `sceneColor` | `SHADER_READ` | Copia resuelta del color, al menos del tamaño del target (su tamaño real se lee con `GetDimensions`). |
| `time` | — | No se usa (el dither es estático a propósito, ver §7). |

Salida: el color target con los efectos aplicados dentro de `rect`. El canal alfa (cobertura del RDP) no se toca.
El framebuffer, pipeline y descriptor sets enlazados cambian; quien llama los restaura (ya lo hace).

## 2. Pases

Todos son pases de fragment con `FullScreenVS` (un triángulo que cubre el viewport). Para un `rect` de W×H:

| # | Pase (shader) | Resolución | Formato | Lee | Escribe |
|---|---|---|---|---|---|
| 1 | Prefiltro (`PostEffectsPrefilterPS[MS]`) | ½ (W/2 × H/2) | RGBA16F | `sceneColor`, depth | nivel 0: RGB = partes brillantes, A = máscara de rayos |
| 2 | Rayos, pasada 1 (`PostEffectsShaftsPS`) | ¼ (Bajo/Medio) o ½ (Alto/Ultra) | R16F | A del nivel 0 | rayos A |
| 3 | Rayos, pasada 2 (mismo shader) | igual | R16F | rayos A | rayos B (no en Bajo) |
| 4 | Downsample (`PostEffectsDownsamplePS`) × (N−1) | ¼ … 1/2^N | RGBA16F | nivel k−1 | nivel k |
| 5 | Upsample (`PostEffectsUpsamplePS`) × (N−1) | 1/2^(N−1) … ½ | RGBA16F | nivel k+1 | nivel k (blend) |
| 6 | Composición (`PostEffectsComposePS`) | completa, scissor = `rect` | formato y MSAA del target | `sceneColor`, nivel 0, rayos | target (RGB) |

Detalles:

1. **Prefiltro**. Cada texel cubre 2×2 píxeles de la escena. Filtro ancho de 13 muestras bilineales (Jimenez, COD:AW
   2014: cinco cajas 2×2 solapadas, centro 0,5 y esquinas 0,125) con *Karis average* por caja (peso 1/(1+luma)) contra
   el parpadeo de píxeles brillantes aislados; en Bajo, filtro estrecho de 5 muestras (estilo dual filter de Bjørge
   2015). Cada muestra pasa por un **soft knee sobre la luminancia** (`threshold`, `knee`): la imagen es LDR, así que
   solo aporta lo que pasa del umbral, con una transición cuadrática. La máscara de rayos (alfa) es
   `fracción de fondo del bloque 2×2 (depth ≥ depthToClip.z) × brillo del cielo (rampa desde SHAFTS_THRESHOLD) ×
   (1 − distancia al sol / radio)²`. Funciona con el cielo 2D original del juego (que no suele tener disco solar) y con
   el cielo procedural.
2. **Rayos** (Mitchell, GPU Gems 3 cap. 13, reimplementado desde la fórmula). Por píxel se promedia la máscara a lo
   largo del segmento hacia el sol (`density` = fracción del segmento), con pesos que decaen `exp(−falloff)` de un
   extremo al otro (independiente del número de muestras). La segunda pasada promedia sobre el largo de **un paso** de
   la primera, en la misma dirección: elimina el banding de anillos sin más muestras (equivale a N² posiciones). Bajo
   hace una sola pasada con *interleaved gradient noise* (IGN) para cambiar banding por ruido fino.
3. **Bloom**. Cadena de N niveles desde ½ (4/5/6/6 según calidad). El downsample usa el mismo filtro de 13 (o 5)
   muestras sin Karis. El upsample es un tent 3×3 (o 4 bilineales en Bajo) con radio `BLOOM_RADIUS` en texeles del
   nivel menor, mezclado sobre el nivel mayor con alpha blending (`SRC_ALPHA / INV_SRC_ALPHA`, el shader devuelve
   alfa = `scatter`): `nivel_k = lerp(nivel_k, tent(nivel_k+1), scatter)`. Los pesos de todos los niveles suman 1, así
   que los niveles anchos reparten la luz sin hacer el bloom más brillante. El upsample no escribe el alfa (conserva la
   máscara).
4. **Composición**, en este orden:
   1. **CAS** (AMD FidelityFX CAS, portado de `CasFilter` sin escalado; MIT, aviso completo en el shader) sobre la
      escena; cruz de 5 muestras en Bajo y 3×3 con diagonales en el resto. Peso de los vecinos
      `−1/lerp(8, 5, sharpen)`; 0 desactiva.
   2. **Bloom + rayos** con *screen blend*: `1 − (1 − c)(1 − luz)`. Aclara sobre todo lo oscuro y nunca recorta en LDR
      (se comporta como una suma para valores pequeños). Los rayos se tiñen con `sunColor` normalizado (el tono del sol,
      no su intensidad) y se atenúan cuando el sol sale de `rect`.
   3. **Grading**: temperatura (balance cálido/frío conservando la luminancia), contraste (mezcla con una curva S
      `x²(3−2x)`, que deja fijos el negro y el blanco), saturación y vibrance (idea de Vibrance de SweetFX, MIT:
      satura más lo menos saturado), viñeta solo en las esquinas (`smoothstep(0,4; 2,0; r²)`). Con 0, cada uno es la
      identidad.
   4. **Dither** triangular de ±1 paso del formato (1/255 en RGBA8, 1/65535 en RGBA16) con dos IGN, contra el banding
      de los degradados añadidos.

Posición del sol: la dirección del sol con w = 0 se proyecta con `viewProj` (punto de fuga, exacto para un sol en el
infinito); w ≤ 0 → detrás de la cámara → sin rayos. Luego `pixel = (ndc − pixelToClip.zw) / pixelToClip.xy`. La
visibilidad baja con `smoothstep` hasta 0 cuando el sol queda a más de `SHAFTS_EDGE_FADE × altura del rect` fuera del
rect. Sin sol, sin profundidad o con visibilidad 0 no se ejecutan los pases de rayos.

Se omite lo que no hace falta: sin bloom ni rayos no hay prefiltro ni cadena (queda solo la composición con CAS y
grading); sin bloom no hay cadena; sin rayos el prefiltro no calcula la máscara.

## 3. Valores ajustables

Todos se leen con `enhancementValue` en cada frame (variable de entorno o archivo `RT64_RT_TUNING_FILE`, recargado en
vivo). `enabled()` es verdadero si `RT64_POST_ENABLE > 0` y algún efecto tiene intensidad distinta de 0 (el dither
solo no cuenta).

| Nombre | Defecto | Efecto |
|---|---|---|
| `RT64_POST_ENABLE` | 1 | Interruptor global. |
| `RT64_POST_QUALITY` | −1 | −1 usa `getRasterLightingQuality()`; 0–3 fuerza un preset (para medir). |
| `RT64_POST_BLOOM_STRENGTH` | 0,3 | Intensidad del bloom (0 lo apaga). |
| `RT64_POST_BLOOM_THRESHOLD` | 0,7 | Umbral de luminancia. |
| `RT64_POST_BLOOM_KNEE` | 0,2 | Ancho de la transición suave alrededor del umbral. |
| `RT64_POST_BLOOM_SCATTER` | 0,65 | Peso de los niveles anchos (0 = solo ½, 1 = solo el más ancho). |
| `RT64_POST_BLOOM_RADIUS` | 1,0 | Radio del tent del upsample (texeles del nivel menor). |
| `RT64_POST_BLOOM_LEVELS` | 0 | 0 = según calidad (4/5/6/6); 1–8 fuerza el número de niveles. |
| `RT64_POST_SHAFTS_STRENGTH` | 0,35 | Intensidad de los rayos (equivale a *weight × exposure* de Mitchell; 0 los apaga). |
| `RT64_POST_SHAFTS_DENSITY` | 0,9 | Fracción del camino hacia el sol que cubren las muestras (0,05–1). |
| `RT64_POST_SHAFTS_FALLOFF` | 1,0 | Decaimiento a lo largo del rayo (*decay*); el extremo del sol pesa `exp(−falloff)`. |
| `RT64_POST_SHAFTS_THRESHOLD` | 0,35 | Luminancia del cielo desde la que emite rayos. |
| `RT64_POST_SHAFTS_RADIUS` | 0,6 | Radio del brillo del sol en la máscara, relativo a la altura del rect. |
| `RT64_POST_SHAFTS_EDGE_FADE` | 0,4 | Cuánto puede salir el sol del rect (relativo a la altura) antes de que los rayos desaparezcan. |
| `RT64_POST_SHARPEN` | 0,35 | Nitidez de CAS (0–1; 0 lo apaga). |
| `RT64_POST_CONTRAST` | 0,1 | Curva S (negativo baja el contraste). |
| `RT64_POST_VIBRANCE` | 0,15 | Vibrance (negativo desatura lo poco saturado). |
| `RT64_POST_SATURATION` | 0 | Saturación adicional (−1 = gris). |
| `RT64_POST_TEMPERATURE` | 0 | Positivo = cálido, negativo = frío (±10 % rojo/azul con −1…1). |
| `RT64_POST_VIGNETTE` | 0,1 | Oscurecimiento de las esquinas (las esquinas pierden ese porcentaje). En VR conviene 0. |
| `RT64_POST_DITHER` | 1 | Amplitud del dither en pasos del formato (0 lo apaga). |

## 4. Presets de calidad y costo

| Calidad | Niveles de bloom | Filtros (prefiltro, down, up, CAS) | Rayos |
|---|---|---|---|
| 0 Bajo | 4 | 5 muestras, 5, 4, cruz | ¼, 1 pasada × 16 con IGN |
| 1 Medio | 5 | 13, 13, tent 9, 3×3 | ¼, 2 × 12 |
| 2 Alto | 6 | igual | ½, 2 × 16 |
| 3 Ultra | 6 | igual | ½, 2 × 24 |

Costo **estimado** (no medido aún) a 1080p en una GPU media (RTX 2060), sin contar `copyColor` de LightingRenderer
(~0,05–0,1 ms, más con MSAA): composición ≈ 0,12–0,2 ms (domina: 5 o 9 lecturas de la escena y la escritura del
target, ×muestras si es MSAA), prefiltro ≈ 0,02–0,04, cadena de bloom ≈ 0,05–0,12, rayos ≈ 0,02 (¼) a 0,15 (½, 2×16).
Total ≈ **0,25 ms** en Bajo, **0,35 ms** en Medio, **0,5 ms** en Alto/Ultra. En iGPU, Steam Deck o Quest multiplicar
por 3–4. Medir con `RT64_PRINT_FRAME_TIME=1` y `rt_perf.ps1` comparando `RT64_POST_ENABLE 0/1` y `RT64_POST_QUALITY 0–3`.

Memoria por escena a 1080p: niveles de bloom ≈ 5,6 MB (RGBA16F) + rayos 0,5 MB (¼) o 2 MB (½). Las texturas se crean
redondeadas a múltiplos de 64 px de la escena y solo crecen.

## 5. Recursos y sincronización

- **Pipelines**: los de los pases intermedios tienen formatos fijos y se crean en el constructor; los de la composición
  se crean bajo demanda por (número de muestras, formato) en un `std::map`, como `LightingRenderer::getComposePipelines`.
- **Por escena**: cada escena tiene sus propias texturas, framebuffers y descriptor sets (`Impl::Scene`). La clave es
  la dirección de su `LightingParams`, única por escena dentro del frame y estable entre frames (vive en
  `LightingRenderer::scenes`). Esto evita dos problemas: actualizar un descriptor set que un comando anterior de la
  misma command list todavía va a usar (cada escena tiene el suyo) y destruir texturas en uso al redimensionar (las de
  una escena solo las usan sus propios comandos, que pertenecen a un frame anterior ya terminado, porque RT64 ejecuta y
  espera cada command list antes de grabar la siguiente).
- **Fin de frame sin hooks**: como cada escena se graba una vez por frame, volver a ver una escena indica que empezó
  otro frame; las escenas que no aparecen durante un frame completo se liberan. Requisitos (documentados en el header):
  `record()` a lo sumo una vez por escena y frame, y frames grabados uno después del otro.
- **Sin compute**: los pases intermedios son de fragment para que funcionen igual en D3D12, Vulkan y Metal, en GPUs
  móviles por tiles (Quest), y para no tener que pasar `sceneColor` (que llega en `SHADER_READ` para gráficos, es decir
  `PIXEL_SHADER_RESOURCE` en D3D12) ni la profundidad a estados legibles desde compute. Tampoco hacen falta texturas
  `STORAGE | UNORDERED_ACCESS` ni lecturas de UAV tipadas.
- **Barreras**: cada textura intermedia pasa a `COLOR_WRITE` antes de dibujarse y a `SHADER_READ` después; antes de la
  composición todas las que enlaza quedan en `SHADER_READ` (aunque su efecto esté apagado ese frame; el shader no las
  muestrea si su intensidad es 0, así que el contenido indefinido de una textura nueva nunca llega a la imagen).

## 6. Portarlo a otro renderer (otros juegos N64, ports de PS1)

Los shaders no dependen del RDP ni de nada de RT64 salvo `shared/rt64_hlsl.h` (tipos compartidos con C++). Para
llevarlos a otro motor:

1. Copiar `PostEffectsParams.hlsli` (constantes compartidas con C++; usa `HLSL_CPU` para compilar también en C++),
   `PostEffectsCommon.hlsli` y los seis `PostEffects*PS.hlsl`, más un vertex shader de triángulo a pantalla completa.
2. Proveer: una copia del color de la escena sin MSAA (o resuelta), la profundidad (o cualquier máscara de "cielo" en
   su lugar: basta con que el prefiltro sepa qué píxeles son fondo) con su umbral de fondo, la posición del sol en
   píxeles (proyectar la dirección del sol con w = 0 con la matriz vista-proyección) y su color. En PS1 no hay depth
   buffer: la máscara puede salir de marcar el fondo/cielo al dibujarlo (por ejemplo, un stencil o el alfa) y pasar
   `backgroundDepth` acorde.
3. Repetir la secuencia del §2 con las mismas constantes (ver `record()` en `rt64_post_effects.cpp`): prefiltro a ½,
   rayos (1–2 pasadas), downsample/upsample con el blend `SRC_ALPHA / INV_SRC_ALPHA` y composición con máscara de
   escritura RGB.
4. Si el motor trabaja en HDR (16F), el soft knee puede usar valores > 1 directamente y la composición puede sumar en
   lugar de usar *screen*; si trabaja en sRGB lineal, aplicar el grading después del tonemap.

## 7. Licencias y referencias

| Pieza | Origen | Licencia / uso |
|---|---|---|
| CAS | AMD FidelityFX CAS, `ffx_cas.h` (`CasFilter`) | **MIT**, © 2017-2019 Advanced Micro Devices, Inc. Código portado; aviso completo en `PostEffectsComposePS.hlsl`. |
| Filtro de 13 muestras + Karis + tent | Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 | Técnica de una charla; reimplementada. |
| Filtro de 5 muestras | Bjørge, "Bandwidth-Efficient Rendering", SIGGRAPH 2015 (dual filter) | Técnica; reimplementada. |
| Rayos por radial blur | Mitchell, "Volumetric Light Scattering as a Post-Process", GPU Gems 3 cap. 13 | El código del libro no tiene licencia explícita: reimplementado desde la fórmula. |
| Vibrance | Idea de `Vibrance.fx` de SweetFX (MIT, © CeeJayDK) | Fórmula reescrita; se menciona en el shader. |
| IGN | Jimenez 2014 | Fórmula pública. |

El dither es estático (no usa `time`): el juego corre a 30 fps y un patrón animado se vería como grano parpadeante.

## 8. Limitaciones conocidas

- Valores por defecto elegidos con una simulación en CPU de los mismos pases (escena sintética), sin probar aún en el
  juego: el bloom en LDR es deliberadamente discreto (las paredes blancas apenas brillan) y las fuentes de luz pequeñas
  casi no generan halo (no hay máscara emisiva ni pseudo-HDR).
- Los rayos son de espacio de pantalla: desaparecen si el sol sale de la pantalla más allá de `SHAFTS_EDGE_FADE`, se
  dibujan delante de objetos cercanos (limitación conocida de la técnica) y los fondos 2D pintados (montañas, edificios)
  también emiten si son claros (los atenúa `SHAFTS_THRESHOLD`).
- Con MSAA, la máscara usa la muestra 0 de la profundidad y la composición escribe el mismo valor en todas las muestras
  (el target queda "resuelto" dentro del rect; es lo que se vería igual tras el resolve).
- Bajo (una pasada de rayos con ruido) puede mostrar un ruido fino en los rayos al ampliarlos a resolución completa.
- La viñeta está centrada en el rect de cada escena; en VR probablemente convenga desactivarla.
- Validación hecha: todos los shaders compilan a SPIR-V y DXIL con el DXC de RT64 (offsets de push constants
  verificados contra las estructuras de C++) y `rt64_post_effects.cpp` pasa `rt64_compile_check.bat`. Falta: build
  completo, validación de Vulkan/D3D12 en ejecución y medición de tiempos en el juego.
