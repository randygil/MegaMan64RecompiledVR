# Iluminación raster mejorada (RT64)

Alternativa barata al path tracer: sombras del sol, oclusión ambiental, luz de cielo y relighting con técnicas raster
clásicas. Funciona en cualquier GPU (D3D12, Vulkan, Metal) y no depende del juego: solo usa matrices de la
proyección, las luces que estima el `State` y la base del mundo que entrega el host.

Menú: **Gráficos → Enhanced Lighting (Off/On, F3)** y **Lighting Quality (Low/Medium/High/Ultra)**
(`graphics.json`: `el_option`, `el_quality_option`). El path tracing, si está activo, la reemplaza.

## Flujo de un frame

Por cada proyección perspectiva con depth buffer (una "escena de iluminación"; en VR hay una por ojo):

1. `FramebufferRenderer::addFramebuffer` clasifica los draw calls:
   - **opacos** (sin alpha blend): se dibujan en su orden; los que escriben profundidad (no decals) son
     *casters* de sombra y van al buffer de normales;
   - **translúcidos** (alpha blend): se postergan;
   - al cerrar la escena se insertan dos marcadores: `Lighting` (antes de los translúcidos) y `PostScene` (después).
   - Una proyección con las mismas matrices que otra ya iluminada en el mismo framebuffer no se ilumina dos veces.
2. `recordFramebuffer`: antes del framebuffer se dibuja el **shadow map** (una vez por frame).
3. Marcador `Lighting` (`submitRasterScene`):
   1. **G-buffer replay** (`LightingGBufferVS/PS`): vuelve a dibujar los opacos con la misma transformación que
      `RasterVS`, sin depth attachment, descartando los píxeles cuya profundidad no coincide con el depth buffer
      (tolerancia como los decals). Guarda normal octaédrica (16:16) y, en follaje, el radio de la esfera.
   2. **AO** (`LightingAOCS` + `LightingAOBlurCS`): GTAO a media resolución, blur separable con pesos de profundidad.
   3. **Composición** (`LightingComposePS`): calcula un factor de luz por píxel y multiplica el color target con
      blending `2 * src * dst` (el factor va dividido por dos y puede aclarar hasta x2). Respeta el alfa (cobertura RDP).
   4. **Cielo procedural** (`LightingSky`, módulo aparte): reemplaza los píxeles de fondo que son cielo.
4. Translúcidos en su orden original.
5. Marcador `PostScene`: **efectos de post** (`PostEffects`, módulo aparte: bloom, god rays, grading, CAS).
6. HUD y 2D (sin tocar).

## Factor de luz (composición)

```
ambiente  = lerp(suelo, cielo, N·up * 0.5 + 0.5) * AO
sol       = colorSol * lerp(1, wrap(N·L), shading) * sombra   (+ translucidez en follaje)
puntual   = colorLinterna * difuso * atenuación              (solo escenas sin sol)
factor    = (ambiente + (sol + puntual) * lerp(1, AO, aoDirecto)) * exposición
factor    = lerp(factor, 1, alfaNiebla)                       (la niebla del juego tapa la luz)
```

Los colores del N64 ya traen la iluminación horneada; el factor está calibrado para que el suelo al sol quede
cerca de x1,1 y en sombra cerca de x0,6.

## Sombras

- Mapa ortográfico único (1024/2048/2048/4096 según preset) que cubre una esfera alrededor del frustum hasta
  `RT64_LIGHT_SHADOW_DISTANCE` (4000 unidades). El radio solo depende del FOV, así que los texels no cambian de tamaño
  al girar; el centro se ajusta a texels en una base **alineada al mundo real** (`worldRight/Up/Forward/Origin` del
  host), así las sombras no titilan. Función portable: `computeStableShadowMatrix` en `rt64_lighting.cpp`.
- Casters dibujados desde el sol con sus posiciones de mundo (`worldPosBuffer`, ahora calculado siempre que la
  iluminación está activa) y el **alpha test del RDP** (`LightingAlpha.hlsli`) para que las hojas proyecten su forma.
  Los opacos contiguos se dibujan juntos. Depth clip desactivado (casters entre el sol y el plano cercano se aplastan).
- Filtro: 1 comparación bilineal (Low), 3x3 (Medium/High), 5x5 (Ultra). Bias por pendiente + normal offset.
- El follaje mueve su búsqueda hacia el sol según el radio del árbol para no sombrearse con su propia tarjeta cruzada.

## Normales

- Vértices con iluminación RSP: su normal. Sin iluminación (Mega Man 64 nunca la usa): **normales suaves** soldadas
  por posición (`RSPSmoothNormalCS`, ángulo `RT64_LIGHT_SMOOTH_NORMALS` = 75°, draw calls de hasta 256/1024 triángulos
  según preset; el costo es O(n²) por draw call). Si no, la normal de la cara.
- **Follaje** (recortes sin luz con textura): *sphere impostor*. El centro de cada tarjeta es el punto medio de la
  arista más larga del triángulo (la diagonal del quad); la normal es la de la esfera donde entra el rayo de la cámara,
  así las dos tarjetas cruzadas de un árbol comparten la misma normal en cada píxel y el árbol se ve como un volumen.
  Requiere `SV_PrimitiveID` (capacidad de geometry shaders; si falta, el follaje usa normales de cara).

## Parámetros (`RT64_LIGHT_*`, editables en vivo con `RT64_RT_TUNING_FILE`)

| Nombre | Defecto | Efecto |
|---|---|---|
| `ENABLE` | 1 | Apaga todo (para comparar en la misma ejecución). |
| `QUALITY` | -1 | Fuerza un preset 0-3 (−1 = el del menú). |
| `STRENGTH`, `EXPOSURE` | 1, 1 | Mezcla con el original y exposición del factor. |
| `SUN` | 0,9 | Intensidad del sol (tinte del sol estimado normalizado). |
| `SKY_R/G/B`, `GROUND_R/G/B` | 0,62/0,66/0,74, 0,50/0,47/0,42 | Ambiente de cielo y de suelo (exteriores). |
| `INDOOR_AMBIENT`, `INDOOR_GROUND`, `POINT` | 0,95, 0,85, 0,35 | Interiores: ambiente y linterna cerca de la cámara. |
| `WRAP`, `SHADING` | 0,5, 1 | Wrap del difuso y cuánto modulan las normales al sol. |
| `SHADOW_SIZE`, `SHADOW_DISTANCE`, `SHADOW_CASTER_DISTANCE` | por preset, 4000, 6000 | Tamaño y cobertura del shadow map. |
| `SHADOW_STRENGTH`, `SHADOW_BIAS`, `SHADOW_NORMAL_OFFSET`, `SHADOW_SOFTNESS` | 1, 1, 1,5, 1 | Sombras (bias y offset en texels). |
| `FOLIAGE_WRAP`, `FOLIAGE_TRANSLUCENCY`, `FOLIAGE_SHADOW`, `FOLIAGE_SHADOW_OFFSET` | 0,8, 0,6, 0,35, 1 | Follaje. |
| `AO_RADIUS`, `AO_STRENGTH`, `AO_POWER`, `AO_SLICES`, `AO_DIRECT`, `AO_FOLIAGE` | 160, 0,9, 1,5, por preset, 0,35, 0,3 | Oclusión ambiental. |
| `GBUFFER`, `SMOOTH_NORMALS`, `SMOOTH_NORMALS_MAX` | 1, 75, por preset | Buffer de normales y suavizado. |
| `BACKGROUND_DEPTH` | 0,99995 | Profundidad desde la que un píxel es fondo. |
| `DEBUG` | 0 | Vistas: 1 factor, 2 normales, 3 sombra, 4 posición, 5 niebla, 6 AO. |
| `PRINT` (variable de entorno) | — | Imprime cada 120 frames las escenas (rect, sol, cámara, casters, texel). |

`RT64_LIGHTING=1/0` fuerza la iluminación ignorando el menú (para pruebas).

## Costo medido (RTX 4070 SUPER, 1600x960, MSAA 4x, 165 Hz con interpolación)

Base sin iluminación ≈ 0,88 ms/frame. Low +0,67 ms, Medium +1,26, High +1,42, Ultra +2,6. Desglose en Medium:
shadow map ≈ 0,45, normales suaves ≈ 0,33, G-buffer ≈ 0,13, AO ≈ 0,08, resto (composición, posiciones de mundo,
CPU) ≈ 0,3. El path tracer completo costaba ~9,8 ms.

## Portabilidad

- Genérico: shaders `Lighting*.hlsl/.hlsli`, `rt64_lighting_params.h`, `computeStableShadowMatrix`, la composición,
  el AO y el *sphere impostor*. Entradas: depth buffer, matrices (`viewProj`, `invViewProj`, mapeo píxel→clip),
  buffers de posición/normal/índices por vértice y la base del mundo.
- Específico de RT64: los *replays* usan los descriptor sets y el alpha test del RDP (`LightingAlpha.hlsli`).
  En otro renderer (p. ej. un port de PS1) basta con dibujar los opacos con su propio shader de alpha test.
- Específico del juego: nada en RT64. El host de MM64 entrega la rotación/traslación de la cámara y la clave de área.

## Limitaciones conocidas

- Solo proyectan sombra los objetos que el juego dibuja (lo que queda fuera de cámara no existe para el HLE).
- La niebla del juego se aproxima por píxel con los parámetros del primer draw call con niebla.
- En bordes con MSAA el factor es por píxel (leve halo).
- Con el menú del recomp abierto (juego en pausa) aparece una silueta negra dentada junto a Mega Man; desaparece al
  cerrar el menú. Pendiente de investigar (probablemente pesos de interpolación sin frames nuevos).
- El follaje se detecta por heurística (recorte + sin luz + textura); rejas o carteles recortados también reciben
  normales esféricas.

## Pruebas

`C:\Users\Usuario\Devel\tools\light_run.ps1` (arranca con la iluminación y captura) y `light_tune.ps1 -Sets
"label|NOMBRE valor;..."` (una ejecución, varios sets de tuning en vivo, captura y tiempo de frame por set).
Ejemplo: `& light_tune.ps1 -Sets @("low|RT64_LIGHT_QUALITY 0","off|RT64_LIGHT_ENABLE 0")`.
Desde la herramienta PowerShell hay que llamarlos con `&` (no con `powershell -File`, que no pasa arreglos).
