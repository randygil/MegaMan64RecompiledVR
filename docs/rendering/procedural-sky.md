# Cielo procedural de la iluminación raster mejorada

Módulo de RT64 que reemplaza el cielo pintado del juego (la imagen 2D que muchos juegos de N64 dibujan detrás de la
escena 3D) por una atmósfera física con disco y halo del sol y una capa de nubes anclada al mundo. Es barato, agnóstico
al juego y solo usa datos genéricos: matrices de la proyección, dirección del sol y la base del mundo que entrega el
host.

Documentos relacionados: `HANDBOOK.md` (arquitectura de RT64, pruebas), `techniques-research.md` §7 (investigación de
modelos de cielo y nubes), `enhanced-lighting.md` (resto de la iluminación raster, cuando exista).

---

## 1. Archivos

| Archivo | Contenido |
|---|---|
| `lib/rt64/src/render/rt64_lighting_sky.h/.cpp` | API (`LightingSky`, `LightingSkyDesc`), parámetros en CPU, pipelines y grabación de los pases. |
| `lib/rt64/src/shaders/LightingSkyCommon.hlsli` | Estructuras compartidas CPU/GPU (`LightingSkyParams`, `LightingSkyCB`, `LightingSkyLutCB`) y parametrización de la LUT. Lo incluyen el `.cpp` y los shaders. |
| `lib/rt64/src/shaders/LightingSkyLutPS.hlsl` | Pase de la tabla *sky-view* (atmósfera). |
| `lib/rt64/src/shaders/LightingSkyPS.hlsl` | Pase principal (variante `LightingSkyPSMS` con `-D MULTISAMPLING`). |
| `lib/rt64/src/shaders/LightingSkyClouds.hlsli` | Funciones puras, sin recursos: ruido, fbm filtrado, nubes, sol, *key*, compresión de luces, *dither*. Reutilizables tal cual en otro renderer. |
| `lib/rt64/cmake/lighting_sky_shaders.cmake` | Registro de los shaders y dependencias de `src/shaders/*.hlsli` y `src/shared/*.h`. |

## 2. Cuándo corre y qué pases tiene

`LightingRenderer::recordSky` lo llama después de iluminar las superficies opacas de una escena 3D con sol y antes de
dibujar las translúcidas (agua, partículas) y todo lo 2D (HUD). Antes de llamarlo, `LightingRenderer::copyColor` copia
el color del target (resuelto si hay MSAA) a `sceneColor`. `enabled()` devuelve `RT64_SKY_ENABLE > 0`; si es falso,
no se hace ni la copia.

1. **Tabla sky-view** (`LightingSkyLutPS`, 64×32 `R16G16B16A16_FLOAT`): radiancia del cielo por azimut relativo al
   sol (0–180°, lineal) y elevación (0–90°, ley cuadrática para dar más resolución al horizonte), ya graduada
   (exposición, rango, saturación, tinte). Depende solo de la elevación del sol y de los ajustes de aspecto, así que
   **solo se vuelve a dibujar cuando cambian** (con tolerancia de 1e-4 en la dirección del sol, que varía por redondeo al
   girar la cámara). Con el sol fijo de un área cuesta cero; con un ciclo de día y noche cuesta microsegundos.
2. **Pase principal** (`LightingSkyPS`): triángulo de pantalla completa (`FullScreenVS`) con *scissor* = rectángulo de
   la escena, sobre el framebuffer del color target. Lee profundidad, `sceneColor` y la LUT; escribe solo RGB (el alfa
   guarda la cobertura del RDP) con *blending* `SRC_ALPHA / INV_SRC_ALPHA`, donde el alfa es el *key*. Sin profundidad.
   Si el target es MSAA, el pipeline usa su cantidad de muestras y el shader escribe `SV_Coverage` con las muestras
   que son fondo.

Recursos propios: la LUT y su framebuffer, un *upload buffer* con un anillo de 32 ranuras de `LightingSkyParams`
(224 bytes) y 32 *descriptor sets* (uno por ranura: parámetros, profundidad, `sceneColor`, LUT y un sampler lineal
inmutable). El push constant del pase principal solo lleva el índice de la ranura. El anillo es seguro porque el worker
espera al GPU al final de cada frame (igual que el buffer de luces del path tracer); solo fallaría si un frame dibujara
el cielo más de 32 veces. Los pipelines del pase principal se crean bajo demanda por (muestras, formato).

Estados: el color target llega y queda en `COLOR_WRITE`, la profundidad en `DEPTH_READ`, `sceneColor` en
`SHADER_READ`. La LUT pasa de `SHADER_READ` a `COLOR_WRITE` y de vuelta solo cuando se redibuja.

## 3. Entradas

De `LightingSkyDesc`: `colorTarget`, `depthTarget`, `rect`, `sceneColor`, `time` (segundos) y `lighting`, del que se
usan:

| Campo de `LightingParams` | Uso |
|---|---|
| `invViewProj`, `pixelToClip` | Rayo de vista de cada píxel (ver 4.1). |
| `depthToClip.z` | Umbral de fondo: profundidad ≥ z significa que no se dibujó nada. |
| `sunDirection` (w > 0) | Dirección del sol. Sin sol no se dibuja nada. |
| `sunColor` | Tinte del sol de la escena (parcial, ver 4.3). |
| `worldRight/Up/Forward` | Base del mundo: define el "espacio del cielo" (y hacia arriba). |
| `worldOrigin` (w = 1), `cameraPosition` | Posición de la cámara en el mundo, para el paralaje de las nubes. |
| `groundColor` | Luz rebotada del suelo hacia la base de las nubes (×0,65). |

El shader no lee `LightingParams`: todo pasa por `LightingSkyParams`, calculado en CPU. Por eso un cambio de layout de
`rt64_lighting_params.h` no afecta a estos shaders.

## 4. Modelo

### 4.1 Espacio del cielo y rayos de vista

Todo se expresa en el espacio del cielo: x = `worldRight`, y = `worldUp`, z = `worldForward`. Así el cielo queda fijo
en el mundo al girar la cámara aunque el juego hornee la cámara en las matrices de modelo (como MM64).

Rayo por píxel sin invertir matrices en el shader: los puntos de un píxel son combinaciones del punto homogéneo
A = (ndc.x, ndc.y, 0, 1)·invViewProj y de la cámara C = (0, 0, 1, 0)·invViewProj, así que
`A.xyz·C.w − C.xyz·A.w` apunta a lo largo del rayo y es **afín en la posición del píxel**. La CPU calcula en doble
precisión `rayOrigin + x·rayStepX + y·rayStepY` (orientado hacia delante con un punto a profundidad NDC 0,5, escalado
para que el rayo central mida 1 y pasado al espacio del cielo); el shader solo normaliza. Validado contra la
desproyección de dos profundidades: error < 3e-6° en float. También se calcula el tamaño angular de un píxel, que
usan el antialiasing del disco solar y el filtrado de las nubes. Proyecciones no perspectivas se ignoran.

### 4.2 Atmósfera (tabla sky-view)

Dispersión simple en una atmósfera esférica tipo Tierra (radios 6360/6460 km), observador a 200 m:

- Rayleigh β = (5,802; 13,558; 33,1)·10⁻⁶ m⁻¹, altura de escala 8 km, fase 3/(16π)(1+μ²).
- Mie β = 3,996·10⁻⁶ (extinción 4,44·10⁻⁶) × `RT64_SKY_HAZE`, altura 1,2 km, fase Cornette-Shanks g = 0,8.
- Ozono (absorción (0,650; 1,881; 0,085)·10⁻⁶, perfil triangular en 25 ± 15 km) × `RT64_SKY_OZONE`: azul más profundo.
- 24 pasos por rayo de vista (distribución cuadrática) × 6 pasos hacia el sol; el planeta tapa el sol bajo el horizonte.
- Dispersión múltiple barata: término isotrópico proporcional a la dispersión acumulada en el rayo
  (× `RT64_SKY_MULTI_SCATTERING`) que aclara el lado opuesto al sol.
- Graduación en la propia LUT: exposición (10,8 × `RT64_SKY_EXPOSURE`), compresión del rango cenit/horizonte
  `c·L^(rango−1)` que conserva el tono (el cenit físico es oscuro y poco saturado), saturación alrededor de la luminancia y
  tinte. Los valores por defecto (saturación 2,0, tinte (0,85; 0,97; 1,12), rango 0,75) dan un azul profundo pero
  natural, cercano al estilo vivo de los cielos de N64 sin llegar a recortar canales.

El pase principal hace una sola lectura bilineal por píxel; tres lecturas más en coordenadas fijas (cenit y horizonte
hacia el sol y en contra) dan la luz ambiente de las nubes y el color de la bruma.

### 4.3 Sol

- Color: transmitancia de la misma atmósfera calculada en CPU (16 pasos) normalizada a máximo 1, suavizada con el
  exponente `RT64_SKY_SUN_REDDENING` (0,6: nubes blancas con el sol alto, doradas al atardecer) y multiplicada por una
  parte (`RT64_SKY_SUN_TINT` = 0,5) del tinte del sol de la escena, que en juegos con ciclo de día sigue su hora. Se
  apaga gradualmente cuando el sol se pone.
- Disco de radio angular `RT64_SKY_SUN_SIZE` (0,012 rad) con borde suavizado de un píxel y oscurecimiento del limbo;
  el ángulo sale de `asin(|d×s|)`, preciso para ángulos pequeños. Halo `pow(cosθ, 2000)·0,8 + pow(cosθ, 80)·0,12`, además
  del lóbulo de Mie de la LUT. En LDR el disco satura a blanco; la compresión de luces suaviza la transición.

### 4.4 Nubes (capa 2,5D)

- **Capa curva**: a una "altura de nube" sobre la cámara, curvada como la superficie de un planeta de radio
  `RT64_SKY_CLOUD_CURVATURE` (12 alturas). En el horizonte la distancia máxima es ≈5 alturas, así que las nubes no se
  aplastan en franjas infinitas como con un plano.
- **Anclaje al mundo**: coordenadas = (cámara.xz / `RT64_SKY_CLOUD_HEIGHT` + dirección.xz·distancia) × frecuencia
  (1/`RT64_SKY_CLOUD_SCALE`). Con `worldOrigin.w = 1` hay paralaje al moverse; sin él las nubes solo giran con la vista.
- **Ruido**: ruido de gradiente con hash entero, **periódico cada 256 celdas**; las octavas usan la matriz entera
  [[2,−1],[1,2]] (rotación 26,6° y escala √5), que conserva el período. Por eso los desplazamientos del viento y de la
  cámara se envuelven en CPU (módulo 256 y 1024/frecuencia) sin costuras ni pérdida de precisión con el tiempo.
- **Forma**: fbm de 3–6 octavas con *domain warp* suave (ruido a ¼ de frecuencia, amplitud 0,3, que se mueve al 40 %
  de la velocidad de las nubes para que cambien de forma). Umbral = 0,60 − 0,28·cobertura; borde = smoothstep angosto,
  espesor = rampa más ancha (bordes finos brillantes, centros grises).
- **Luz**: 1–4 muestras hacia el sol sobre la capa (distancias crecientes, pesos decrecientes, octavas reducidas) dan
  la auto-sombra; transmitancia `exp(−(sombra·1,5 + espesor·0,8)·absorción)` con mínimo 0,4; fase HG doble (adelante
  g = 0,75, atrás g = −0,25); "panza" más oscura donde la nube es gruesa y se mira hacia arriba; borde plateado en las
  partes finas a contraluz; ambiente del cielo (atenuado por el espesor) y rebote del suelo.
- **Bruma**: las nubes lejanas se funden con el color del horizonte y desaparecen en los últimos 3,4° sobre él.
- **Viento**: dirección fija del mundo (`RT64_SKY_WIND_ANGLE`, desde el eje derecho hacia adelante), 0,015
  celdas/s × `RT64_SKY_CLOUD_SPEED`.

### 4.4.1 Sombras de las nubes sobre el suelo

La composición de la iluminación (`LightingComposePS`, función `cloudShadow`) multiplica la sombra del sol por la de
las nubes: desde la posición de cada píxel sigue el rayo hacia el sol hasta la capa (plana, a una altura de nube sobre
el origen del mundo: `(p.xz + sol.xz / sol.y · (1 − p.y)) / altura`) y evalúa ahí la misma forma de las nubes (ruido,
*warp*, cobertura y viento, con los desplazamientos que calcula `LightingSky::getCloudShadow` para el mismo instante).
`densidad = smoothstep(umbral ± suavidad, forma)` y `luz del sol × (1 − densidad · fuerza)`; la vista de depuración 3
de la iluminación la muestra junto con el shadow map.

- Solo en exteriores con sol cuyo cielo reemplaza el cielo procedural: se pondera por la parte reemplazable del cielo
  del juego que mide el análisis (`gSkyAnalysis[escena].w`), salvo en VR, donde el cielo del juego no está y vale 1.
  Los cielos que se conservan (atardeceres, violeta) no tienen nubes procedurales y tampoco sombras.
- La escala es **12 veces menor** que la de las nubes del cielo (`RT64_SKY_GROUND_SHADOW_SCALE`): con la escala real
  (una celda = 30000 unidades) una sola sombra cubría el área entera y no se notaba el paso de las nubes; con 12 las
  manchas miden del orden de 2500 unidades y cruzan el área a ~40 unidades/s. Nadie puede comparar la sombra con la
  nube de arriba, así que la incoherencia no se ve.
- Calidad Low: 2 octavas sin *warp*; Medium en adelante: 3 octavas + *warp* de 2. Costo medido en el bosque
  (RTX 4070 SUPER, 1600x960 MSAA 4x): composición 0,15 → 0,20 ms (~0,05 ms).

### 4.5 Keying del fondo (qué píxeles se reemplazan)

Solo píxeles de fondo (profundidad ≥ umbral) dentro del rectángulo y por encima del horizonte geométrico
(`smoothstep(−0,02; 0,06; y)`), con un *key* suave tomado del enfoque del path tracer:

- azul: `smoothstep(0,04; 0,16; b − max(r; 0,85·g))`;
- blanco neutro (nubes pintadas): `smoothstep(0,62; 0,8; min) × (1 − smoothstep(0,12; 0,25; max − min))` ×
  `RT64_SKY_KEY_WHITE`;
- multiplicado por `smoothstep(L/2; L; luma)` con L = `RT64_SKY_KEY_MIN_LUMA` (0,18): un fondo oscuro (cielo nocturno,
  niebla azul oscura) se conserva, porque el cielo procedural es diurno.

El paisaje pintado (montañas, edificios, árboles) y todo lo que está bajo el horizonte (por ejemplo el mar pintado del
área 6 de MM64) se conserva. El *key* es el alfa del blending, así que el borde contra el paisaje pintado queda suave.

**Solo cielos diurnos** (`LightingSkyAnalyzeCS`): antes del cielo, un compute de un solo grupo promedia el color del
fondo por encima del horizonte (rejilla de 32x32 muestras de la copia de color) y lo guarda por índice de escena en un
buffer que persiste entre frames (suavizado al 10 % por frame; si se ven menos de 24 muestras de cielo se conserva el
valor anterior). El cielo se reemplaza en la medida en que ese promedio parezca un cielo de día,
`smoothstep(0,02; 0,10; b − r) × smoothstep(−0,02; 0,04; g − r) × smoothstep(0,2; 0,35; luma)`, y casi todo él quede
reconocido por el *key* (`smoothstep(0,6; 0,85; key medio)`): si no, se reemplazaría a parches. Un cielo violeta o de
atardecer tiene más rojo que verde, y uno verde menta apenas se reconoce: se conservan enteros. Antes se mezclaban a
parches (el área 10 de MM64, violeta con nubes rosadas, salía azul con manchas rosadas; el área 18, menta con nubes
crema, con trozos de cielo azul dentro de las nubes; el área 21 es un atardecer). Si el host quitó el cielo del juego (VR,
`LIGHTING_SCENE_FLAG_SKY_HIDDEN` desde `Application::setSkyBackgroundHint`) se reemplaza todo sin analizar.

### 4.6 Horizonte y salida

- Cerca del horizonte el cielo se funde con el color original de cada píxel:
  `mezcla = RT64_SKY_HORIZON_BLEND × (1 − y/RT64_SKY_HORIZON_HEIGHT)²` (0,8 y 0,12 ≈ 7°). Así la niebla del juego, que
  suele igualar su horizonte pintado, y la geometría lejana siguen coincidiendo, y se conserva el ánimo del cielo original.
- Compresión de luces por canal desde `RT64_SKY_KNEE` (0,6) hacia 1, conversión a gamma 2,2 (la misma convención que
  el resto de RT64) y *dither* de ±½ LSB (ruido de gradiente intercalado, fijo en pantalla) solo en targets de 8 bits.
- Exposición fija: el rango queda parecido al de un cielo LDR original, para que el HUD y la niebla sigan coincidiendo.

### 4.7 MSAA

La variante MS carga todas las muestras de profundidad, descarta si ninguna es fondo y escribe `SV_Coverage` con las
que sí lo son. En píxeles de borde (cobertura parcial) el color resuelto mezcla geometría y fondo, así que el *key* se
calcula con el promedio de los vecinos en cruz que son solo fondo; esto reduce el halo del cielo original alrededor
de las siluetas.

### 4.8 Estabilidad (sin parpadeo)

- Cada octava se desvanece a su media cuando sus celdas bajan de 4 a 2 píxeles (huella del píxel en la capa calculada de
  forma analítica con el tamaño angular del píxel y la inclinación de la capa), y lo que se pierde ensancha el borde
  para conservar la cobertura media. No depende de derivadas de pantalla ni de TAA.
- El disco solar tiene borde antialiasado; la LUT es suave; el *dither* es fijo.

## 5. Ajustes (`enhancementValue`, en vivo con `RT64_RT_TUNING_FILE`)

Salvo `RT64_SKY_ENABLE`, se leen cada 8 llamadas (leerlos toma un mutex y consulta el entorno), así que un cambio
tarda unos frames en verse.

| Nombre | Defecto | Efecto |
|---|---|---|
| `RT64_SKY_ENABLE` | 1 | Activa el cielo (si es 0 tampoco se copia la escena). |
| `RT64_SKY_QUALITY` | −1 | Preset 0–3; −1 sigue a `getRasterLightingQuality()`. |
| `RT64_SKY_DEBUG` | 0 | 1 *key*, 2 reemplaza todo el fondo sobre el horizonte, 3 alfa de las nubes, 4 sin nubes. |
| `RT64_SKY_REPLACE_ALL` | 0 | Ignora el *key* (como el modo 2 del path tracer). |
| `RT64_SKY_EXPOSURE` | 1,0 | Multiplica la exposición base (10,8). |
| `RT64_SKY_SATURATION` | 2,0 | Saturación de la atmósfera. |
| `RT64_SKY_TINT_R/G/B` | 0,85 / 0,97 / 1,12 | Tinte de la atmósfera. |
| `RT64_SKY_RANGE` | 0,75 | Exponente de compresión cenit/horizonte (1 = físico). |
| `RT64_SKY_HAZE` | 0,8 | Densidad de aerosoles (Mie): horizonte más blanco y halo solar más ancho. |
| `RT64_SKY_MULTI_SCATTERING` | 0,3 | Luz dispersada varias veces (aclara y desatura). |
| `RT64_SKY_OZONE` | 1,0 | Absorción del ozono. |
| `RT64_SKY_SUN_SIZE` | 0,012 | Radio angular del disco (rad). |
| `RT64_SKY_SUN_DISC` | 6,0 | Brillo del disco. |
| `RT64_SKY_SUN_GLOW` | 1,0 | Brillo del halo analítico. |
| `RT64_SKY_SUN_TINT` | 0,5 | Parte del tinte del sol de la escena que se usa. |
| `RT64_SKY_SUN_REDDENING` | 0,6 | Exponente del enrojecimiento atmosférico del sol. |
| `RT64_SKY_CLOUD_COVERAGE` | 0,45 | Cobertura (0 despejado, ~0,9 cubierto). |
| `RT64_SKY_CLOUD_OPACITY` | 1,0 | Opacidad (0 quita las nubes y su costo). |
| `RT64_SKY_CLOUD_SCALE` | 1,0 | Tamaño de las nubes. |
| `RT64_SKY_CLOUD_HEIGHT` | 30000 | Altura de la capa en unidades del mundo (paralaje). Depende de la escala del juego. |
| `RT64_SKY_CLOUD_CURVATURE` | 12 | Radio de curvatura de la capa, en alturas. |
| `RT64_SKY_CLOUD_SPEED` | 1,0 | Velocidad del viento. |
| `RT64_SKY_WIND_ANGLE` | 20 | Dirección del viento en grados. |
| `RT64_SKY_CLOUD_WARP` | 0,3 | Deformación de las formas (más de ~0,5 produce estrías). |
| `RT64_SKY_CLOUD_SHADOW` | 1,0 | Absorción de la luz del sol dentro de las nubes. |
| `RT64_SKY_CLOUD_LIGHT` | 1,0 | Luz del sol en las nubes. |
| `RT64_SKY_CLOUD_AMBIENT` | 0,55 | Luz del cielo y del suelo en las nubes. |
| `RT64_SKY_CLOUD_BELLY` | 0,5 | Oscurecimiento de la base de las nubes gruesas. |
| `RT64_SKY_CLOUD_HAZE` | 0,15 | Bruma de las nubes lejanas. |
| `RT64_SKY_GROUND_SHADOW` | 0,45 | Fuerza de las sombras de las nubes sobre el suelo (0 las apaga). |
| `RT64_SKY_GROUND_SHADOW_SCALE` | 12 | Cuánto más chicas que las nubes del cielo son sus sombras. |
| `RT64_SKY_GROUND_SHADOW_SOFTNESS` | 0,06 | Suavidad del borde de las sombras (en unidades del ruido; 0,12 las hacía un degradé sin borde). |
| `RT64_SKY_HORIZON_BLEND` | 0,8 | Cuánto del color original se conserva en el horizonte. |
| `RT64_SKY_HORIZON_HEIGHT` | 0,12 | Alto de esa mezcla (seno de la elevación). |
| `RT64_SKY_KEY_WHITE` | 1,0 | *Keying* de píxeles blancos neutros (nubes pintadas). 0 si el juego tiene un fondo gris liso de niebla. |
| `RT64_SKY_KEY_MIN_LUMA` | 0,18 | Luminancia bajo la cual el fondo no se considera cielo. |
| `RT64_SKY_KNEE` | 0,6 | Inicio de la compresión de luces. |
| `RT64_SKY_DITHER` | 1,0 | Amplitud del *dither* (en LSB de 8 bits). |

## 6. Calidad y costo

| Preset | Octavas | *Warp* | Muestras de luz (octavas) |
|---|---|---|---|
| Bajo (0) | 3 | no (las nubes se trasladan sin cambiar de forma) | 1 (1) |
| Medio (1) | 4 | 1 octava | 2 (2) |
| Alto (2) | 5 | 2 octavas | 3 (3) |
| Ultra (3) | 6 | 2 octavas | 4 (4) |

Costo por píxel de cielo: base ≈ 120 operaciones (rayo, *key*, LUT, sol, horizonte, compresión, gamma) y ≈ 100 por
octava de ruido evaluada. Las muestras de luz solo se evalúan dentro de las nubes y las octavas filtradas cerca del
horizonte no se evalúan. Estimación a 1080p con 40 % de cielo en una GPU media (RTX 2060, ~336 GB/s), incluido el
ancho de banda (profundidad de todo el rectángulo, `sceneColor` y lectura/escritura del target):

| Preset | Estimado | Presupuesto |
|---|---|---|
| Bajo | ~0,15–0,2 ms | ≤ 0,2 ms |
| Medio | ~0,3 ms | ≤ 0,4 ms |
| Alto | ~0,45 ms | ≤ 0,8 ms |
| Ultra | ~0,55 ms | — |

A eso se suma la copia de `copyColor` que hace el llamador (~0,05 ms a 1080p con 8 bits). La LUT no cuesta nada
mientras el sol no cambie. En iGPU o Steam Deck multiplicar por 3–4 (ver `techniques-research.md` §1.4). Son
estimaciones: medir con `RT64_PRINT_FRAME_TIME=1` alternando `RT64_SKY_ENABLE` y `RT64_SKY_QUALITY` con `rt_perf.ps1`.

## 7. Depuración y pruebas

- `RT64_SKY_DEBUG 1` muestra el *key* (blanco = se reemplaza); `2` dibuja el cielo procedural sobre todo el fondo por
  encima del horizonte (útil para ver la atmósfera y las nubes sin depender del *key*); `3` el alfa de las nubes; `4`
  la atmósfera sin nubes.
- Ejemplo con el archivo de ajustes en vivo:
  `rt_tune.ps1 -Sets "key|RT64_SKY_DEBUG 1","nublado|RT64_SKY_CLOUD_COVERAGE 0.8","atardecer|RT64_RT_SUN_ELEVATION 5"`
  (requiere la iluminación raster activa y el path tracer apagado; hoy `rt_tune.ps1` arranca con `-Rt 1`).
- Para comprobar el anclaje: girar la cámara (`rt_rotate.ps1`) y verificar que las nubes no se deslizan respecto del
  paisaje; moverse lejos para ver el paralaje.

## 8. Portabilidad

**Otro juego de N64 en RT64.** No hay nada específico del juego. Requisitos: un sol en la escena (estimado por
`State` o una luz direccional del juego) y una base del mundo correcta (`setWorldViewRotation/Translation` del host si
el juego hornea la cámara; si no, RT64 usa los ejes de la geometría con y hacia arriba). Si el juego no dibuja un
cielo pintado y limpia el fondo con un color de niebla gris claro, conviene `setSunRequiresSkyBackground` en el host o
`RT64_SKY_KEY_WHITE 0`. Ajustar `RT64_SKY_CLOUD_HEIGHT` a la escala de sus unidades.

**Otro renderer (por ejemplo un port de PS1).** `LightingSkyClouds.hlsli` (ruido, nubes, sol, *key*, compresión) y
`LightingSkyLutPS.hlsl` (solo push constants) no dependen de RT64. Para integrarlo hacen falta:

1. Un pase que dibuje la LUT 64×32 RGBA16F con `LightingSkyLutCB` cuando cambien el sol o los ajustes.
2. Un pase de pantalla completa después de los opacos que, por píxel de fondo (profundidad = valor de *clear*), arme el
   rayo (cualquier método; la base afín de 4.1 es la más barata), copie de `LightingSkyPS.hlsl` el armado de
   `LightingSkyCloudLayer` y la mezcla final, y use como "original" el color del fondo antes del pase.
3. En CPU: la base del mundo (y hacia arriba), la dirección del sol, el color del sol (`computeSunTransmittance` de
   `rt64_lighting_sky.cpp`) y los desplazamientos del viento envueltos en 256.

## 9. Limitaciones conocidas y trabajo futuro

- El *key* es heurístico: un fondo liso gris claro (niebla) o un paisaje pintado azulado (montañas lejanas) sobre el
  horizonte se reemplazan; un sol pintado en el cielo original puede quedar junto al sol procedural si no es neutro.
- Las transiciones parciales del *key* conservan algo del cielo original (sin "desmezcla"); con el cielo procedural
  cercano en tono al original no se nota.
- El cielo es diurno: con el sol bajo el horizonte da un crepúsculo oscuro y la mezcla del horizonte puede dejar una
  franja clara del cielo original.
- Al cambiar de preset cambia la forma de las nubes (el *warp* depende del preset).
- No hay perspectiva aérea sobre la geometría; puede reutilizar `lightingSkyFbm` y la tabla del cielo como las sombras
  de las nubes (4.4.1; ver `techniques-research.md` §7.3–7.4).
- Si un frame dibujara el cielo más de 32 veces, las ranuras se reutilizarían dentro del mismo frame.
- Todavía no tiene opción de menú: `enabled()` es el único punto donde combinarlo con una opción del host (por ejemplo
  la de "Sky Enhanced/Original" del path tracer).

## 10. Prototipo offline

El aspecto se ajustó sin el juego con un prototipo en Python/numpy y luego con una traducción línea por línea de los
shaders y del `.cpp` (incluida la base afín del rayo a través de una base del mundo rotada), comparando contra un fondo
sintético parecido al de MM64. Ese código y las imágenes de prueba no están en el repositorio.
