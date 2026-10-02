# Iluminación raster mejorada (RT64)

Alternativa barata al path tracer: sombras del sol, oclusión ambiental, luz de cielo y relighting con técnicas raster
clásicas. Funciona en cualquier GPU (D3D12, Vulkan, Metal) y no depende del juego: solo usa matrices de la
proyección, las luces que estima el `State` y la base del mundo que entrega el host.

Menú: **Gráficos → Enhanced Lighting (Off/On, F3)** y **Lighting Quality (Low/Medium/High/Ultra)**
(`graphics.json`: `el_option`, `el_quality_option`). El path tracing, si está activo, la reemplaza. Las opciones
**Effects** (Off/Subtle/Full: intensidad del post) y **Sky** (Enhanced/Original: cielo procedural) son compartidas
con el path tracer.

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
   2. **AO y sombras de contacto** (`LightingAOCS` + `LightingAOBlurCS`): GTAO a media resolución y, en el mismo pase,
      un rayo corto (120 unidades, 8/12/16 pasos según preset) hacia la luz por el depth buffer: si pasa por detrás
      de una superficie visible de menos de 30 unidades de grosor, el píxel queda a la sombra. Blur separable con
      pesos de profundidad (más estrecho para el contacto). Textura RGBA16F: x AO, y distancia, z contacto.
   3. **Composición** (`LightingComposePS`): calcula un factor de luz por píxel y multiplica el color target con
      blending `2 * src * dst` (el factor va dividido por dos y puede aclarar hasta x2). Respeta el alfa (cobertura RDP).
      Con MSAA son dos pasadas: la superficie más cercana de cada píxel y luego la más lejana de los píxeles de borde,
      cada una escribiendo solo sus muestras (`SV_Coverage`); la lejana toma la normal de un vecino que la muestre.
   4. **Cielo procedural** (`LightingSky`, módulo aparte, `procedural-sky.md`): reemplaza los píxeles de fondo que son cielo.
4. Translúcidos en su orden original.
5. Marcador `PostScene`: **efectos de post** (`PostEffects`, módulo aparte, `post-effects.md`: bloom, rayos de luz,
   grading, CAS, viñeta, dither).
6. HUD y 2D (sin tocar).

Los *replays* (shadow map y G-buffer) dibujan juntos los draw calls consecutivos en el index buffer: un buffer por
triángulo (24 bits bajos = render index, 8 altos = flags `LIGHTING_GBUFFER_*`, descriptor set 4) le dice al shader
`MERGED` a qué draw call pertenece cada primitiva (`SV_PrimitiveID`). Así son unas pocas llamadas en vez de ~1000.

**Recortes con antialiasing** (`RT64_LIGHT_CUTOUT_AA`, activo con la iluminación): con MSAA, el alpha test del RDP en
`RasterPS` escribe `SV_Coverage` según el alfa (afinado con `fwidth`), como *alpha to coverage*: los bordes de las hojas
dejan de verse dentados.

## Factor de luz (composición)

```
ambiente  = lerp(suelo, cielo, N·up * 0.5 + 0.5) * AO
sol       = colorSol * lerp(1, wrap(N·L), shading) * min(sombra, contacto)   (+ translucidez en follaje)
puntual   = colorLinterna * difuso * (1 - d/radio)^caída * contacto          (solo escenas sin sol)
factor    = (ambiente + (sol + puntual) * lerp(1, AO, aoDirecto)) * exposición
factor    = lerp(factor, 1, alfaNiebla)                       (la niebla del juego tapa la luz)
```

Los colores del N64 ya traen la iluminación horneada; el factor está calibrado para que el suelo al sol quede
cerca de x1,1 y en sombra cerca de x0,6.

**Interiores y dungeons** (sin sol): ambiente 0,72 algo frío (tinte 0,94/1,0/1,08) y la linterna cálida que lleva la
cámara (450 arriba, 550 adelante, la pone el `State`) con fuerza 0,9, radio x0,75 del del path tracer y caída
cuadrática: un charco de luz alrededor del jugador y salas que se oscurecen lejos de él, en vez de aclararlo todo
por igual (el ajuste anterior, ambiente 0,85 + linterna 0,5, daba un factor casi constante de ~1,14 y lavaba las
dungeons). La linterna no tiene shadow map (podría quedar por encima del techo en salas bajas); sus sombras de
contacto solo se aplican a los suelos: en las paredes junto a la cámara, vistas casi de canto, el rayo en pantalla
daba franjas negras.

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
- El sol sale de `GameConfiguration` (azimut 35°, elevación 32°, `RT64_RT_SUN_AZIMUTH/ELEVATION`). Con el sol bajo las
  sombras de los edificios cubren calles enteras: es correcto, no un bug (ver la vista de depuración 7).

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
| `INDOOR_AMBIENT`, `INDOOR_GROUND`, `INDOOR_TINT_R/G/B` | 0,72, 0,85, 0,94/1,0/1,08 | Interiores: ambiente, fracción para superficies que miran abajo y tinte. |
| `POINT`, `POINT_RADIUS`, `POINT_FALLOFF` | 0,9, 0,75, 2 | Linterna de interiores: fuerza, escala del radio (el del `State` es 3000) y exponente de la caída. |
| `CONTACT_LENGTH`, `CONTACT_THICKNESS`, `CONTACT_STRENGTH`, `CONTACT_STEPS` | 120, 30, 1, 0/8/12/16 | Sombras de contacto (unidades del juego, 1 ≈ 1 cm). |
| `WRAP`, `SHADING` | 0,5, 1 | Wrap del difuso y cuánto modulan las normales al sol. |
| `SHADOW_SIZE`, `SHADOW_DISTANCE`, `SHADOW_CASTER_DISTANCE` | por preset, 4000, 6000 | Tamaño y cobertura del shadow map. |
| `SHADOW_STRENGTH`, `SHADOW_BIAS`, `SHADOW_NORMAL_OFFSET`, `SHADOW_SOFTNESS` | 1, 1, 1,5, 1 | Sombras (bias y offset en texels). |
| `FOLIAGE_WRAP`, `FOLIAGE_TRANSLUCENCY`, `FOLIAGE_SHADOW`, `FOLIAGE_SHADOW_OFFSET` | 0,8, 0,6, 0,35, 1 | Follaje. |
| `AO_RADIUS`, `AO_STRENGTH`, `AO_POWER`, `AO_SLICES`, `AO_DIRECT`, `AO_FOLIAGE` | 120, 0,8, 1,3, por preset, 0,35, 0,3 | Oclusión ambiental (antes 160/0,9/1,5: oscurecía de más los marcos y esquinas junto a la cámara). Ignora capas a menos de 4 unidades sobre la superficie (carteles, decals como geometría propia). |
| `GBUFFER`, `SMOOTH_NORMALS`, `SMOOTH_NORMALS_MAX` | 1, 75, por preset | Buffer de normales y suavizado. |
| `MERGE_DRAWS` | 1 | Dibuja juntos los draw calls consecutivos en los *replays*. |
| `CUTOUT_AA` | 1 | Alpha to coverage de los recortes con MSAA. |
| `BUMP` | 0 | Relieve desde el brillo de la textura (apagado: en MM64 las texturas del terreno tienen ventanas por quad y salía una cuadrícula). |
| `BACKGROUND_DEPTH` | 0,99995 | Profundidad desde la que un píxel es fondo. |
| `DEBUG` | 0 | Vistas: 1 factor, 2 normales (espacio de la geometría), 3 sombra (incluye el contacto), 4 posición, 5 niebla, 6 AO, 7 distancia al oclusor del shadow map (rojo delante hasta 200 u, verde detrás, azul = normal guardada), 8 sombras de contacto. |
| `PRINT` (variable de entorno) | — | Imprime cada 120 frames las escenas (rect, sol, cámara, casters, texel). |

`RT64_LIGHTING=1/0` fuerza la iluminación ignorando el menú (para pruebas).

## Costo medido (RTX 4070 SUPER, 1600x960, MSAA 4x, 165 Hz con interpolación)

Base sin iluminación ≈ 0,88 ms/frame. Low +0,67 ms, Medium +1,26, High +1,42, Ultra +2,6. Desglose en Medium:
shadow map ≈ 0,45, normales suaves ≈ 0,33, G-buffer ≈ 0,13, AO ≈ 0,08, resto (composición, posiciones de mundo,
CPU) ≈ 0,3. El path tracer completo costaba ~9,8 ms.

Con todo (iluminación Medium + cielo + post, *replays* fusionados): bosque ≈ 2,45 ms de frame y 1,6 ms de GPU;
calle de la ciudad ≈ 3,0 ms y 1,95 ms de GPU; sin iluminación ≈ 1,57 / 0,93 ms. A 165 Hz no baja de 165 FPS
(el path tracer daba ~81 FPS).

GPU por pase (`RT64_PRINT_FRAME_TIME=2`, ms por frame, bosque, Medium):

| Pase | Escritorio 1600x960 MSAA 4x | VR debug, 2 ojos de 640x960 | VR, preset Low |
|---|---|---|---|
| normales suaves | 0,20 | 0,75 | — |
| shadow map | 0,30 | 0,55 (0,83 antes de quitar los casters repetidos del otro ojo) | 0,25 |
| raster del juego | 0,13 | 0,51 | 0,53 |
| G-buffer | 0,16 | 0,58 | ~0,3 |
| AO | 0,16 | 0,42 | — |
| composición | 0,30–0,37 | 0,37 | 0,5 |
| cielo | 0,11 | 0,47 | 0,43 |
| post | 0,14 | 0,52 | 0,41 |

En VR todo se hace dos veces (una escena por ojo) y el campo de visión ancho mete más geometría. Lo más caro con MSAA
es leer el depth buffer multisample (composición, cielo). Para el Quest el preset Low sigue siendo pesado: ver
"Plataformas móviles" abajo.

## VR

- Cada ojo es una escena de iluminación (su propia proyección y mitad del framebuffer); G-buffer, AO, composición,
  cielo y post corren por ojo. El shadow map se hace una vez con los casters del primer ojo: el otro dibuja la misma
  geometría con la misma matriz de modelo, así que sus casters se descartan (`Caster::sceneIndex`).
- El modo VR quita el cielo 2D del juego (marea en el casco). El parche lo informa al host y este a RT64 con
  `setSkyBackgroundHint`, así que las áreas exteriores tienen sol, sombras y el cielo procedural (fijo en el mundo,
  cómodo en VR) en vez de un fondo plano del color de la niebla.
- El FOV ancho agranda la esfera del shadow map (texel ~5,9 unidades contra ~2,8 en escritorio): las sombras se ven
  más blandas. Unas cascadas lo resolverían.

## Preset Low

Shadow map de 1024 con una sola comparación, sin AO ni sombras de contacto, sin normales suaves, composición de una
sola pasada con MSAA (la superficie más cercana para todas las muestras del píxel) y sin rayos de luz; bloom de 4
niveles y nubes del cielo con 3 octavas. En escritorio cuesta ~0,7 ms de GPU sobre el juego (Medium ~1,3 ms).
El G-buffer se mantiene completo: se probó dibujar solo el follaje y sacar las normales del resto de la profundidad,
pero el depth buffer guarda la profundidad cuantizada como el N64 y en suelos vistos en ángulo rasante las normales
salían con escalones (líneas diagonales negras en el terreno).

## Plataformas móviles (Quest/Android)

En Android la iluminación arranca apagada y, si se enciende, en Low (`src/game/config.cpp`). El build nativo de
Android (arm64 + VR) compila con todo esto (`android/build_native.sh`, `JOBS=8` para no quedarse sin memoria).

Sin medir en el casco. Por los números de escritorio, el preset Low en VR cuesta ~2,2 ms de una RTX 4070 SUPER, lo que
en un Adreno 650 sería del orden de decenas de ms: demasiado. Ideas para un preset móvil: sin G-buffer (normales de la
profundidad; el follaje pierde el volumen), composición de una pasada sin el tratamiento por superficie del MSAA,
cielo a un cuarto de resolución, post reducido a grading (sin bloom ni rayos), shadow map de 1024 solo cada dos frames.

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
- En bordes con MSAA se ilumina cada superficie por separado; quedan líneas muy tenues donde una superficie lejana no
  tiene vecinos que la muestren (toma la normal de la profundidad).
- Con el menú del recomp abierto aparecen líneas negras dentadas en los bordes del camino de tierra. **No es de la
  iluminación** (pasa igual con ella apagada): el fondo del menú deja ver el canal alfa del color target, que guarda la
  cobertura del RDP en los bordes de las texturas.
- El follaje se detecta por heurística (recorte + sin luz + textura); rejas o carteles recortados también reciben
  normales esféricas.

## Pruebas

`C:\Users\Usuario\Devel\tools\light_run.ps1` (arranca con la iluminación y captura) y `light_tune.ps1 -Sets
"label|NOMBRE valor;..."` (una ejecución, varios sets de tuning en vivo, captura y tiempo de frame por set).
Ejemplo: `& light_tune.ps1 -Sets @("low|RT64_LIGHT_QUALITY 0","off|RT64_LIGHT_ENABLE 0")`.
Desde la herramienta PowerShell hay que llamarlos con `&` (no con `powershell -File`, que no pasa arreglos).
