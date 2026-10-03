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
      `RasterVS`, descartando los píxeles cuya profundidad no coincide con el depth buffer (tolerancia como los
      decals). Guarda normal octaédrica (16:16) y, en follaje, el radio de la esfera. Tiene su propio depth buffer
      (D32, `LESS_EQUAL`): donde dos superficies que se cruzan pasan las dos la tolerancia, gana la más cercana y no la
      última dibujada (sin eso quedaban líneas oscuras en los cruces, p. ej. tarjeta de árbol con su núcleo 3D).
   2. **AO y sombras de contacto** (`LightingAOCS` + `LightingAOBlurCS`): GTAO a media resolución y, en el mismo pase,
      un rayo corto (120 unidades, 8/12/16 pasos según preset) hacia la luz por el depth buffer: si pasa por detrás
      de una superficie visible de menos de 30 unidades de grosor, el píxel queda a la sombra. Blur separable con
      pesos de profundidad (más estrecho para el contacto). Textura RGBA16F: x AO, y distancia, z contacto.
   3. **Composición** (`LightingComposePS`): calcula un factor de luz por píxel y multiplica el color target con
      blending `2 * src * dst` (el factor va dividido por dos y puede aclarar hasta x2). Respeta el alfa (cobertura RDP).
      Con MSAA son dos pasadas: la superficie más cercana de cada píxel y luego la más lejana de los píxeles de borde,
      cada una escribiendo solo sus muestras (`SV_Coverage`); la lejana toma la normal de un vecino que la muestre.
      También la toma la cercana cuando el G-buffer no la guardó (el borde antialiasado de un recorte: el raster
      conservó su cobertura pero el G-buffer rechazó su alfa); la normal sacada de la profundidad mezclaría las dos
      superficies y dejaría una línea oscura en el borde.
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
sol       = colorSol * lerp(1, wrap(N·L), shading) * min(sombra, contacto) * nubes   (+ translucidez en follaje)
puntual   = colorLinterna * difuso * (1 - d/radio)^caída * contacto * sombraPuntual   (solo escenas sin sol)
factor    = (ambiente + (sol + puntual) * lerp(1, AO, aoDirecto)) * exposición
factor    = lerp(factor, 1, alfaNiebla)                       (la niebla del juego tapa la luz)
```

Los colores del N64 ya traen la iluminación horneada; el factor está calibrado para que el suelo al sol quede
cerca de x1,1 y en sombra cerca de x0,6.

**Tinte del cielo**: cuando el cielo del juego no es diurno (atardecer, cielo violeta), la luz del cielo y el sol toman
su tono: la composición lee el color medio que mide el análisis del cielo procedural (`LightingSkyAnalyzeCS`, un frame
de retraso, buffer `t7`) y tiñe con su croma (`RT64_LIGHT_SKY_TINT` = 0,8 × lo poco diurno que sea el tono). Sin esto,
el interior frente al atardecer del área 21 quedaba iluminado con el ambiente azulado de un cielo de día. Requiere la
opción Sky en Enhanced (es el cielo procedural el que mide); en VR no se aplica.

**Interiores y dungeons** (sin sol): ambiente 0,72 algo frío (tinte 0,94/1,0/1,08) y la linterna cálida que pone el
`State`: 350 unidades sobre el jugador y 200 hacia la cámara si el host da su posición (`Application::setFocusPosition`;
el host de MM64 la lee del actor de Mega Man en `0x802049B0`, +0x14 s16 x3, y la pasa al espacio de la geometría con la
matriz de vista); si no la da o está a más de 2500 unidades de la cámara, 450 arriba y 550 adelante de la cámara con fuerza 0,9, radio x0,75 del del path tracer y caída
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

## Sombras estables al mover la cámara (geometría fuera de vista)

Problema (video del usuario, 2026-10-02): al girar o mover la cámara aparecían y desaparecían sombras grandes. El mapa
de sombras es estable; lo que cambiaba eran los **casters**: el HLE solo ve lo que el juego dibuja, y los juegos
dibujan solo lo que puede verse. Todo lo que el juego descarta por estar detrás o al costado de la cámara, o por tapar
la vista, deja de proyectar sombra justo cuando su sombra sí se ve. Es un problema general de cualquier port con
sombras modernas: hay que pedirle al juego que dibuje un poco más mientras la iluminación lo necesita.

Lo que hace MM64 y cómo se corrige (`patches/offscreen_geometry.c` + hooks en `us.rev1.toml`, solo con la iluminación
mejorada o el trazado de rayos encendidos; `MM64_OFFSCREEN_GEOMETRY 0` en el archivo de tuning vuelve al culling del
juego para comparar en vivo):

| Culling del juego | Dónde | Arreglo |
|---|---|---|
| Ventana de celdas de terreno (512 u) centrada una distancia de dibujo **delante** de la cámara: detrás solo cubre 0.2 de esa distancia | `func_80038934` (ventana en `0x8017B220..2C`) | La ventana también incluye ±0x1000 u alrededor de la cámara, antes de que las zonas del mapa la recorten |
| Culling por celda: centro a más de 0x600 u detrás → fuera; cerca → sin prueba de pantalla | `func_8003912C` | Rango 0x1000 (instrucciones en `us.rev1.toml`) |
| Utilería (carpas, furgoneta, cajas): ventana + profundidad < 1 + centro proyectado fuera de pantalla | `func_8003AB60`, `func_8003AFB0` | Se dibuja si su profundidad está en ±0x1000 |
| Personajes: igual, pero dibujarse los marca como vistos (byte 6, bit 0x80; lo lee su comportamiento y el fijado de blanco) | `func_8003A2EC` | Se dibujan igual y se les quita la marca enseguida si el juego no los habría dibujado |
| Árboles/arbustos (registros en modo de desvanecido 2): un recorredor en CPU descarta cada quad con una esquina más cerca que un umbral (`0x801FFBB8`), incluidos todos los que están detrás | `func_8007D798` | Los quads enteros detrás de la cámara se dibujan; los cercanos se emiten **solo como sombra** |
| Paredes y utilería grande (modo 3): oculta los quads a menos de 0xC0 u y los cercanos que tapan el centro de la pantalla (donde está Mega Man) | `func_8007DA78` | Igual: detrás se dibujan, los ocultos se emiten solo como sombra |

**Geometría solo de sombra (RT64, genérico):** comando extendido `gEXSetShadowOnly(cmd, 1/0)`
(`G_EX_SETSHADOWONLY_V1`). Los triángulos dibujados con la marca entran como casters del mapa de sombras pero no van
al raster, ni al G-buffer, ni a la escena de trazado de rayos; sin la iluminación no se dibujan. Sirve para cualquier
juego que esconda geometría para despejar la cámara: el juego la sigue escondiendo y su sombra no cambia.

Medido (RTX 4070 SUPER, Ultra, 1600x960, vista del guardado): GPU 2.95 → 3.03 ms; casters 1035 → 1089. Lo que más sube
son las normales suaves (0.15 → 0.27 ms), que se calculan también para lo que quedó fuera de vista. Prueba: la
caminata hacia atrás junto a la carpa (`walk_bisect.ps1`) tenía 4 saltos de sombra; ahora 0.

Cómo se encontró (útil para otros juegos): grabar con `motion_rec.ps1` la vista de sombras sola mientras se repite el
mismo movimiento, detectar saltos con `pop_scan.py`, y repetirlo quitando casters por tipo de etiqueta y por celda del
terreno (tuning temporal de depuración) hasta dar con la celda; luego contar sus draw calls por frame. Con eso se vio
que el contenido de una celda cambiaba sin que la celda desapareciera: eran los recorredores de quads.

Queda: casters muy altos más allá de la distancia de dibujo del juego (al frente, hacia el sol) siguen dependiendo de
su ventana; y con el trazado de rayos la geometría solo de sombra no entra en la escena de rayos (habría que añadirla
con una máscara que solo vean los rayos de sombra), así que allí esas sombras siguen dependiendo del juego.

## Normales

- Vértices con iluminación RSP: su normal. Sin iluminación (Mega Man 64 nunca la usa): **normales suaves** soldadas
  por posición (`RSPSmoothNormalCS`, ángulo `RT64_LIGHT_SMOOTH_NORMALS` = 75°, draw calls de hasta 512/1024 triángulos
  según preset; Medium subió de 256 a 512 para los núcleos 3D de los árboles). Sigue siendo O(n²) por draw call, pero recorre los triángulos en bloques de 64 cargados en memoria
  compartida y todas las draw calls del frame van en un solo dispatch (tabla de grupos: rango, primer triángulo,
  pliegue): 0,22 → 0,08 ms en el bosque. Si no, la normal de la cara.
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
| `POINT_SHADOW`, `POINT_SHADOW_SIZE`, `POINT_SHADOW_HEIGHT`, `POINT_SHADOW_NEAR` | 1, 0/512/768/1024, 110, 80 | Sombras de la linterna (ver abajo): fuerza, lado de cada cara del cubo por preset (Low sin sombras), altura del origen sobre los pies del jugador y plano cercano (lo que está más cerca, como el propio jugador, no proyecta). |
| `POINT_SHADOW_NORMAL_OFFSET`, `POINT_SHADOW_BIAS`, `POINT_SHADOW_SOFTNESS` | 1,5, 1,5, 1 | Desplazamiento por la normal y sesgo (en texels a la distancia del píxel) y suavidad del PCF 3x3. |
| `POINT_SHADOW_ORIGIN`, `POINT_OFFSET_X/Y/Z` | 0, 0 | Desarrollo: 1 dibuja las sombras desde la luz aunque no se conozca al jugador, y la luz se puede mover por los ejes del mundo (para ver las sombras desde una cámara fija de los warps). |
| `CONTACT_LENGTH`, `CONTACT_THICKNESS`, `CONTACT_STRENGTH`, `CONTACT_STEPS` | 120, 30, 1, 0/8/12/16 | Sombras de contacto (unidades del juego, 1 ≈ 1 cm). |
| `WRAP`, `SHADING` | 0,5, 1 | Wrap del difuso y cuánto modulan las normales al sol. |
| `SHADOW_SIZE`, `SHADOW_DISTANCE`, `SHADOW_CASTER_DISTANCE` | por preset, 4000, 6000 | Tamaño y cobertura del shadow map. |
| `SHADOW_STRENGTH`, `SHADOW_BIAS`, `SHADOW_NORMAL_OFFSET`, `SHADOW_SOFTNESS` | 1, 1, 1,5, 1 | Sombras (bias y offset en texels; la suavidad fija solo se usa sin PCSS). |
| `SHADOW_SUN_SIZE`, `SHADOW_MIN_SOFTNESS`, `SHADOW_MAX_SOFTNESS` | 0,025, 0,75, 5/6/8 | Penumbra variable (PCSS, de Medium en adelante; 0 vuelve a la grilla fija): radio angular del sol en radianes y suavidad mínima/máxima en texels. Búsqueda de oclusores con 8/12/16 lecturas en espiral de ángulo áureo, la distancia media al oclusor por la tangente del sol da el ancho de la penumbra y el filtro usa 12/16/24 comparaciones; si no encuentra oclusores no filtra. Las sombras quedan nítidas junto a lo que las proyecta y más suaves lejos (la punta de la sombra de un árbol); cuesta lo mismo que la grilla fija en el bosque y ~0,05 ms más en la calle. |
| `FOLIAGE_WRAP`, `FOLIAGE_TRANSLUCENCY`, `FOLIAGE_SHADOW`, `FOLIAGE_SHADOW_OFFSET` | 0,8, 0,6, 0,35, 1 | Follaje. |
| `AO_RADIUS`, `AO_STRENGTH`, `AO_POWER`, `AO_SLICES`, `AO_DIRECT`, `AO_FOLIAGE` | 120, 0,8, 1,3, por preset, 0,35, 0,3 | Oclusión ambiental (antes 160/0,9/1,5: oscurecía de más los marcos y esquinas junto a la cámara). Ignora capas a menos de `AO_MIN_HEIGHT` = 12 unidades sobre la superficie (carteles, pósters, decals como geometría propia): con 4, el borde de un póster de Apple Market, visto casi de canto, dejaba una franja oscura en la pared. |
| `GBUFFER`, `SMOOTH_NORMALS`, `SMOOTH_NORMALS_MAX` | 1, 75, por preset | Buffer de normales y suavizado. |
| `MERGE_DRAWS` | 1 | Dibuja juntos los draw calls consecutivos en los *replays*. |
| `CUTOUT_AA` | 1 | Alpha to coverage de los recortes con MSAA. |
| `BUMP` | 0 | Relieve desde el brillo de la textura (apagado: en MM64 las texturas del terreno tienen ventanas por quad y salía una cuadrícula). |
| `BACKGROUND_DEPTH` | 0,99995 | Profundidad desde la que un píxel es fondo. |
| `EMISSIVE_MAX`, `EMISSIVE`, `EMISSIVE_LIGHT`, `EMISSIVE_THRESHOLD`, `EMISSIVE_RADIUS`, `EMISSIVE_QUALITY` | 0 (apagado), 0,35, 10, 0,65, 24, 2 | Superficies que brillan en interiores (experimental, ver abajo): luz máxima que suman, brillo propio, fuerza de la luz que proyectan, canal más brillante desde el que una superficie colorida brilla, radio del desenfoque (píxeles a ¼ de resolución) y preset mínimo. |
| `DEBUG` | 0 | Vistas: 1 factor, 2 normales (espacio de la geometría), 3 sombra (incluye el contacto y las nubes), 4 posición, 5 niebla, 6 AO, 7 distancia al oclusor del shadow map (rojo delante hasta 200 u, verde detrás, azul = normal guardada), 8 sombras de contacto, 9 luz de las superficies que brillan (rojo = brillo propio), 10 sombra de la linterna, 11 el atlas de la linterna tal cual (un texel por píxel desde la esquina). |
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
- El FOV ancho agrandaba la esfera del shadow map (texel ~5,9 unidades contra ~2,8 en escritorio). Ahora el radio se
  acota (`RT64_LIGHT_SHADOW_MAX_RADIUS` = 3000): con FOV ancho se acorta la distancia de las sombras (~2000 unidades en
  VR) y el texel queda en ~2,9. Unas cascadas darían ambas cosas a cambio de dibujar los casters dos veces.

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

## Sombras de la linterna de interiores

En escenas sin sol, la linterna que lleva el jugador proyecta sombras en todas las direcciones: un cubo de 6 caras de 90°
en un atlas de 3x2 (`pointShadowMap`, D32), dibujado con los mismos *casters* y *replays* fusionados que el shadow map
del sol (`recordShadowMap` dibuja uno u otro). La composición elige la cara por el eje dominante de
`posición − origen` (`lightingPointShadowFace`, con los mismos ejes que `computePointShadowFace` en el CPU), calcula
la profundidad como `a + b / z` y hace un PCF 3x3 que no sale de la cara. Cuesta ~0,03–0,07 ms en Apple Market con
caras de 512.

- **Origen**: el pecho del jugador (`focusPosition` del host + 110 hacia arriba), no la luz: la linterna flota 350 sobre
  él y en un techo bajo quedaría encima del techo, que dejaba toda la sala a la sombra (se vio al mover la luz con
  `POINT_OFFSET_Y` en la casa de Roll). Las sombras salen un poco más bajas que la luz que ilumina; no se nota.
- **Plano cercano de 80 con recorte de profundidad** (pipelines `shadow*PointPipeline`, a diferencia del sol que aplasta
  los *casters* contra su plano cercano): el cuerpo del jugador, alrededor del origen, no tapa nada. Recortar en vez de
  aplastar también bajó el costo de ~0,15 a ~0,05 ms (lo que estaba detrás del plano cercano ocupaba las caras).
- Sin la posición del jugador (warps de cámara fija, cinemáticas) no hay sombras: la luz se coloca solo desde la cámara.
  En VR las dos escenas (ojos) usan el mismo atlas, dibujado desde el foco de la primera.
- Low no las tiene; Medium 512, High 768 y Ultra 1024 por cara (el atlas mide 3 × 2 caras).

## Superficies que brillan en interiores (experimental, apagado)

Como el path tracer (que en escenas sin sol hace emisivas las superficies con luminancia > 0,45), las escenas sin sol
pueden tener superficies que brillan y proyectan luz de su color: `LightingEmissiveCS` toma la copia del color target
(antes de la iluminación), promedia en bloques de 4x4 el color de los píxeles que parecen luces
(`lightingEmissiveMask`: coloridos con el canal más brillante ≥ `EMISSIVE_THRESHOLD` y luminancia ≥ 0,35, o casi
blancos), `LightingEmissiveBlurCS` lo desenfoca (gaussiana separable de radio 24 a ¼ de resolución, las muestras fuera
de la pantalla cuentan como oscuridad) y la composición suma esa luz (saturada en `EMISSIVE_MAX`) y un brillo propio en
los píxeles que brillan. Corre al principio de `recordCompose` (marcadores de GPU `emissive copy` y `emissive`).

Probado en Apple Market (4,0) y las dungeons 14,1, 20,0 y 26,0: cuesta 0,18–0,25 ms (copia con resolve del MSAA
~0,06–0,09 ms, los tres pases chicos y sus barreras ~0,12–0,17 ms) y aporta poco: las lámparas de las dungeons son
pocas, chicas y no llegan al tope del rango (las naranjas de 20,0 valen (0,72, 0,45, 0,09) antes de la iluminación),
mientras que los estandartes rosados de Apple Market sí pasan el umbral y brillaban como neón. Solo con colores no se
distinguen; haría falta marcar las texturas emisivas por hash (E5 de `remake-research.md`). Por eso está apagado
(`EMISSIVE_MAX` = 0) y queda para juegos con luces claras; para probarlo: `RT64_LIGHT_EMISSIVE_MAX 0.6` y
`RT64_LIGHT_EMISSIVE_QUALITY 1` en el archivo de tuning.

## Ideas de rendimiento evaluadas (sin implementar)

- **Shadow map cada N frames con interpolación**: con 165 Hz RT64 dibuja ~5 frames por frame del juego; la geometría
  interpolada se mueve poco entre frames, así que redibujar el shadow map en frames alternos (reusando su matriz, para
  que la búsqueda siga siendo exacta) ahorraría ~0,15–0,2 ms con un retraso imperceptible. Sin interpolación (30/60
  fps) el retraso sí se notaría en personajes en movimiento: habría que activarlo solo cuando la tasa de render supera
  bastante a la del juego.
- **Pre-pase de profundidad para el G-buffer**: probado, no ganó (ver HANDBOOK, lecciones).

## Portabilidad

- Genérico: shaders `Lighting*.hlsl/.hlsli`, `rt64_lighting_params.h`, `computeStableShadowMatrix`, la composición,
  el AO, el *sphere impostor*, las sombras de las nubes (necesitan la base del mundo y el cielo procedural) y el
  emisivo experimental. Entradas: depth buffer, matrices (`viewProj`, `invViewProj`, mapeo píxel→clip),
  buffers de posición/normal/índices por vértice y la base del mundo.
- Específico de RT64: los *replays* usan los descriptor sets y el alpha test del RDP (`LightingAlpha.hlsli`).
  En otro renderer (p. ej. un port de PS1) basta con dibujar los opacos con su propio shader de alpha test.
- Específico del juego: nada en RT64. El host de MM64 entrega la rotación/traslación de la cámara y la clave de área.

## Limitaciones conocidas

- Solo proyectan sombra los objetos que el juego dibuja. En MM64 el parche de geometría fuera de vista (ver arriba) hace
  que dibuje lo que rodea a la cámara y que lo que oculta para despejar la vista siga como caster solo de sombra.
- La niebla del juego se aproxima por píxel con los parámetros del primer draw call con niebla.
- En bordes con MSAA se ilumina cada superficie por separado. Una superficie lejana que ningún vecino muestra (se ve
  solo por una grieta, como las costuras entre los quads de una pared) conserva su color original: antes tomaba la
  normal de la profundidad y salían puntos oscuros en las costuras.
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
