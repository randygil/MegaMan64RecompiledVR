# Iluminación raster mejorada para RT64 (Mega Man 64): investigación de técnicas

Fecha de la investigación: 2026-10-02. Documentos relacionados en esta carpeta: `HANDBOOK.md` (arquitectura de RT64,
datos del juego, scripts de prueba) y `ROADMAP.md` (objetivos y presupuestos por preset).

**Cómo leer este documento**

- Cada técnica tiene una ficha: *qué hace*, *encaje con el N64*, *entradas*, *costo*, *niveles*, *problemas y
  soluciones* e *implementaciones con licencia verificada*.
- **usable** = licencia permisiva compatible con MIT (se puede copiar respetando el aviso de copyright y licencia).
  **solo idea** = no copiar código; reimplementar desde el paper o la descripción.
- Las licencias se verificaron el 2026-10-02 contra el archivo `LICENSE` o el encabezado del archivo (API de GitHub y
  descargas oficiales). Si se actualiza una dependencia, volver a verificar.
- Costos: «(med.)» indica una medición publicada (con su fuente). El resto son **estimaciones** escaladas por ancho
  de banda y ALU (ver §1.4). Validar siempre con `RT64_PRINT_FRAME_TIME` y timestamps de GPU.

---

## 0. Resumen ejecutivo

1. **Arquitectura**: modulación *forward* dentro de `RasterPS` (la luz se aplica antes de la niebla y del blender del
   N64) más passes auxiliares por *replay* de la geometría (shadow map y prepass de profundidad/normales). La
   post-modulación diferida entre opacos y transparentes es el camino rápido para iterar el *look* (§1.3).
2. **Sombras del sol**: un solo shadow map ortográfico estabilizado (esfera + texel snapping en el marco de mundo
   real, no en el espacio de ojo que usa RT64 en MM64), 2048², filtro *optimized PCF* de The Witness (4/9/16 taps,
   MIT vía el sample de MJP). Cascadas y PCSS solo en Alto/Ultra. Hojas con alpha test en el pase de sombras.
3. **Sombras de contacto**: Bend Studio SSS (Apache-2.0), 0,19 ms a 1440p en PS5 con 60 muestras (med.). Versión
   simple de 8 a 16 pasos para Medio.
4. **AO**: XeGTAO (MIT). Bajo/Medio a media resolución con upsample bilateral; Alto a resolución completa (0,56 ms en
   RTX 2060 y 2,39 ms en Iris Xe a 1080p, med.). Radio grande, suave, aplicado solo a la parte ambiental.
5. **Normales**: no iluminar personajes low-poly con normales reconstruidas de depth (facetado). Usar
   `RSPSmoothNormalCS` (ya existe en el fork) en un pase de replay.
6. **GI barata**: luz hemisférica cielo/suelo + AO multi-bounce. "Fake GI" estilo Glamarye (MIT) en Medio. SSGI con
   visibility bitmask (SSRT3, MIT) solo en Ultra.
7. **Cielo**: conservar la imagen 2D (es arte original) y añadir disco/halo del sol, bruma y perspectiva aérea. Cielo
   procedural (Hillaire 2020, MIT) opcional, con LUT cacheada por área (el sol es fijo por área). Cuidado con el mar
   del área 6, que forma parte del fondo 2D.
8. **God rays**: radial blur (Mitchell, GPU Gems 3) a 1/4–1/8 de resolución con máscara de cielo. Raymarch del shadow
   map en Alto/Ultra.
9. **Bloom**: dual filter (Bjørge 2015: 2,8 ms contra 41,9 ms de un Gaussiano equivalente en Mali-T760 a 1080p, med.)
   en Bajo/Medio; cadena de COD:AW (13 taps + tent + Karis) en Alto/Ultra. En LDR usar máscara emisiva y pseudo-HDR,
   no un umbral puro.
10. **Tonemap/grading**: si la luz excede [0,1], Khronos PBR Neutral (Apache-2.0, casi identidad por debajo de ~0,76).
    AgX (implementación mínima MIT) como "look" opcional. ACES no (cambia los colores del arte). Vibrance (SweetFX,
    MIT), curva suave y LUT opcional.
11. **Nitidez/AA**: mantener el MSAA. CAS (MIT) al final. FXAA 3.11 trae un aviso «ALL RIGHTS RESERVED» sin
    concesión de licencia: evitarlo (SMAA o CMAA2 si hiciera falta).
12. **Árboles**: alpha-to-coverage con alpha afilado por `fwidth` (el equivalente moderno del `cvgXAlpha` del N64),
    sombras de hojas por alpha test, normales esféricas en espacio de mundo (coherentes entre las dos tarjetas),
    translucidez a contraluz y viento por vértice anclado en la base. Retirar el vaivén de UV y el ruido de hojas.
13. **ReShade**: solo reutilizar código de paquetes MIT/BSD/CC0 (SweetFX, prod80, Glamarye, OtisFX, FXShaders, CShade,
    Insane-Shaders, NiceGuy, Deband de haasn). qUINT, iMMERSE y RTGI (propietarios), AstrayFX (CC BY-ND/SA), Lilium
    (GPL-3.0), fubax (CC BY-NC-SA), dh (GPL-2.0), Zenteon (propietaria): solo ideas.
14. **Presupuestos a 1080p en GPU media (RTX 2060)**: Bajo ≈ 0,35 ms, Medio ≈ 1,0 ms, Alto ≈ 2,3 ms, Ultra ≈ 4–6 ms.
    En iGPU/Steam Deck multiplicar por 3–4. El path tracer actual cuesta ~9,8 ms a 960×576 en una RTX 4070 SUPER.

---

## 1. Contexto, restricciones y arquitectura

### 1.1 Lo que hay y lo que falta

| Dato | ¿Disponible? | Notas |
|---|---|---|
| Depth de la proyección 3D principal | Sí (`D32_FLOAT`, MSAA opcional) | El cielo es un fondo 2D sin depth útil; los decals usan `DEPTH_READ`. |
| Matrices de proyección y de mundo→vista | Sí | En MM64 la cámara va horneada en las matrices de modelo: el "mundo" de RT64 es el espacio de ojo PSX (+Y abajo). El host pasa la rotación/traslación mundo→vista (`setWorldViewRotation/Translation`). |
| Posición, normal y velocidad de mundo por vértice | Sí: `RSPWorldCS` (`worldPos/Norm/VelBuffer`) | Hoy solo corre con frame matching (`prevFrame.matched`); el modo mejorado debe forzarlo. |
| Normales suaves | Sí: `RSPSmoothNormalCS` (suelda vértices por posición y respeta un ángulo de pliegue) | MM64 nunca usa luces RSP: no hay normales de vértice. |
| Motion vectors | No directos, pero se derivan de `dstVel` + `prevViewProj` en el replay | Habilita acumulación temporal para AO/SSGI/PCSS. |
| MRT en el pase raster | **No** (blending dual source: `SV_TARGET0` color, `SV_TARGET1` factor) | Todo G-buffer sale de un pase de replay aparte. |
| HDR | No: exposición fija, targets de 8 bits (o 16 bits con el flag HDR de RT64) | Usar 16 bits o dithering al modular. |
| Cielo | Rectángulos 2D texturizados antes de la proyección 3D (pool 3); HUD en pools ≥ 8 | El mar del área 6 es parte del fondo 2D. |
| Interiores | Sin cielo y a menudo sin techo | El sol solo se activa con fondo de cielo; los interiores requieren luces locales (§6.6). |

### 1.2 Infraestructura del fork que conviene reutilizar

- `lib/rt64/src/shaders/RSPSmoothNormalCS.hlsl`: normales suaves por draw call con ángulo de pliegue; base del
  G-buffer de normales y de la iluminación N·L.
- `lib/rt64/src/shaders/RSPWorldCS.hlsl`: posiciones/normales/velocidades de mundo por vértice (shadow map, motion
  vectors, viento).
- `lib/rt64/src/shaders/RaytracingLibrary.hlsl`: `atmosphereScattering()` (Rayleigh + Mie de una dispersión),
  `skyClouds()` (capa 2D con auto-sombra), `sunSkyGlow()`, `emissionFactor()`. El código de follaje (`foliageWind`,
  `foliageDetailAt`) es justo lo que el usuario rechazó. El "roundSprite" curva la normal usando las UV de cada
  triángulo, por lo que las dos tarjetas cruzadas reciben normales distintas en la misma zona (costura de luz); §12.4
  lo reemplaza por normales en espacio de mundo.
- `BloomCS.hlsl` (Gaussiano a 1/4), `TemporalAACS.hlsl`, `LuminanceHistogramCS.hlsl` + `HistogramAverageCS.hlsl`
  (luminancia promedio: sirve para la normalización de brillo de §17.8), `BlueNoise.hlsli` + `res/bluenoise`
  (64×64×64 capas).
- plume: `RenderGraphicsPipelineDesc` ya expone `alphaToCoverageEnabled`, `depthClipEnabled`, `depthBias` y
  `slopeScaledDepthBias` (ver §12.2 sobre A2C).
- `render/rt64_framebuffer_renderer.cpp`: `RaytracingScene` ya calcula la base de mundo (`worldUp/Right/Forward`,
  `worldOrigin`), las matrices actuales y previas y qué proyección es la escena principal. Reutilizar esa partición
  para la "escena raster mejorada".

### 1.3 Arquitectura: forward contra diferida

| Aspecto | A. Forward en `RasterPS` (calidad final) | B. Post-modulación entre opacos y transparentes (iteración rápida) |
|---|---|---|
| Dónde se aplica | Sobre la salida del combiner, antes de niebla/blender, por muestra MSAA | Pase full-screen tras los opacos de la proyección principal |
| Niebla N64 | Correcta (se mezcla después) | Guardar el factor de niebla (alpha de shade) en el G-buffer y usar `m = lerp(m, 1, fog)` |
| Transparencias | Se iluminan en orden N64 o se excluyen por draw | Requiere reordenar transparentes después del marcador (está en el roadmap) |
| Bordes MSAA | Correctos | Halos leves en siluetas (luz a 1×); mitigar con depth "más cercano" |
| AO / contact shadows | Necesitan un prepass de depth (replay) antes del pase principal | Usan el depth del propio pase principal |
| Cambios de código | Ubershader y shaders especializados, interpolantes nuevos (posición de mundo, normal suave) | Un pase full-screen + G-buffer por replay |

Recomendación: prototipar con **B** (todo el *look* en un solo pase, fácil de depurar) y migrar a **A** cuando el
*look* esté fijado. El shadow map, el AO y el cielo se comparten entre ambas.

### 1.4 Hardware de referencia y cómo leer los costos

| GPU | FP32 aprox. | Ancho de banda aprox. | Rol |
|---|---|---|---|
| RTX 4070 SUPER | 35 TFLOPS | 504 GB/s | Equipo de desarrollo |
| RTX 2060 | 6,5 TFLOPS | 336 GB/s | "GPU media" de referencia (XeGTAO publica tiempos aquí) |
| GTX 1650 Max-Q | 2,6 TFLOPS | 128 GB/s | Portátil modesto (Glamarye publica tiempos aquí) |
| Intel Iris Xe (i7-1195G7) | 2,1 TFLOPS | ~68 GB/s compartidos | iGPU (XeGTAO publica tiempos aquí) |
| Steam Deck (RDNA2, 8 CU) | 1,6 TFLOPS | 88 GB/s compartidos | Portátil, 1280×800 |
| Adreno 650 (Quest 2) | ~1,2 TFLOPS | ~44 GB/s compartidos | TBDR; 1832×1920 por ojo (×2 ≈ 3,4 veces los píxeles de 1080p) |

Regla práctica: una iGPU o la Steam Deck tardan 3–4 veces lo que una RTX 2060 en passes de post (XeGTAO: 2,39 contra
0,56 ms). En Quest/Android cada pase full-screen extra vuelca tiles a memoria; la guía de Arm recomienda no procesar
con compute imágenes generadas por fragment shaders (dependencia hacia atrás que crea una "burbuja") y fusionar
passes. En VR los efectos de pantalla corren por ojo; el shadow map se comparte entre ojos.

---

## 2. Sombras del sol

### 2.1 Mapa único ortográfico estabilizado (recomendado por defecto)

- **Qué hace**: un shadow map ortográfico centrado cerca del jugador cuyo tamaño de texel es constante y cuya
  traslación se cuantiza a texels enteros, así no hay *shimmering* al mover o girar la cámara.
- **Encaje N64**: las áreas de MM64 son pequeñas, con niebla y distancia de dibujo corta. Un mapa de 2048² cuyo radio
  cubre lo visible da texels finos; CSM solo se justifica en campos abiertos.
- **Punto crítico en MM64**: el "mundo" de RT64 es el espacio de ojo (la cámara está horneada). La vista del sol y el
  snapping deben construirse en el **marco de mundo real**: `P_mundo = Rᵀ·(P_ojo − t)` con la rotación/traslación que
  pasa el host. Si se construye en espacio de ojo, cada giro de cámara rota el frustum de luz y vuelve el parpadeo.
- **Construcción** (código en §17.1): esfera de radio R alrededor de `C = jugador + forward·R/2`; R redondeado hacia
  arriba a 1/16; vista del sol con un "up" fijo; ortho `[-R, R]²`; snapping del origen del mundo proyectado a
  múltiplos de texel; extensión en Z (`zExtra`) o `depthClipEnabled = false` (*pancaking*) para que proyecten los
  casters que quedan fuera de la esfera (montañas, árboles altos).
- **Resolución/radio**: tamaño de texel = 2R/S. Elegir R por área (pista del host) o a partir de la distancia de
  niebla; por ejemplo, si la niebla cierra a 60 unidades, R ≈ 40–60.
- **Actualización**: cada frame (la geometría N64 es trivial). En Quest se puede cachear la parte estática y
  re-renderizar solo cuando C se mueve más de N texels.

### 2.2 Cascadas estables (CSM), solo Alto/Ultra en áreas abiertas

- 2–3 cascadas con el *split* práctico de Zhang et al. (GPU Gems 3, cap. 10):
  `split_i = λ·n·(f/n)^(i/N) + (1−λ)·(n + (f−n)·i/N)`, con λ ≈ 0,7–0,9.
- Cada cascada: esfera envolvente del sub-frustum (radio redondeado) + snapping, exactamente como
  `StabilizeCascades` en `MeshRenderer.cpp` del sample de MJP (MIT).
- Selección por profundidad de vista o "dentro de la proyección"; banda de mezcla del 10 % entre cascadas e
  interpolación de la escala del normal offset (The Witness lo hace para ocultar el salto entre cascadas).
- Costo: N passes de sombra; dos cascadas de 2048² ≈ 0,1–0,15 ms de render en RTX 2060 (estimado).

### 2.3 Filtrado

| Método | Taps | Calidad | Costo 1080p (media / iGPU) | Uso |
|---|---|---|---|---|
| PCF de hardware 2×2 (`SampleCmpLevelZero`) | 1 | Borde de un texel, escalonado | 0,03 / 0,1–0,15 ms | Quest |
| *Optimized PCF* 3×3 (Castaño) | 4 | Suave y estable | 0,05 / 0,2–0,3 ms | Bajo/Medio |
| *Optimized PCF* 5×5 | 9 | Muy suave | 0,1 / 0,4–0,6 ms | Medio/Alto |
| *Optimized PCF* 7×7 | 16 | Penumbra ancha fija | 0,15 / 0,7–1,0 ms | Alto |
| Poisson/Vogel rotado con IGN | 8–16 | Ruidoso sin TAA | similar | No recomendado sin acumulación |
| PCSS (bloqueadores 16 + filtro 16–32) | 32–48 | Penumbra variable con la distancia | 0,3–0,5 / 1,5–2,5 ms | Ultra, con acumulación temporal |

- The Witness midió su PCF optimizado entre 10 y 20 % más rápido que la variante con `Gather4` (GTX 480 y HD 4000,
  med.). MJP: pasar de 2×2 a 7×7 cuesta ~0,4 ms a 1080p en una AMD 7950 (med.).
- COD:AW (Jimenez 2014) advierte que con un *look* estilizado y ~8 muestras conviene un kernel fijo uniforme: el
  banding se nota menos que el ruido. Eso aplica de lleno al N64: **kernel fijo, no Poisson**.
- PCSS para el sol (luz ortográfica): penumbra en unidades de mundo `w = (z_receptor − z_bloqueador)·tan(θ)`, con θ
  el radio angular aparente del sol (0,27° real; 1–2° se ve mejor). La búsqueda de bloqueadores usa `Gather` sobre la
  profundidad cruda (sin comparación).

### 2.4 Alternativas prefiltrables: VSM, ESM, EVSM, MSM

Permiten mips, blur separable y MSAA en el mapa, con sombras muy suaves y sin ruido. En contra: *light bleeding* (VSM),
fugas cerca del contacto (ESM), memoria y ancho de banda (EVSM4 usa 4×FP32 por texel; MJP mide ~3 ms en una
configuración práctica de 1024² contra 7×7 PCF) y un blur extra por frame. MSM (Peters y Klein 2015) da buena calidad
con bias de 0,001–0,003 en 32 bits. Para N64 en hardware modesto **no compensan**: el PCF optimizado con depth de 16 o
32 bits es más barato y estable. ESM podría servir en Quest para una sombra muy blanda con un solo tap tras un blur a
512² (idea a evaluar).

### 2.5 Bias

- En el pipeline del shadow map: `depthBias` + `slopeScaledDepthBias` (plume los expone).
- *Normal offset* (Holbert, GDC 2011; The Witness): desplazar la posición de muestreo a lo largo de la normal en
  proporción a `sin(acos(N·L))` por el tamaño de texel; el bias de pendiente proporcional a `tan`, con tope 2 (código
  en §17.2). Variante de MJP: `offset = texel·escala·saturate(1 − N·L)·N`.
- *Receiver plane depth bias*: The Witness lo desactivó porque el PCF por hardware no admite un gradiente por texel.
- En N64 la geometría es gruesa y de pocos polígonos, así que un bias generoso es tolerable. Lo delicado son las
  tarjetas de árboles (dos caras) y los decals coplanares: **no dibujar decals en el shadow map** (`zmode` decal).

### 2.6 Casters con alpha test (hojas)

- En el pase de sombras, los draws con `cvgXAlpha` o `alphaCompare` usan un pixel shader mínimo que muestrea la
  textura (mismo tile y UV) y descarta con el mismo umbral que el pase principal. Hoy `RasterPS` descarta con alpha
  < 1/8 (cobertura `8/255·alpha < 1/255`), lo que engorda los recortes; con el afilado de §12.2 el umbral pasa a ~0,5.
- `cullMode NONE` para las tarjetas y algo más de bias de pendiente.
- Resultado: sombras moteadas de copa (*dappled light*), la mejora más visible en árboles.
- Costo: un fetch por píxel de hoja en el pase de sombras; insignificante a esta escala.
- Sin mips (texturas N64 de 32×32/64×64 a LOD 0) el texel de sombra suele ser mayor que el de la textura y las hojas
  parpadean; mitigar con PCF 3×3 o mayor y, si hace falta, probar el alpha contra una versión reducida (box 2×2).

### 2.7 Desvanecido, niebla y luz horneada

- Llevar la sombra a cero en el último 10–15 % del radio (o del rango de cascadas): sin borde visible.
- Multiplicar la fuerza de la sombra por `(1 − fog)`: a lo lejos manda la niebla del N64.
- **No duplicar sombras horneadas**: los colores de vértice ya oscurecen bajo techos y en rincones. La fórmula de
  §17.8 atenúa la sombra dinámica donde la luz horneada ya es baja.
- Sombra *blob* de Mega Man: con sombras reales conviene atenuarla o apagarla (identificarla en el host por hash de
  textura y pasarla a RT64 como pista genérica).

### 2.8 Problemas típicos y soluciones

| Problema | Solución |
|---|---|
| *Shimmering* al mover/girar | Snapping en marco de mundo real + R constante (cuantizado) |
| Acné | Bias de pendiente + normal offset |
| *Peter panning* | Bajar el bias constante; el normal offset desplaza menos |
| Fugas por paredes finas | Normal offset moderado. Front-face culling solo si la malla es cerrada (en N64 casi nunca: preferir sin culling) |
| Fondo 2D o HUD en el mapa | Dibujar solo draws de la proyección perspectiva principal |
| Cascadas que saltan | Banda de mezcla + escala del normal offset interpolada |
| VR | Un shadow map por frame, compartido por ambos ojos |

### 2.9 Implementaciones y licencias

| Recurso | Licencia verificada | URL | Veredicto |
|---|---|---|---|
| MJP, sample "Shadows" (optimized PCF, EVSM, MSM, CSM estable) | MIT (© 2016 MJP) | https://github.com/TheRealMJP/Shadows | usable |
| Castaño, "Shadow Mapping Summary – Part 1" (The Witness) | blog; su código está integrado en el sample de MJP | http://the-witness.net/news/2013/09/shadow-mapping-summary-part-1/ | idea + código vía MJP |
| Microsoft, "Common Techniques to Improve Shadow Depth Maps" y "Cascaded Shadow Maps" | documentación; samples en `walbourn/directx-sdk-samples-reworked` (MIT) | https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps | usable |
| Filament (PCF, PCSS/DPCF, VSM/EVSM, cascadas) | Apache-2.0 | https://github.com/google/filament | usable |
| bgfx, ejemplo `16-shadowmaps` | BSD-2-Clause | https://github.com/bkaradzic/bgfx | usable |
| Sascha Willems, `shadowmappingcascade` | MIT | https://github.com/SaschaWillems/Vulkan | usable |
| Godot (PCF/PCSS direccional) | MIT | https://github.com/godotengine/godot | usable |
| NVIDIA PCSS (Fernando 2005) y Bavoil (GDC 2008) | whitepapers | https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf | idea |
| Moment Shadow Mapping (Peters y Klein 2015) | paper (código en el sample de MJP) | https://momentsingraphics.de/I3D2015.html | idea |

---

## 3. Sombras de contacto en espacio de pantalla

### 3.1 Bend Studio Screen Space Shadows (Days Gone; también usado en Ghost of Tsushima)

- **Qué hace**: marcha en espacio de pantalla desde cada píxel hacia el punto proyectado de la luz. Mapea cada
  **rayo** a un wavefront 1D de 64 hilos que comparten lecturas en LDS, de modo que 60 muestras por píxel cuestan
  4 lecturas de imagen por hilo. Cada muestra hace un bilinear manual con umbral de borde (no interpola a través de
  discontinuidades). Las muestras se promedian en grupos de 4 (ninguna sola sombrea del todo), salvo las primeras
  `HARD_SHADOW_SAMPLES` (4); las últimas `FADE_OUT_SAMPLES` (8) se desvanecen. Es determinista: no usa ruido ni TAA.
- **Parámetros** (valores por defecto del código): `SurfaceThickness 0.005` (fracción de profundidad no lineal),
  `BilinearThreshold 0.02`, `ShadowContrast 4`, `IgnoreEdgePixels false`, `UsePrecisionOffset false`,
  `BilinearSamplingOffsetMode false`, `UseEarlyOut` (con wave intrinsics; ~15 % de sobrecosto si nada sale antes).
  Acepta cualquier convención de depth (`NearDepthValue`/`FarDepthValue`) y necesita un sampler *clamp-to-border* con
  borde igual a la profundidad lejana.
- **Luz direccional**: `lightProjection = float4(L, 0)·ViewProj` sin división; `BuildDispatchList()` genera 1–2
  dispatches si la luz está fuera de pantalla y 4–6 si está dentro.
- **Costo (med.)**: PS5, 1440p, 60 muestras a pantalla completa: **0,19 ms** (diapositivas SIGGRAPH 2023); menos de
  300 instrucciones (≈200 ALU), "menos de 5 instrucciones por muestra". Estimado a 1080p: RTX 2060 ≈ 0,15–0,25 ms;
  iGPU/Deck ≈ 0,6–1,0 ms.
- **Encaje N64**: excelente para asentar a Mega Man y los objetos en el suelo y para la sombra fina que el mapa de
  2048² pierde.
- **Truco "Depth Bias" de Bend**: sus materiales exportan un desplazamiento de alta frecuencia (±8 cm) que se suma a
  la profundidad usada por SSS, AO y decals → micro-sombras en suelos planos. Para N64: derivar una altura de la
  luminancia de la textura (§14.5) y sumarla a la depth del prepass → micro-relieve en suelos de 64×64.
- **Requisitos**: compute + groupshared, `WAVE_SIZE 64` (solo probado con 64). El early-out usa wave ops de SM 6.0
  (D3D12/Vulkan; en Metal verificar la traducción a simdgroup). Se puede compilar sin early-out.
- **Licencia**: **Apache-2.0** (© 2023 Sony Interactive Entertainment), en
  https://www.bendstudio.com/assets/cms/downloads/code_final_candidate.zip (`bend_sss_gpu.h`, `bend_sss_cpu.h`).
  Usable en un proyecto MIT conservando el encabezado y el texto de la licencia.

### 3.2 Ray march por píxel (estilo contact shadows de UE / Karabelas)

- 8–16 pasos hacia la luz en espacio de vista, longitud máxima corta (Karabelas: `max_steps 16`,
  `ray_max_distance 0.05`, `thickness 0.02`), test `0 < Δz < grosor`, jitter con Interleaved Gradient Noise y fade
  en los bordes de pantalla.
- Costo: 16 lecturas de depth por píxel. A media resolución ≈ 0,08 ms (media) / 0,3–0,5 ms (iGPU).
- UE expone "Contact Shadow Length" (fracción de pantalla, recomienda ≤ 1); con muestras fijas, más largo implica más
  espaciado. El código de UE está bajo su EULA y el de Spartan Engine bajo la «Spartan Engine License 1.0» (no
  comercial): **solo idea**.

### 3.3 Grosor y combinación con el shadow map

- Grosor constante en espacio de vista que crece con la distancia (como el "Linear Thickness" de SSRT3).
- `vis = min(shadowMap, contact)`. El early-out de Bend admite un `EarlyOutPixel()` propio: descartar píxeles de
  cielo y los que el shadow map ya ocluye.
- Las tarjetas de follaje no deberían proyectar sombras de contacto (son planos sin grosor → sombras de "papel"):
  marcarlas en el G-buffer y excluirlas o darles un grosor mucho menor.

### 3.4 Problemas

Disoclusión (lo que no está en pantalla no proyecta), por eso solo complementa al shadow map; aliasing del depth
proyectado (se mitiga con el bilinear con umbral de Bend); bordes de pantalla (fade); con MSAA usar un depth resuelto
"más cercano" a 1×; las muestras parciales de A2C en el follaje dejan depth mezclado en los bordes.

### 3.5 Implementaciones

| Recurso | Licencia | URL | Veredicto |
|---|---|---|---|
| Bend Studio SSS | Apache-2.0 | https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/ | usable |
| Unreal Engine contact shadows | EULA de Epic | https://dev.epicgames.com/documentation/en-us/unreal-engine/contact-shadows-in-unreal-engine | solo idea |
| Karabelas, "Screen space shadows" | blog sin licencia; Spartan Engine License 1.0 (no comercial) | https://panoskarabelas.com/blog/posts/screen_space_shadows/ | solo idea |
| Filament (screen-space contact shadows) | Apache-2.0 | https://github.com/google/filament | usable |

---

## 4. Ambient occlusion

### 4.1 Qué se ve bien en escenas low-poly del N64

- AO **grande, suave y de baja frecuencia** (esquinas de edificios, bajo objetos, base de muros y árboles). La AO de
  alta frecuencia sobre normales facetadas dibuja los polígonos y se ve "sucia".
- Radio en mundo equivalente a 0,5–1,5 m, intensidad 0,5–0,7, curva (`FinalValuePower`) suave, multi-bounce para no
  ennegrecer superficies claras.
- Aplicar la AO **solo a la parte ambiental** (en la fórmula de §17.8 multiplica el término hemisférico, no al sol).
- Desvanecer con la niebla y la distancia; excluir cielo, HUD y transparencias.
- Usar normales suaves del replay, no las reconstruidas de depth (§5).

### 4.2 XeGTAO (Intel)

- **Qué hace**: GTAO (Jimenez et al. 2016) práctico: integral analítica de visibilidad coseno por *slices* de horizonte
  en espacio de pantalla, con pirámide de profundidad para lecturas lejanas.
- **Passes**: `PrefilterDepths` (depth de vista + MIPs) → `MainPass` → `Denoise` (filtro espacial 5×5 consciente de la
  profundidad). Para lo temporal confía en el TAA si existe. Sin TAA: `NoiseIndex = 0` (ruido fijo, como indica su
  `XeGTAO.h`) y `DenoisePasses` 2–3 (0 desactivado, 1 nítido, 2 medio, 3 suave).
- **Presets** (slices × pasos por lado): Low 1×2, Medium 2×2, High 3×3, Ultra 9×3. Medium ≈ 2/3 del costo de High y
  Low ≈ 2/3 de Medium.
- **Costo (med.)**: 0,56 ms a 1080p en RTX 2060 (High); 2,39 ms a 1080p en Iris Xe (i7-1195G7); 1,4 ms a 4K en RTX
  3070. En la misma RTX 2060, ASSAO Medium ≈ 0,72 ms. Bent normals: +25 % (salida RGBA8 con la normal en RGB y la
  visibilidad en A).
- **Entradas**: depth (convertido a vista), normales opcionales (recomendado: las suaves del replay), constantes de
  proyección. Valores por defecto útiles: `RadiusMultiplier 1.457`, `FalloffRange 0.615`, `SampleDistributionPower 2`,
  `FinalValuePower 2.2`, `DepthMIPSamplingOffset 3.3`.
- **Licencia**: MIT (© 2016–2021 Intel), https://github.com/GameTechDev/XeGTAO. Ports: Bevy (MIT/Apache-2.0) y
  `BaBa_XeGTAO` para ReShade (sin licencia clara: no usar).

### 4.3 AO de horizonte barato para Bajo

- XeGTAO Low (1 slice × 2 pasos = 4 muestras) a media resolución + denoise de un pase + upsample bilateral (§4.5).
  Estimado: ≈ 0,1 ms en RTX 2060 y 0,4–0,6 ms en iGPU a 1080p.
- Alternativa aún más barata: la "Fast AO" de Glamarye (MIT), 2–8 puntos de depth en el mismo pase que otros efectos,
  con "AO shine" que aclara lo convexo para no oscurecer la imagen. Medición del autor (GTX 1650 Max-Q, 1080p, Witcher
  3): de 77 a 74 fps → ≈ 0,5 ms (med.).
- En Quest: sin AO de pantalla; usar AO por altura en árboles (§12.4) y el oscurecimiento horneado del juego.

### 4.4 Otras variantes

| Técnica | Notas | Licencia / URL | Veredicto |
|---|---|---|---|
| ASSAO (Intel, 2016) | Desentrelazado 2×2, 6/10/24 taps según preset, blur "smart". Low: 2,38 ms en Iris Pro 580 y 0,26 ms en GTX 1080 a 1080p (med.). Reemplazado por XeGTAO. | MIT, https://github.com/GameTechDev/ASSAO | usable (archivado) |
| FidelityFX CACAO (AMD) | Derivado de ASSAO, optimizado para RDNA | MIT, https://github.com/GPUOpen-Effects/FidelityFX-CACAO | usable |
| FidelityFX SDK | v1.1.x MIT; la rama 2.x usa una licencia de solo binarios con prohibición de ingeniería inversa para la mayoría de archivos | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK | usar repos sueltos o la etiqueta v1.1.4 |
| HBAO+ (NVIDIA) | Calidad buena, superado | "NVIDIA Source Code License (1-Way Commercial)": permite derivados pero obliga a distribuir esa parte bajo esa licencia | solo idea (no mezclar) |
| MXAO (qUINT / iMMERSE) | Ideas valiosas: `MXAO_SMOOTHNORMALS` ("útil en juegos viejos de pocos polígonos") y dos capas (fina + grande) | qUINT: "All rights reserved" sin licencia; iMMERSE: licencia propietaria | solo idea |
| SSAO Crytek (2007), Alchemy (2011), SAO (McGuire 2012) | Históricos; SAO trae una pirámide de depth útil | papers (G3D es BSD) | idea |
| Godot SSAO/SSIL | Basado en ASSAO | MIT, https://github.com/godotengine/godot | usable |

### 4.5 Media resolución + upsample bilateral

- Calcular AO (y contact shadows, god rays, SSGI) a ½ o ¼ con el depth "más cercano" de cada bloque 2×2 (o un
  patrón ajedrezado min/max) y subir con 4 taps ponderados por bilinear × similitud de profundidad relativa (código en
  §17.4). Si los cuatro difieren mucho, tomar el de profundidad más cercana (*nearest-depth upsampling*).
- Referencia teórica: Kopf et al., "Joint Bilateral Upsampling" (SIGGRAPH 2007).
- Costo: 4 fetches de AO + 4 de depth por píxel; se puede fusionar en el pase que consume la AO (forward o diferido).

### 4.6 Denoise sin TAA y reproyección por cámara

- Espacial: el 5×5 de XeGTAO (1–3 iteraciones) o un bilateral separable 2×7 taps sobre ½ resolución.
- **Temporal sin motion vectors de objetos**: la geometría estática se reproyecta solo con la cámara (depth +
  `invViewProj` actual + `prevViewProj`); rechazar la historia si la profundidad reproyectada no coincide (> 2–5 %) y
  limitar con el min/max del vecindario. Para objetos móviles, el fork ya tiene `dstVel` por vértice: escribir
  velocidad de pantalla en el replay. Código en §17.10. Esto habilita SSGI y PCSS de pocas muestras.
- Ojo con la interpolación de frames de RT64 (el juego corre a 30 fps y se interpola a la tasa de la pantalla): la
  reproyección debe usar las matrices del frame interpolado.

### 4.7 Bent normals y specular occlusion

- Bent normals (+25 % en XeGTAO): mejoran la dirección de la luz de cielo en rincones. Para N64 aportan poco; solo en
  Ultra.
- Specular occlusion (Lagarde, "Moving Frostbite to PBR"): solo si se añade especular falso (§14.4).
- Multi-bounce (Jimenez 2016, también en `surface_ambient_occlusion.fs` de Filament, Apache-2.0):
  `a = 2.0404·albedo − 0.3324; b = −4.7951·albedo + 0.6417; c = 2.7552·albedo + 0.6903;`
  `vis' = max(vis, ((vis·a + b)·vis + c)·vis)`.

### 4.8 Problemas

Halos alrededor de personajes contra el cielo (depth lejano: tratar el cielo como "sin ocluidor"), AO sobre niebla
(atenuar con el factor de niebla), AO bajo transparencias y decals (excluir o aplicar antes), bordes de pantalla
(radio en píxeles limitado), cambios de FOV (radio en mundo, no en píxeles), depth MSAA (usar un depth resuelto de una
muestra o el más cercano) y follaje (las tarjetas producen AO absurda: reducir la AO que reciben y que proyectan).

---

## 5. Normales: reconstrucción desde depth contra replay

| Método | Taps | Resultado | Licencia |
|---|---|---|---|
| `cross(ddx(P), ddy(P))` | 1 | Facetado y con artefactos en bordes | — |
| Turánszki: el mejor vecino por eje entre 5 taps (centro, izquierda, derecha, arriba, abajo); variante compute con caché 8×8 + borde en groupshared | 5 | Corrige el caso más común de bordes | Wicked Engine MIT; el autor permite usarlo libremente |
| atyuwen: 5 taps por eje con extrapolación del error de profundidad para decidir a qué segmento pertenece el centro | 9 | Elimina los tres tipos de artefacto que muestra el artículo | blog sin licencia explícita → reimplementar desde la descripción |
| **Replay con normales suaves** (`RSPSmoothNormalCS`) | — | Normales de vértice interpoladas, sin facetas | código propio del fork |

- **Por qué lo facetado se ve mal**: un personaje N64 tiene unos cientos de triángulos; con normales por cara, N·L y la
  AO cambian de golpe en cada arista (sombreado "plano" tipo PS1), justo lo contrario del sombreado suave horneado en
  los colores de vértice.
- **Recomendación**: pase de replay de la proyección principal con normales suaves en un target `RG16_SNORM`
  (octaédrica) o `RGB10A2`. Canales extra útiles: luminancia de la luz horneada (`shade`), factor de niebla y banderas
  (follaje, personaje, emisivo, transparente). Costo: geometría de pocos miles de triángulos, dominado por el ancho de
  banda de escribir los targets: ≈ 0,05–0,1 ms (media) / 0,2–0,4 ms (iGPU) a 1080p.
- Para la AO se pueden usar normales reconstruidas (Turánszki) suavizadas con un blur bilateral de 3×3 (la idea de
  `MXAO_SMOOTHNORMALS`); para N·L y rim light, siempre las del replay.
- El ángulo de pliegue de `RSPSmoothNormalCS` debe ser ~60–70°: suaviza cabezas y brazos sin redondear las esquinas de
  los edificios.

---

## 6. GI barata e iluminación ambiental

### 6.1 Luz hemisférica de cielo

`ambiente(N) = lerp(colorSuelo, colorCielo, 0.5 + 0.5·dot(N, arribaMundo))`. El color de cielo sale del promedio de
la imagen de cielo 2D (una reducción a 1×1 cuando cambia el área) o del preset del área; el color del suelo, del
promedio de la mitad inferior del frame (de la cadena de bloom: el último nivel ya es un promedio). Costo ≈ 0.

### 6.2 Sonda SH del cielo

Proyectar la imagen de cielo (o el cielo procedural) a SH de orden 2 (9 coeficientes) una vez por área y evaluar la
irradiancia con la normal (Sloan, "Stupid Spherical Harmonics Tricks"). Mejor que la hemisférica cuando el sol está
bajo. Costo ≈ 0.

### 6.3 Fake GI 2D (Glamarye)

Una versión muy desenfocada de la imagen aporta "luz del entorno" a cada píxel, con saturación ajustable y desplazada
según la orientación de la superficie. No entiende la escena, pero imita bien el sangrado de color ("lo gris junto a lo
rojo se ve un poco rojo"). Se obtiene casi gratis de la cadena de bloom (un nivel de ¼–⅛). MIT. Medido por su autor:
"Fake GI only" de 77 a 74–75 fps en GTX 1650 Max-Q (≈ 0,3–0,5 ms, med.). Recomendado en Medio.

### 6.4 SSDO y SSGI

- SSDO (Ritschel, Grosch y Seidel, I3D 2009): oclusión direccional + un rebote de color en pantalla. Moderado.
- **SSGI con visibility bitmask** (Therrien, Levesque y Gilet, "Screen Space Indirect Lighting with Visibility
  Bitmask", The Visual Computer; arXiv:2301.11376): reemplaza los ángulos de horizonte por una máscara de bits de
  sectores ocluidos con grosor constante, así la luz pasa detrás de superficies finas. Implementación: SSRT3 (Unity
  HDRP), **MIT** (© 2024 CDRIN), https://github.com/cdrinmatane/SSRT3; costo publicado ≈ 2–3 ms en RTX 3060 para el
  efecto completo. Solo Ultra, a media resolución y con acumulación temporal (§4.6).
- MXAO IL y RTGI de Pascal Gilcher: propietarios, solo idea. NGLighting (NiceGuy, CC0) es una SSGI de ReShade usable
  como referencia.

### 6.5 Recomendación por nivel

Bajo: hemisférica. Medio: hemisférica + SH + AO multi-bounce + Fake GI. Alto: lo anterior + bent normals opcionales.
Ultra: SSGI con visibility bitmask (reemplaza Fake GI).

### 6.6 Interiores y dungeons (sin sol)

- No usar sol (muchos interiores no tienen techo). Iluminar con: ambiente coloreado por área (hemisférico desde el
  promedio del frame), AO más fuerte, niebla de distancia/altura y **una luz local**: una "linterna" spot asociada a
  la cámara o a Mega Man con shadow map de perspectiva 512–1024² y PCF 3×3. Una luz puntual omnidireccional requiere
  6 caras (cubo) o dos paraboloides: 2–6 veces el costo, solo Alto/Ultra.
- Emisivos: detectar texturas muy brillantes sin luz o draws con blending aditivo → máscara emisiva → bloom dirigido
  (§9.3) y, en Medio+, "luz falsa" alrededor (el nivel ¼ del bloom emisivo sumado como luz difusa a las superficies
  cercanas, multiplicado por su albedo aproximado).

---

## 7. Cielo y atmósfera

### 7.1 Conservar, decorar o reemplazar el cielo 2D

- **Detección de píxeles de cielo**: los rectángulos de fondo se dibujan en el pool 3 antes de la primera proyección
  perspectiva y no escriben depth útil. Sirve cualquiera de estas máscaras: depth = valor de clear después del pase 3D,
  o un bit de stencil escrito por los rectángulos de fondo. RT64 ya sabe cuándo hay "fondo grande" (lo usa para
  activar el sol).
- Opción 1 (Bajo): **conservar la imagen** y sumar en los píxeles de cielo un disco y un halo de sol analíticos
  (`sunSkyGlow()` del fork) y un degradado de bruma hacia el horizonte. Costo ≈ 0 si se fusiona en la composición.
- Opción 2 (Medio+): **cielo procedural** con LUT (§7.2) + capa de nubes 2D (§7.4), mezclado con la imagen original
  (por ejemplo, procedural cerca del sol y del horizonte e imagen original en el resto).
- **Trampa**: en el área 6 el mar es parte del fondo 2D. Reemplazar todo el fondo borraría el mar: limitar el reemplazo
  por encima de la línea de horizonte (calculada con la orientación de la cámara y el "arriba" del mundo) o usar una
  pista del host por área.

### 7.2 Modelos de cielo

| Modelo | Costo | Calidad | Licencia del código | Comentario |
|---|---|---|---|---|
| Hillaire 2020 (LUTs en PC: transmitancia 256×64, sky-view 200×100, perspectiva aérea 32³, multi-dispersión 32²; en iPhone 6s sky-view 96×50 y perspectiva aérea 32²×16) | GTX 1080: LUTs 0,01/0,05/0,04/0,07 ms, dibujado en pantalla 0,14 ms, total 0,31 ms a 720p; Fortnite en iPhone 6s ≈ 1 ms total (med.) | Muy buena, físicamente basada, multi-dispersión | MIT (© 2020 Epic Games), https://github.com/sebh/UnrealEngineSkyAtmosphere | recomendado; con sol fijo las LUT se calculan una vez por área |
| Bruneton 2017 (precomputado) | LUTs caras de actualizar (~250 ms según Hillaire) | Muy buena | BSD-3-Clause, https://github.com/ebruneton/precomputed_atmospheric_scattering | precalcular offline |
| Hosek-Wilkie 2012 | ALU baratas, sin LUT | Buena de día; sin noche ni planeta | BSD-3-Clause (archivo `ArHosekSkyModel.c` v1.4a), https://cgg.mff.cuni.cz/projects/SkylightModelling/ | alternativa ligera |
| Preetham 1999 | Muy barato | Aceptable, colores algo apagados | three.js `Sky.js` (MIT) | Bajo |
| Una dispersión por rayo (fork actual) | Caro por píxel | Buena | propio | hornear en una LUT sky-view de ~200×100 al cambiar de área |

### 7.3 Perspectiva aérea y niebla con dispersión del sol

- Niebla con dirección del sol (Íñigo Quílez, artículo "fog"):
  `sunAmount = max(dot(rd, L), 0); colorNiebla = mix(azulado, amarillento, pow(sunAmount, 8))`.
- Niebla por altura con integral analítica (mismo artículo):
  `fog = (a/b)·exp(−ro.y·b)·(1 − exp(−t·rd.y·b))/rd.y`.
- En RT64: la niebla del N64 (color de niebla + factor por vértice) **tapa la distancia de dibujo**: no reducirla.
  Mejora barata: sustituir el color de niebla por uno teñido por el sol según la dirección de vista, por píxel, en el
  mismo lugar donde el blender aplica la niebla. Un término extra de altura se suma en post con el depth.
- Código del artículo de Quílez sin licencia declarada: son fórmulas, reimplementar.

### 7.4 Nubes

- **Capa 2D/2,5D**: fbm en un plano a altura fija, con 2–4 pasos de densidad hacia el sol para auto-sombra, como
  `skyClouds()` del fork. A ¼ de resolución con upsample: ≈ 0,05 ms (media) / 0,2–0,3 ms (iGPU). Animación lenta;
  se puede amortizar actualizando media pantalla por frame.
- **Volumétricas con raymarch** (estilo Nubis): demasiado caras para el objetivo; solo Ultra a ¼ con acumulación
  temporal (el fork ya tiene `cloudVolumeDensity()`).
- Sombra de nubes sobre el terreno: proyectar la misma fbm sobre el plano del suelo a lo largo de L y multiplicar la
  visibilidad del sol. Es casi gratis y añade mucha vida.

### 7.5 Problemas

Bandas en degradados de cielo de 16 bits (deband §14.2), costuras al mezclar imagen y procedural (mezcla suave por
ángulo), HUD (componer antes del pool 8), cielos que se desplazan en 2D con la cámara (respetarlo: la imagen original
ya está alineada con la vista).

---

## 8. Luz volumétrica / god rays

### 8.1 Radial blur con máscara (Mitchell, GPU Gems 3, cap. 13)

- **Qué hace**: por cada píxel acumula N muestras de una máscara de oclusión a lo largo del segmento hacia la posición
  del sol en pantalla, con decaimiento exponencial: `L = exposición·Σ peso·decay^i·muestra_i / n` (parámetros
  `Density`, `Weight`, `Decay`, `Exposure`, `NUM_SAMPLES`). Código en §17.5.
- **Máscara**: píxeles de cielo × brillo del sol (disco + halo) a ¼ u ⅛ de resolución; los ocluidores quedan en negro.
  Crysis usaba una máscara de profundidad (`1 − depth normalizado`) y 3 passes iterativos de 8 muestras (512 muestras
  "virtuales").
- **Costo**: a ¼ con 16–24 muestras o dos passes de 8, ≈ 0,05 ms (media) / 0,2–0,3 ms (iGPU); a ⅛ es casi gratis.
- **Encaje N64**: es el efecto "de época" por excelencia y funciona sin shadow map.
- **Limitaciones**: los rayos de objetos del fondo se dibujan delante de objetos cercanos; parpadeo cuando un ocluidor
  cruza el borde de pantalla; cuando el sol queda fuera o detrás de la cámara la posición tiende a infinito. Soluciones:
  atenuar por `dot(forward, L)` y por la distancia del sol al borde, *jitter* con IGN contra el banding y composición
  aditiva antes del tonemap.
- Licencia: GPU Gems es © NVIDIA sin una licencia de código explícita en el sitio → reimplementar desde la fórmula (es
  un bucle de 10 líneas).

### 8.2 Raymarch del shadow map (Killzone Shadow Fall, Vos, GPU Pro 5, 2014)

Marcha de 8–16 pasos desde la cámara hasta el depth de cada píxel, muestreando el shadow map (dispersión simple con
fase de Henyey-Greenstein), a ¼ de resolución con dithering, blur bilateral y upsample. Funciona aunque el sol esté
fuera de pantalla y respeta la geometría. Costo ≈ 0,15–0,2 ms (media) / 0,6–1,0 ms (iGPU). Referencia Unity:
`SlightlyMad/VolumetricLights` (BSD-3-Clause). Alto/Ultra.

### 8.3 Epipolar sampling

Engelhardt y Dachsbacher (I3D 2010); implementación de Egor Yusov en Intel `OutdoorLightScattering` (Apache-2.0) y
DiligentFX `EpipolarLightScattering` (Apache-2.0). Excelente calidad, pero complejo y pensado para atmósferas
completas; no compensa para MM64 salvo Ultra.

### 8.4 Variantes para móvil

Máscara y blur en un mismo pase de fragment a ⅛, 8–12 muestras, aplicado dentro de la composición final; desactivar
si el sol está detrás de la cámara. En Quest, apagado por defecto. Unity "Sun Shafts" (Unity Companion License) es la
misma idea: solo idea.

---

## 9. Bloom

### 9.1 Dual filter (Bjørge, ARM, SIGGRAPH 2015 "Bandwidth-Efficient Rendering")

- **Qué hace**: cadena de reducción con un filtro de 5 taps (centro 4/8 + cuatro diagonales a medio píxel 1/8) y de
  ampliación con 8 taps (cuatro en cruz a un píxel con peso 1/12 y cuatro diagonales con peso 2/12). Código en §17.6.
- Kawase (GDC 2003, "Frame Buffer Postprocessing Effects in DOUBLE-S.T.E.A.L"): passes sucesivos de 4 taps
  diagonales con distancia creciente (p. ej. 0, 1, 2, 2, 3) sobre una imagen reducida; el dual filter mezcla esa idea con
  la reducción/ampliación de resolución.
- **Costo (med.)**, Mali-T760 MP8 para un desenfoque equivalente a 97×97: Gaussiano 41,9 ms (1080p) / 19,2 ms
  (720p); "box" 21,4 / 9,9; Gaussiano 5×5 con reducción 23,5 / 7,0; Kawase 4,5 / 2,8; **dual filter 2,8 / 1,7**.
  PSNR frente al Gaussiano de referencia: 49,78 dB (dual) y 50,02 dB (Kawase).
- Estimado a 1080p, 5 niveles empezando a ½: ≈ 0,1 ms (media) / 0,4–0,6 ms (iGPU).
- Nota: Aras Pranckevičius (blog del 2026-10-01) compara blurs con radio animado y encuentra el dual Kawase 2–3 veces
  más lento que su "Smol Gaussian" y menos suave al animar el radio; para un bloom de radio fijo el dual filter sigue
  siendo adecuado (su `blur-playground` no tiene licencia detectada: solo idea).

### 9.2 Cadena de COD:AW (Jimenez, SIGGRAPH 2014)

- Reducción de 13 taps bilineales (36 texels) formada por 5 cajas 2×2 superpuestas con pesos 0,5 (centro) + 4×0,125.
- **Promedio de Karis** solo en la reducción de mip0 a mip1, aplicado por bloques de 4 muestras ("partial Karis
  average") contra los *fireflies*.
- Ampliación progresiva con tent 3×3 (9 taps) escalado por un radio, sumando cada nivel al anterior
  (`D' = D + blur(E')`). Seis mips `R11G11B10`. En COD:AW la entrada **no** se umbraliza.
- Estimado a 1080p desde resolución completa: ≈ 0,2 ms (media) / 0,8–1,2 ms (iGPU).
- Implementaciones usables: Bevy `crates/bevy_post_process/src/bloom/bloom.wesl` (MIT o Apache-2.0, cita COD:AW y
  usa Karis y tent 3×3); KinoBloom de Keijiro (MIT). LearnOpenGL "Phys. Based Bloom" es CC BY-NC 4.0 (solo idea);
  el bloom de Unity URP usa la Unity Companion License (solo idea).

### 9.3 Umbral en LDR

- En LDR casi todo está en [0,1]: un umbral puro hace brillar paredes blancas y nada más. Combinar:
  1. **Máscara emisiva**: draws con blending aditivo (disparos, explosiones, luces), texturas sin luz muy brillantes y
     el disco del sol → contribución completa.
  2. **Pseudo-HDR**: invertir un Reinhard con tope, `hdr = c / max(1 − min(max(c), 0.95), 0.05)`, y aplicar la rodilla
     suave de `BloomCS::brightPass` sobre ese valor (Glamarye llama a esto "tone mapping compensation").
  3. Un bloom global muy bajo (2–4 %) con mezcla conservadora `lerp(escena, bloom, k)` en vez de suma.
- Componer antes del tonemap/grading y con dithering.

### 9.4 Costo en móvil/TBDR

Cada nivel es un render pass con load/store: empezar a ½ o ¼, 4 niveles, usar fragment shaders (aprovechan la
compresión de framebuffer AFBC/UBWC y evitan la "burbuja" de compute) y fusionar el último upsample con la composición
final. En Quest: solo bloom emisivo a ¼ o apagado.

### 9.5 Otras implementaciones usables

Godot glow (MIT), FXShaders `NeoBloom`/`ArcaneBloom` (MIT), prod80 `PD80_02_Bloom` (MIT), CShade `cBloom`
(BSD-3-Clause), MagicBloom de la rama `legacy` de reshade-shaders (MIT), Insane-Shaders `Bessel_Bloom` (CC0).

---

## 10. Tonemapping y gradación en un pipeline LDR

### 10.1 ¿Hace falta tonemap?

Solo si la luz modulada puede pasar de 1. Con una modulación acotada en gamma (§17.8) basta un "hombro" suave. Si se
trabaja en un target de 16 bits flotante:

| Operador | Comportamiento | Licencia | Veredicto |
|---|---|---|---|
| **Khronos PBR Neutral** | Lineal (identidad) hasta `startCompression = 0.8 − 0.04` y luego comprime con desaturación suave: conserva los colores del arte | Apache-2.0 (`PBR_Neutral/pbrNeutral.glsl`); documentación CC-BY-4.0. https://github.com/KhronosGroup/ToneMapping | **recomendado** |
| AgX (Troy Sobotka) | Look "fílmico" con buena desaturación de altas luces; cambia contraste y saturación | El repo original no tiene licencia; implementación mínima de Benjamin Wrensch **MIT** (© 2024 Missing Deadlines, polinomio de 6.º orden y looks Golden/Punchy); también three.js (MIT), Filament (Apache-2.0), Godot (MIT) | opcional ("look cinematográfico") |
| ACES ajustado | Satura y desplaza tonos; oscurece medios | Narkowicz: "public domain CC0 or MIT"; Stephen Hill en `BakingLab/ACES.hlsl` (MIT) | no por defecto |
| Lottes 2016 | Curva paramétrica (contraste, hombro, `midIn/midOut`, `hdrMax`) | fórmula de GDC 2016 | si se quiere control fino |
| Tony McMapface | LUT 3D con buena gestión del color | MIT o Apache-2.0, https://github.com/h3r2tic/tony-mc-mapface | alternativa a Neutral |

```hlsl
// Khronos PBR Neutral (Apache-2.0, (c) 2024 The Khronos Group; PBR_Neutral/pbrNeutral.glsl), entrada lineal Rec.709.
float3 PBRNeutral(float3 c) {
    const float startCompression = 0.8 - 0.04, desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(c, newPeak.xxx, g);
}
// ACES ajustado de Narkowicz (CC0/MIT): saturate((x*(2.51*x + 0.03)) / (x*(2.43*x + 0.59) + 0.14))
// Lottes 2016: y = x^a / (x^(a*d) * b + c), con a = contraste, d = hombro, y b, c resueltos para que
//   midIn -> midOut y hdrMax -> 1:
//   b = (-midIn^a + hdrMax^a * midOut) / ((hdrMax^(a*d) - midIn^(a*d)) * midOut)
//   c = (hdrMax^(a*d) * midIn^a - hdrMax^a * midIn^(a*d) * midOut) / ((hdrMax^(a*d) - midIn^(a*d)) * midOut)
```

### 10.2 Gradación que vale la pena

- **Vibrance** (SweetFX, MIT): `color = lerp(luma, color, 1 + V·(1 − sign(V)·(max − min)))`; satura lo poco saturado.
  Valores de 0,1–0,2.
- **Curva de contraste** suave (S) o lift/gamma/gain: `out = pow(saturate(gain·(x + lift·(1 − x))), 1/gamma)`.
- **Temperatura**: tinte cálido leve en exteriores; prod80 `PD80_04_Color_Temperature` (MIT).
- **LUT 3D** 32³ (`RGBA8`, 128 KB) para presets artísticos; aplicar con `Texture3D` y filtro lineal. Fusionar todo en
  una LUT por preset.
- **Film grain**: solo opcional, después de CAS (CAS amplifica el grano). Grano dependiente de luminancia con el ruido
  azul del fork. prod80 `PD80_06_Film_Grain` (MIT).
- **Viñeta**: muy sutil (`1 − k·|uv − 0,5|^p`, k ≤ 0,15).
- No vale la pena: aberración cromática, lens dirt, DOF fuerte (el N64 no lo necesita; un "ADOF" leve solo en Ultra).

### 10.3 Orden y problemas

Iluminación → niebla → transparencias → bloom → tonemap (si 16F) → LUT/grading → CAS → grano/dither → HUD. No aplicar
gamma dos veces (RT64 trabaja con valores codificados en gamma, como el N64); dithering antes de cuantizar a 8 bits.

---

## 11. Nitidez y antialiasing

| Técnica | Qué hace | Costo 1080p (media / iGPU) | Licencia | Veredicto |
|---|---|---|---|---|
| **MSAA 4×** (ya existe) | AA geométrico real | — | — | mantener; base de A2C |
| **AMD FidelityFX CAS** | Afilado adaptativo al contraste local en cruz 3×3: `amp = sqrt(saturate(min(mn, 1 − mx)/mx))`, `w = amp·(−1/lerp(8, 5, sharpness))`, `out = (w·(b+d+f+h) + e)/(1 + 4w)` | ≈ 0,05 / 0,2–0,3 ms | MIT (© 2017–2019 AMD), https://github.com/GPUOpen-Effects/FidelityFX-CAS | **recomendado** (después del tonemap) |
| FSR1 RCAS | Variante de CAS del FSR1 | similar | MIT; port CShade `cRCAS` (BSD-3) | alternativa |
| LumaSharpen (SweetFX) | Unsharp mask solo de luma con límite (`sharp_clamp`), 2–4 taps | ≈ 0,03 / 0,1 ms | MIT | Bajo |
| FXAA 3.11 | AA post de bordes | ≈ 0,7 ms en GTX 1650 Max-Q (77 → 73 fps, med. Glamarye) | El encabezado dice «COPYRIGHT (C) 2010, 2011 NVIDIA CORPORATION. ALL RIGHTS RESERVED» y solo trae un descargo de garantías | **evitar copiar** |
| SMAA | AA morfológico de alta calidad | ≈ 1,1 ms en GTX 1650 Max-Q (77 → 71 fps, med.) | estilo MIT (Jimenez et al. 2013; no exige el aviso en binarios), https://github.com/iryoku/smaa | si no hay MSAA |
| CMAA2 (Intel) | AA conservador, barato | bajo | Apache-2.0, https://github.com/GameTechDev/CMAA2 | alternativa |
| Fast FXAA de Glamarye | 7 lecturas, combinado con sharpen | ≈ 0,3 ms (75 fps, med.) | MIT | Bajo sin MSAA |

TAA: el fork tiene `TemporalAACS` para el path tracer; en raster no hace falta con MSAA. Si se añade acumulación para
AO/SSGI, que sea por efecto (§4.6), no un TAA de escena (el HUD y el 2D del N64 sufren con TAA).

---

## 12. Follaje: tarjetas cruzadas con alpha test

### 12.1 Por qué el intento anterior se veía mal

El vaivén de UV desplaza la textura dentro de una silueta fija (efecto "papel que se derrite"), y el ruido procedural
añade detalle de alta frecuencia ajeno al pixel art de 32×32. Además, la normal "redondeada" se calculaba por tarjeta
con sus UV, así que las dos tarjetas cruzadas se iluminaban distinto justo donde se cruzan. Principio: **no tocar los
texels**; mover la geometría de forma rígida y mejorar la cobertura y la luz.

### 12.2 Alpha test con antialiasing: alpha-to-coverage + alpha afilado

- **Afilado** (Ben Golus, "Anti-aliased Alpha Test: The Esoteric Alpha To Coverage", 2017):
  `a = (a − cutoff) / max(fwidth(a), 0.0001) + 0.5;` → la transición ocupa un píxel, sin bandas ni interior
  transparente. Con mips: `a *= 1 + mipLevel·0.25` conserva la densidad a distancia (Golus; equivalente al cálculo de
  mips con cobertura conservada de Castaño, "Computing Alpha Mipmaps").
- **Encaje N64**: el RDP calcula cobertura por píxel y `CVG_X_ALPHA` multiplica cobertura por alpha (N64brew, "Reality
  Display Processor/Pipeline"). Alpha-to-coverage con MSAA es su equivalente moderno directo: es la forma *fiel* de
  dibujar estas tarjetas.
- **Detalle de RT64**: `SV_TARGET0.a` transporta la cobertura N64 (no el alpha) y el pase usa blending dual source,
  por lo que activar `alphaToCoverageEnabled` leería el valor equivocado (además hay reportes de restricciones al
  combinar A2C con dual source en algunas plataformas). Ruta robusta: calcular la máscara en el shader y emitir
  `SV_Coverage` (Vulkan `SampleMask`, Metal `[[sample_mask]]`); según Microsoft, si el shader escribe `SV_Coverage`
  el runtime desactiva A2C. Patrón: `bits = round(a_afilado·muestras)`, máscara ordenada (o rotada por píxel con IGN
  para evitar patrones). Sin MSAA, A2C equivale a alpha test: usar `a_afilado ≥ 0,5`.
- **Umbral**: hoy `RasterPS` descarta con alpha < 1/8 (siluetas gordas y dentadas). Con el afilado, un `cutoff` de
  ~0,4–0,5 conserva la forma de la hoja tal como la diseñó el artista con el filtrado de 3 puntos.
- Costo: un `fwidth` y unas pocas ALU por píxel de hoja.
- Alternativas cuando no hay MSAA ni TAA: *hashed alpha testing* (Wyman y McGuire, I3D 2017,
  https://casual-effects.com/research/Wyman2017Hashed/index.html) necesita acumulación temporal; no conviene aquí.

### 12.3 Sombras de hojas

Pase de sombras con alpha test (§2.6) → la copa proyecta luz moteada sobre el suelo y sobre sí misma. Limitar la
auto-sombra a ~60 % de intensidad sobre las propias tarjetas para que no se vean sucias.

### 12.4 Normales "volumétricas" (esféricas)

- Técnica estándar en follaje estilizado: transferir a las tarjetas las normales de una esfera o elipsoide que envuelve
  la copa (foros de Unreal "Spherical Normals for Trees", aVersionOfReality "Stylized Tree Shader").
- Para tarjetas cruzadas de N64 basta con el centro del bounding box de la tarjeta: dos quads cruzados por su eje
  comparten ese centro, por lo que la normal depende solo de la posición de mundo y es **coherente entre las dos
  tarjetas** (a diferencia del "roundSprite" por UV). Código en §17.9:
  `n = normalize(lerp(nTarjeta·signo, normalize((P − C)·(1, 0.6, 1)), k))`, con k ≈ 0,6–0,8.
- Difusa con *wrap* (`(N·L + w)/(1 + w)`, w ≈ 0,5) y AO de copa por altura y por distancia al centro
  (`ao = lerp(0.55, 1, h)·lerp(0.7, 1, r)`): parte inferior e interior más oscuros, como un volumen real.
- Detección: el path tracer ya usa la heurística `cvgXAlpha && !alphaBlend && textura && sin luces RSP && cara
  vertical`; reutilizarla y exponerla como bandera de draw.

### 12.5 Translucidez a contraluz

- Barré-Brisebois y Bouchard (DICE, GDC 2011): `lt = L + N·distorsión; t = pow(saturate(dot(V, −lt)), potencia)·escala;
  aporte = albedo·colorSol·t·visibilidad`. Con distorsión 0,2–0,4, potencia 4–8 y escala 0,5–1,5. Crysis (GPU Gems 3,
  cap. 16) combina `−N·L` con `E·L` y un mapa de grosor.
- Efecto: bordes de copa que se encienden cuando el sol está detrás del árbol. Multiplicar por la visibilidad del sol
  en la tarjeta (shadow map) para que no brille a la sombra de otra cosa.

### 12.6 Viento por vértice, anclado en la base

- Crysis, "main bending" (GPU Gems 3, cap. 16): `fBF = altura·escala; vNewPos.xy += vWind.xy·fBF;`
  `vPos = normalize(vNewPos)·longitud` (se conserva la longitud para que la copa no se estire). Funciones
  `SmoothCurve(x) = x·x·(3 − 2x)` y `TriangleWave(x) = abs(frac(x + 0.5)·2 − 1)` para ondas baratas.
- Para tarjetas: base = mínimo en "arriba" del bounding box; factor de flexión `h²`; fase estable por árbol (hash de
  la posición de la base) para que los árboles no se muevan al unísono; ráfagas lentas (0,1–0,3 Hz) sobre una oscilación
  de 0,5–1,5 Hz; amplitud 1–3 % de la altura. Código en §17.9.
- Dónde: en `RSPProcessCS`/VS, sumando `ViewProj·Δmundo` a la posición de clip; **aplicar el mismo desplazamiento en
  el pase de sombras** (si no, la sombra queda quieta bajo un árbol que se mueve).
- Las dos tarjetas comparten base y fase: se mueven juntas.

### 12.7 Otros

- Fade por distancia: la niebla del N64 ya oculta la aparición; en VR o primera persona, desvanecer con A2C las hojas
  muy cercanas a la cámara (evita "pantallas" verdes).
- Experimento futuro (roadmap): impostores o modelos 3D procedurales que reemplacen las tarjetas, con las mismas
  normales esféricas y viento.

### 12.8 Referencias y licencias de follaje

| Recurso | Licencia | URL |
|---|---|---|
| Golus, "Anti-aliased Alpha Test" (2017) | artículo; fórmulas reimplementables | https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f |
| Castaño, "Computing Alpha Mipmaps" | blog | http://the-witness.net/news/2010/09/computing-alpha-mipmaps/ |
| GPU Gems 3, cap. 16 (Crysis) | © NVIDIA, sin licencia de código explícita | https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis |
| DICE, translucidez (GDC 2011) | presentación | https://colinbarrebrisebois.com/2011/03/07/gdc-2011-approximating-translucency-for-a-fast-cheap-and-convincing-subsurface-scattering-look/ |
| ARM OpenGL ES SDK, ejemplo de translucidez | MIT (© ARM) | https://arm-software.github.io/opengl-es-sdk-for-android/translucency.html |
| N64brew, pipeline del RDP (cobertura) | wiki | https://n64brew.dev/wiki/Reality_Display_Processor/Pipeline |

---

## 13. Ecosistema ReShade

### 13.1 Paquetes y licencias verificadas

Estrellas de GitHub al 2026-10-02 como indicador de popularidad.

| Paquete (repo) | ★ | Licencia verificada | Efectos destacados | Veredicto |
|---|---|---|---|---|
| crosire/reshade-shaders, rama `slim` (por defecto) | 1,2k | Por archivo: `ReShade.fxh` CC0-1.0; `Deband.fx` **MIT** (© 2015 Niklas Haas); `LUT.fx` "© Marty McFly" sin licencia | Deband, LUT, DisplayDepth, UIMask | Deband usable; LUT solo idea |
| crosire/reshade-shaders, rama `legacy` | — | MagicBloom MIT; AmbientLight estilo MIT (Ganossa); AdaptiveSharpen y Colourfulness BSD (bacondither); FilmGrain2 CC BY 3.0 | Bloom, sharpen, color | usable según archivo |
| crosire/reshade (inyector) | 5,6k | BSD-3-Clause | — | — |
| CeeJayDK/SweetFX | 268 | MIT (© 2014 CeeJayDK). Terceros: `CAS.fx` MIT (AMD), `SMAA.fxh` estilo MIT, `FXAA.fxh` NVIDIA "All rights reserved" | Vibrance, LumaSharpen, Curves, LiftGammaGain, Tonemap, Vignette, SMAA | usable (salvo `FXAA.fxh`) |
| martymcmodding/qUINT | 583 | "Copyright (c) Pascal Gilcher / Marty McFly. All rights reserved." (sin licencia) | MXAO, Lightroom, ADOF, Bloom, SSR, Deband, Sharp | solo idea |
| martymcmodding/iMMERSE y METEOR | 498 / 55 | Licencia propietaria: prohíbe redistribuir y usar partes en otros proyectos sin permiso | MXAO, SOLARIS (bloom), SMAA, SHARPEN, FILMGRAIN, LAUNCHPAD | solo idea |
| RTGI (Pascal Gilcher, Patreon) | — | propietario | GI por raymarch en pantalla | solo idea |
| prod80/prod80-ReShade-Repository | 212 | MIT | Levels, Curved Levels, Color Balance, Color Temperature, Shadows/Midtones/Highlights, Selective Color, LUT, Bloom, Film Grain, Sharpening | **usable** |
| BlueSkyDefender/AstrayFX | 196 | Encabezados: CC BY-ND 4.0 (RadiantGI, GloomAO, Smart_Sharp), CC BY-SA 4.0 (BloomingHDR, con partes MIT de MJP); Clarity sin licencia clara | RadiantGI, GloomAO, Clarity, Smart_Sharp | solo idea |
| rj200/Glamarye_Fast_Effects_for_ReShade | 154 | MIT | Fast FXAA, sharpen inteligente, Fast AO + "AO shine", Fake GI, detección de cielo/menús, DOF sutil | **usable**; referencia para Bajo |
| EndlesslyFlowering/ReShade_HDR_shaders (Lilium) | 432 | GPL-3.0 | Análisis HDR, tonemap inverso, herramientas HDR | solo idea |
| FransBouma/OtisFX | 159 | MIT (PandaFX © Jukka Korhonen) | AdaptiveFog, DepthHaze, Heightfog, CinematicDOF, MultiLUT | usable |
| luluco250/FXShaders | 110 | MIT | NeoBloom, ArcaneBloom, MagicHDR, PiecewiseFilmicTonemap, RetroFog, Dither | usable |
| papadanku/CShade | 64 | BSD-3-Clause | cBloom, cCAS, cRCAS, cFXAA, cDLAA, cAutoExposure | usable |
| LordOfLunacy/Insane-Shaders | 117 | CC0-1.0 | LocalContrastCS (tipo Clarity), Dehaze, Bessel_Bloom, BilateralCS, CMAA_2 | usable |
| mj-ehsan/NiceGuy-Shaders | 189 | CC0-1.0 (también en encabezados, p. ej. `Rim.fx`) | NGLighting (SSGI/SSR), Rim, Volumetric Fog, Lamps | usable |
| Fubaxiusz/fubax-shaders | 140 | CC BY-NC-SA por archivo | FilmicSharpen, ContrastSharpening, PerfectPerspective | solo idea |
| AlucardDH/dh-reshade-shaders | 158 | GPL-2.0 | GI/AO/SSR en pantalla | solo idea |
| Zenteon/ZenteonFX | 180 | "AGNYA License" (todos los derechos reservados salvo lo indicado) | iluminación local/GI | solo idea |
| Daodan317081/reshade-shaders | 83 | BSD-3-Clause | RetroTint, Comic, ColorIsolation | usable |
| originalnicodr/CorgiFX, retroluxfilm/reshade-vrtoolkit | 56 / 102 | CC0-1.0 / BSD-3-Clause | utilidades, sharpen para VR | usable |
| Mortalitas/GShade | — | Mezcla de archivos con permisos de redistribución especiales | — | evitar |

### 13.2 Efectos más usados en juegos retro y N64, y qué reproducir

- Evidencia: MXAO trae `MXAO_SMOOTHNORMALS` "especialmente útil en juegos viejos"; el preset "Unrealistcally
  Realistic" (ModDB, 2024), hecho sobre Majora's Mask para juegos N64, combina AO con el RTGI de Marty (opcional).
  Glamarye midió en GTX 1650 Max-Q a 1080p (Witcher 3, base 77 fps): MXAO muy bajo 68 fps (≈ 1,7 ms), MXAO por
  defecto 65 (≈ 2,4 ms), MXAO + IL 63 (≈ 2,9 ms), Glamarye completo 72 (≈ 0,9 ms) (med.).

| Efecto ReShade favorito | Qué aporta | Cómo lo reproducimos (con licencia limpia) |
|---|---|---|
| MXAO | AO con normales suavizadas y dos capas | XeGTAO (MIT) + normales del replay + capa fina y gruesa |
| RTGI | Rebote de color | Fake GI (Glamarye MIT) en Medio; SSRT3 (MIT) en Ultra |
| Bloom / MagicBloom / SOLARIS | Brillo | Dual filter / COD:AW (§9) |
| Lightroom (qUINT) | Gradación completa | prod80 (MIT): niveles, curvas, balance, temperatura, LUT |
| Clarity | Contraste local | Insane-Shaders `LocalContrastCS` (CC0) a radio grande y fuerza baja |
| ADOF | Profundidad de campo | Solo Ultra y muy leve |
| Deband | Quita bandas de cielos de 16 bits | `Deband.fx` de haasn (MIT) |
| LumaSharpen / CAS | Nitidez | CAS (MIT) |
| SMAA / FXAA | AA | MSAA + A2C; SMAA solo sin MSAA |
| Vibrance / Curves / Tonemap | Color | SweetFX (MIT) + PBR Neutral |
| Rim (NiceGuy) | Contorno de luz en personajes | §14.4, con normales reales del replay |

Ventaja de hacerlo en RT64 frente a ReShade: tenemos normales reales, la dirección del sol, la niebla, la separación
cielo/3D/HUD y el orden de los draws; ReShade solo ve la imagen final (aplica AO sobre transparencias y HUD).

---

## 14. Mejoras específicas para N64 retro

### 14.1 Filtrado y escalado de texturas

- RT64 ya emula el **filtro de 3 puntos** del N64 (una interpolación triangular entre tres texels en lugar de cuatro):
  mantenerlo por fidelidad.
- Pre-escalado de texturas al decodificarlas (`TextureDecodeCS` → caché): xBR (Hyllian, MIT), ScaleFX (Sp00kyFox,
  MIT), Super-xBR (Hyllian, MIT) y MMPX (McGuire y Gagiu 2021, MIT) en `libretro/slang-shaders` (licencia por archivo;
  el repo no tiene licencia global). Costo único por textura.
- Problemas: muchas texturas N64 son pequeñas y borrosas a propósito (pensadas para bilinear); xBR da un aspecto
  "pintado". Hay que respetar el modo de repetición (wrap/mirror/clamp) en los bordes durante el escalado, escalar
  después de decodificar paletas (CI4/CI8) y premultiplicar alpha para no crear halos. Conviene una lista por
  textura (UI y sprites sí; terreno no). Para un remake real, los packs de texturas de RT64 (ver `remake-research.md`).

### 14.2 Deband y dither

- `Deband.fx` de haasn (MIT, rama `slim`): 4 muestras aleatorias dentro de un radio; si todas difieren del centro menos
  que un umbral, se promedian y se añade grano. Aplicarlo solo a píxeles de cielo y degradados. La versión de libretro
  viene de mpv bajo GPL-2.0+/LGPL-2.1+: no usarla.
- Dither: Interleaved Gradient Noise (`frac(52.9829189·frac(dot(p, (0.06711056, 0.00583715))))`, Jimenez 2014) o el
  ruido azul del fork (los de Christoph Peters son CC0). El STBN de NVIDIA tiene licencia **no comercial**: no usarlo.
  `TriDither.fxh` no declara licencia: solo idea.

### 14.3 Suavizado de bordes con alpha

A2C (§12.2) para tarjetas y sprites recortados; los sprites 2D del HUD no se tocan.

### 14.4 Especular y rim light en personajes

- Rim: `rim = pow(1 − saturate(N·V), 3)·visibilidadSol·colorCielo·0,15–0,3`, solo en draws marcados como personaje
  (los que tienen velocidad de vértice distinta de cero o matrices animadas) y con normales del replay. Referencia:
  NiceGuy `Rim.fx` (CC0), que lo hace en pantalla.
- Especular Blinn-Phong muy suave en la armadura de Mega Man (material por hash de textura en el host): fácil que se
  vea "plástico"; solo Alto/Ultra y con intensidad baja.

### 14.5 Relieve falso a partir de la luminancia

Altura = luminancia de la textura pasada por un paso bajo (3×3); gradiente en espacio de textura con 2 taps extra;
inclinar la normal (`n = normalize(n − T·dh/du·s − B·dh/dv·s)`, s pequeño) y usarla solo para N·L del sol y para el
"depth bias" de micro-sombras (§3.1). El fork ya tenía `bumpStrength` en el path tracer.

### 14.6 Brillo de emisivos

Ver §6.6 y §9.3: blending aditivo y texturas brillantes sin luz → máscara emisiva → bloom dirigido y luz falsa
cercana. Evita que las paredes blancas "florezcan".

---

## 15. Presets recomendados

Presupuesto total del modo mejorado a 1080p. Objetivos del roadmap en GPU media: Bajo ≤ 0,5 ms, Medio ≤ 1,5 ms,
Alto ≤ 3 ms.

| Función | Bajo (Quest, iGPU, móvil) | Medio (Deck, GTX 1650) | Alto (RTX 2060–3060) | Ultra (RTX 4070+) |
|---|---|---|---|---|
| Shadow map | 1 mapa ortográfico estable 1024² (Quest) / 2048² | 2048² estable | 2 cascadas 2048² | 3 cascadas 2048–4096² |
| Filtro | PCF HW 2×2 (Quest) / optimized 3×3 | Optimized 5×5 | Optimized 7×7 | PCSS + acumulación temporal |
| Hojas con alpha test en sombras | sí | sí | sí | sí |
| Modulación N·L + hemisférica (§17.8) | sí (forward) | sí | sí + SH | sí + SH |
| Prepass de normales (replay) | no (N interpolada en `RasterPS`) | sí, ½ res | sí, completa | sí, completa |
| AO | no (AO de copa por altura) | XeGTAO Low/Med a ½ + upsample | XeGTAO High completa | XeGTAO Ultra o SSGI bitmask |
| Sombras de contacto | no | ray march 8–16 a ½ | Bend SSS 60 muestras | Bend SSS + "depth bias" de textura |
| GI | — | Fake GI | Fake GI + bent normals | SSGI visibility bitmask a ½ + temporal |
| Cielo | imagen + disco/halo del sol | procedural LUT (cacheada) + nubes 2D a ¼ | Hillaire + nubes 2D + sombra de nubes | + nubes volumétricas a ¼ temporal |
| Niebla con sol / perspectiva aérea | color de niebla teñido por el sol | + niebla por altura | + perspectiva aérea | igual |
| God rays | no (Quest) / radial ⅛ 12 muestras | radial ¼, 2×8 | raymarch del shadow map ¼ | raymarch completo |
| Bloom | emisivo a ¼ (Quest: no) / dual 4 niveles | dual 5 niveles | COD:AW 6 mips | COD:AW 6 mips |
| Tonemap/grading | vibrance + curva | + LUT | + PBR Neutral (16F) | + AgX opcional |
| Nitidez | LumaSharpen o nada | CAS | CAS | CAS |
| Follaje | A2C afilado, normales esféricas, viento, translucidez | igual | igual | igual |
| **Total GPU media (RTX 2060)** | **≈ 0,3–0,4 ms** | **≈ 0,9–1,2 ms** | **≈ 2,0–2,5 ms** | **≈ 4–6 ms** |
| **Total iGPU / Deck** | **≈ 1,2–1,6 ms** | **≈ 3–4 ms** | **≈ 7–9 ms** (no recomendado) | — |

Notas:
- En una RTX 4070 SUPER, Ultra completo debería quedar en ≈ 1,5–2,5 ms a 1080p, frente a ~9,8 ms del path tracer a
  960×576 (que tiene 3,75 veces menos píxeles).
- **Quest 2**: solo funciones *forward* (shadow map compartido entre ojos + muestreo en el pase principal + follaje +
  color de niebla). Cada pase full-screen extra se paga dos veces y vuelca tiles a memoria. Si se añade post, fusionar
  todo en un único pase final de fragment.
- Desglose estimado por pase a 1080p (media / iGPU): shadow map 2048² 0,05–0,1 / 0,2–0,4; optimized PCF 3×3 0,05 /
  0,2–0,3; prepass 0,05–0,1 / 0,2–0,4; pirámide de depth 0,03 / 0,1–0,2; XeGTAO Low a ½ 0,1 / 0,4–0,6; XeGTAO High
  0,56 / 2,39 (med.); ray march de contacto a ½ 0,08 / 0,3–0,5; Bend SSS 0,15–0,25 / 0,6–1,0; nubes 2D a ¼ 0,05 /
  0,2–0,3; god rays radiales ¼ 0,05 / 0,2–0,3; raymarch volumétrico ¼ 0,15–0,2 / 0,6–1,0; dual filter 0,1 / 0,4–0,6;
  COD:AW 0,2 / 0,8–1,2; composición final (bloom + tonemap + LUT + CAS + dither) 0,08–0,1 / 0,3–0,5; SSGI bitmask a
  ½ 0,8–1,5 / 3–5.

---

## 16. Orden de passes recomendado

Variante A (forward, objetivo final). Entre corchetes, lo que depende del preset.

1. CPU: partición de draws de la proyección principal (reutilizar la lógica de `RaytracingScene`), dirección del sol
   en mundo real (preset del área), base de cámara y matrices actuales/previas, matriz estable del shadow map (§17.1).
2. Compute existentes: `RSPProcessCS` → `RSPWorldCS` (forzado en modo mejorado) → `RSPSmoothNormalCS` → desplazamiento
   de viento del follaje (mundo → clip).
3. **Shadow map**: replay de opacos y recortes de la proyección principal, depth-only + PS de alpha test para recortes,
   bias de pendiente, `depthClipEnabled = false` [cascadas en Alto/Ultra].
4. [Medio+] **Prepass de replay**: depth + normal suave (`RG16`) + luz horneada/niebla/banderas (`RG8`) + velocidad.
5. [Medio+] Pirámide de depth (compute o fragment en móvil).
6. [Medio+] **AO** (XeGTAO) → denoise → [acumulación temporal por reproyección].
7. [Medio+] **Sombras de contacto** (Bend o ray march) → máscara R8.
8. [Medio+] Cielo procedural/LUT (solo al cambiar de área) y nubes 2D a ¼.
9. **Pase principal N64** (existente, `RasterPS`): fondo 2D de cielo → opacos con modulación (shadow PCF + contact +
   AO con upsample bilateral en el shader + N·L/hemisférica, antes de niebla/blender) → recortes con A2C afilado
   (`SV_Coverage`), normales esféricas y translucidez → transparentes en orden N64 (con o sin modulación según el draw).
10. Resolve MSAA (existente).
11. **Cadena post en el límite entre la escena 3D y el HUD** (antes del primer draw del pool ≥ 8): composición de cielo
    (disco, halo, procedural, deband) → perspectiva aérea/niebla por altura → god rays (máscara de cielo + sol) →
    bloom (máscara emisiva + pseudo-HDR) → **composición final en un solo pase**: suma de bloom, tonemap (si 16F),
    LUT/vibrance/curva, CAS, viñeta y dither.
12. HUD y 2D del N64 (orden original) → VI (existente).

Variante B (post-modulación): igual hasta 8, pero el pase principal dibuja sin cambios los opacos, se inserta un
**pase de modulación full-screen** (G-buffer + shadow map + AO + contact, con `m = lerp(m, 1, fog)`) en el marcador
entre opacos y transparentes, y luego siguen las transparentes y la cadena post.

---

## 17. Pseudocódigo de las piezas centrales

HLSL o C++ ilustrativo; adaptar convenciones (row/column major, inversión de Y de `-fvk-invert-y`, Z invertida).

### 17.1 Matriz ortográfica estable con texel snapping

```cpp
// Marco de mundo REAL. En MM64 las posiciones de RSPWorldCS están en espacio de ojo PSX:
//   P_mundo = transpose(Rwv) * (P_ojo - twv)  (Rwv/twv: rotación/traslación mundo->vista del host)
// Basado en StabilizeCascades de TheRealMJP/Shadows (MIT, (c) 2016 MJP).
struct SunShadow { float4x4 worldToClip; float texelWorld; };

SunShadow BuildStableSunShadow(float3 Lw /*hacia el sol*/, float3 center, float radius,
                               uint size, float zExtra) {
    const float R = ceilf(radius * 16.0f) / 16.0f;                 // tamaño de texel constante
    const float3 up = (fabsf(Lw.y) > 0.99f) ? float3(0, 0, 1) : float3(0, 1, 0);
    float4x4 V = LookAt(center + Lw * (R + zExtra), center, up);    // mira hacia -Lw
    float4x4 P = Ortho(-R, R, -R, R, 0.0f, 2.0f * R + zExtra);
    // Snap: proyectar el origen del mundo y llevarlo a un múltiplo exacto de texel.
    const float4 o = mul(P, mul(V, float4(0, 0, 0, 1)));            // w = 1 en ortho
    const float2 t = float2(o.x, o.y) * (size * 0.5f);
    const float2 d = (round(t) - t) * (2.0f / size);
    P[0][3] += d.x;  P[1][3] += d.y;                                // traslación de clip (convención columna)
    return { mul(P, V), 2.0f * R / size };
}
// center = jugador + forwardHorizontal * R * 0.5. Al moverse o girar, el contenido se desplaza en texels enteros.
// Pase de sombras con depthClipEnabled = false (pancaking) para los casters fuera de la esfera.
```

### 17.2 Optimized PCF (5×5 con 9 taps) y offsets de bias

```hlsl
// Adaptado de TheRealMJP/Shadows (MIT, (c) 2016 MJP); método de Ignacio Castaño (The Witness).
Texture2D<float> gShadow;  SamplerComparisonState gCmp;   // LESS_EQUAL (o GREATER_EQUAL con Z invertida), lineal, clamp

float Tap(float2 base, float u, float v, float2 inv, float z) {
    return gShadow.SampleCmpLevelZero(gCmp, base + float2(u, v) * inv, z);
}
float ShadowPCF5x5(float3 sp /* uv [0,1] y z de luz */, float2 size) {
    float2 inv = 1.0 / size, uv = sp.xy * size;
    float2 base = floor(uv + 0.5);
    float s = uv.x + 0.5 - base.x, t = uv.y + 0.5 - base.y;
    base = (base - 0.5) * inv;
    float uw0 = 4 - 3 * s, uw1 = 7, uw2 = 1 + 3 * s;
    float u0 = (3 - 2 * s) / uw0 - 2, u1 = (3 + s) / uw1, u2 = s / uw2 + 2;
    float vw0 = 4 - 3 * t, vw1 = 7, vw2 = 1 + 3 * t;
    float v0 = (3 - 2 * t) / vw0 - 2, v1 = (3 + t) / vw1, v2 = t / vw2 + 2;
    float sum = uw0 * vw0 * Tap(base, u0, v0, inv, sp.z) + uw1 * vw0 * Tap(base, u1, v0, inv, sp.z)
              + uw2 * vw0 * Tap(base, u2, v0, inv, sp.z) + uw0 * vw1 * Tap(base, u0, v1, inv, sp.z)
              + uw1 * vw1 * Tap(base, u1, v1, inv, sp.z) + uw2 * vw1 * Tap(base, u2, v1, inv, sp.z)
              + uw0 * vw2 * Tap(base, u0, v2, inv, sp.z) + uw1 * vw2 * Tap(base, u1, v2, inv, sp.z)
              + uw2 * vw2 * Tap(base, u2, v2, inv, sp.z);
    return sum / 144.0;
}
// 3x3 con 4 taps: uw0 = 3-2s, uw1 = 1+2s, u0 = (2-s)/uw0 - 1, u1 = s/uw1 + 1 (igual en v); suma / 16.

float2 ShadowOffsets(float3 N, float3 L) {                  // The Witness
    float c = saturate(dot(N, L));
    float sinA = sqrt(1 - c * c);
    return float2(sinA, min(2.0, sinA / max(c, 1e-4)));      // (escala normal offset, escala slope)
}
// Pmuestra = Pmundo + N * ShadowOffsets(N, L).x * texelWorld * kNormal;  z -= kBias * (1 + ShadowOffsets(N, L).y)
```

### 17.3 AO de horizonte (núcleo GTAO, 2 slices × 2 pasos)

```hlsl
// Simplificación del bucle de XeGTAO (MIT, (c) Intel). Espacio de vista de XeGTAO (+Z hacia adelante).
float GTAOLite(float2 uv, float3 P, float3 N, float noiseA, float noiseB) {
    const int SLICES = 2, STEPS = 2;
    float3 V = normalize(-P);
    float radiusPx = min(kRadiusWorld * kProjScale / P.z, kMaxRadiusPx);
    float vis = 0;
    [unroll] for (int s = 0; s < SLICES; s++) {
        float phi = (s + noiseA) * PI / SLICES;
        float2 omega = float2(cos(phi), -sin(phi)) * radiusPx;        // pantalla (Y hacia abajo)
        float3 dirV = float3(cos(phi), sin(phi), 0);
        float3 ortho = dirV - dot(dirV, V) * V;
        float3 axis = normalize(cross(ortho, V));
        float3 projN = N - axis * dot(N, axis);
        float lenN = length(projN);
        float cosN = saturate(dot(projN, V) / lenN);
        float n = sign(dot(ortho, projN)) * FastACos(cosN);
        float lo0 = cos(n + PI / 2), lo1 = cos(n - PI / 2);
        float hc0 = lo0, hc1 = lo1;
        [unroll] for (int k = 0; k < STEPS; k++) {
            float t = (k + frac(noiseB + (s + k * STEPS) * 0.618034)) / STEPS;
            t = t * t + kMinS;                                          // SampleDistributionPower = 2
            float2 off = round(omega * t) * kPixelSize;
            float3 d0 = ViewPos(uv + off) - P, d1 = ViewPos(uv - off) - P;
            float l0 = length(d0), l1 = length(d1);
            float w0 = saturate(l0 * kFalloffMul + kFalloffAdd), w1 = saturate(l1 * kFalloffMul + kFalloffAdd);
            hc0 = max(hc0, lerp(lo0, dot(d0 / l0, V), w0));
            hc1 = max(hc1, lerp(lo1, dot(d1 / l1, V), w1));
        }
        lenN = lerp(lenN, 1, 0.05);
        float h0 = -FastACos(hc1), h1 = FastACos(hc0);
        float a0 = (cosN + 2 * h0 * sin(n) - cos(2 * h0 - n)) / 4;
        float a1 = (cosN + 2 * h1 * sin(n) - cos(2 * h1 - n)) / 4;
        vis += lenN * (a0 + a1);
    }
    return max(pow(vis / SLICES, 2.2), 0.03);                           // FinalValuePower 2.2
}
// FastACos(x): r = (-0.156583*|x| + PI/2) * sqrt(1-|x|); return x >= 0 ? r : PI - r;
// R = kRadiusWorld * 1.457; falloffRange = 0.615 * R; falloffFrom = R - falloffRange;
// kFalloffMul = -1 / falloffRange; kFalloffAdd = falloffFrom / falloffRange + 1; kMinS = 1.3 px / radiusPx.
```

### 17.4 Upsample bilateral (½ → completa)

```hlsl
float UpsampleBilateral(float2 uvFull, float zFull /* depth lineal */) {
    float2 p = uvFull * gLowSize - 0.5;
    int2 i0 = int2(floor(p));  float2 f = p - i0;
    float sumW = 0, sumV = 0, bestDz = 1e9, bestV = 1;
    [unroll] for (int k = 0; k < 4; k++) {
        int2 o = int2(k & 1, k >> 1);
        float bw = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        float zl = gLowDepth.Load(int3(i0 + o, 0));               // depth "más cercano" del bloque 2x2
        float v = gLowAO.Load(int3(i0 + o, 0));
        float dz = abs(zl - zFull) / zFull;
        float w = bw / (dz * 50.0 + 1e-3);
        sumW += w;  sumV += w * v;
        if (dz < bestDz) { bestDz = dz; bestV = v; }
    }
    return (bestDz > 0.1) ? bestV : sumV / max(sumW, 1e-5);         // nearest-depth en bordes duros
}
```

### 17.5 God rays por radial blur

```hlsl
// Mitchell, GPU Gems 3 cap. 13 (reimplementado desde la fórmula). Entrada: máscara a 1/4 (cielo * brillo del sol).
float3 GodRays(float2 uv, float2 sunUV, float2 pixel) {
    const int N = 16;
    float2 delta = (uv - sunUV) * (gDensity / N);
    float2 p = uv - delta * IGN(pixel);                     // jitter contra el banding
    float decay = 1.0;  float3 acc = 0;
    [loop] for (int i = 0; i < N; i++) {
        p -= delta;
        acc += gMask.SampleLevel(gLinearClamp, p, 0).rgb * (decay * gWeight);
        decay *= gDecay;
    }
    // gSunFade: atenuar si el sol sale de la pantalla o queda detrás (dot(forward, L) < 0)
    return acc * (gExposure * gSunFade);
}
// Valores iniciales orientativos (ajustar a ojo): Density 0.9, Weight 0.06, Decay 0.96, Exposure 0.3.
// Dos passes encadenados de 8 muestras equivalen a 64 muestras efectivas.
```

### 17.6 Bloom dual filter

```hlsl
// Bjørge (ARM, SIGGRAPH 2015). hp = 0.5 / tamaño de la textura de ENTRADA (× offset opcional).
float3 DualDown(float2 uv, float2 hp) {
    float3 s = Tex(uv) * 4.0;
    s += Tex(uv - hp) + Tex(uv + hp) + Tex(uv + float2(hp.x, -hp.y)) + Tex(uv - float2(hp.x, -hp.y));
    return s * 0.125;
}
float3 DualUp(float2 uv, float2 hp) {
    float3 s = Tex(uv + float2(-2 * hp.x, 0)) + Tex(uv + float2(2 * hp.x, 0))
             + Tex(uv + float2(0, -2 * hp.y)) + Tex(uv + float2(0, 2 * hp.y));
    s += 2.0 * (Tex(uv + float2(-hp.x, hp.y)) + Tex(uv + float2(hp.x, hp.y))
              + Tex(uv + float2(hp.x, -hp.y)) + Tex(uv + float2(-hp.x, -hp.y)));
    return s / 12.0;
}
// Prefiltro (primer down): pseudo-HDR + rodilla suave + máscara emisiva; para Alto/Ultra usar el 13 taps de
// COD:AW con promedio de Karis por bloques de 4 (w = 1 / (1 + luma)). Composición: lerp(escena, bloom, k) o suma.
```

### 17.7 CAS (modo solo afilado)

```hlsl
// Simplificado de ffx_cas.h (MIT, (c) 2017-2019 AMD). Después del tonemap; entrada en [0,1].
float3 CAS(int2 p, float sharpness /* 0..1 */) {
    float3 b = Load(p + int2(0, -1)), d = Load(p + int2(-1, 0)), e = Load(p);
    float3 f = Load(p + int2(1, 0)),  h = Load(p + int2(0, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
    float  peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
    float  w = amp.g * peak;                            // AMD usa el peso del verde para los tres canales
    return saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));
}
```

### 17.8 Modulación de luz sobre color N64 horneado (sin lavarlo)

```hlsl
// c: color del combiner (gamma, antes de niebla/blender). Lb: luma de la luz horneada (shade); 1 si el combiner
// no usa SHADE. N: normal suave (mundo real); up: arriba del mundo real (en MM64, no es +Y de RT64).
// albedo ≈ pow(c, 2.2) (aproximación suficiente para el multi-bounce); transl: §17.9 (0 fuera del follaje).
struct RelightParams {
    float3 sunColor, skyColor, groundColor;  // lineales, normalizados a luma ~1 (sol cálido, cielo frío)
    float  sunWeight;   // 0.6  : fracción de la luz horneada que se considera "sol"
    float  sunRef;      // 0.65 : N·L medio que asume la luz horneada
    float  strength;    // 0..1 : cuánto se reemplaza (preset)
    float  bakedLo, bakedHi;   // 0.15, 0.55 : compuerta por luz horneada
    float  minM, maxM;  // 0.45, 1.35 : límites de seguridad
};

float3 RelightFactor(float3 N, float3 L, float3 up, float vis, float ao, float3 albedo,
                     float Lb, bool foliage, float3 transl, RelightParams p) {
    float ndl  = foliage ? saturate((dot(N, L) + 0.5) / 1.5) : saturate(dot(N, L));   // wrap en follaje
    float3 hemi = lerp(p.groundColor, p.skyColor, 0.5 + 0.5 * dot(N, up));
    float3 aoMB = GTAOMultiBounce(ao, albedo);                       // §4.7
    float3 dyn = p.sunColor * (p.sunWeight * ndl * vis / p.sunRef) + transl
               + hemi * ((1.0 - p.sunWeight) * aoMB);
    // Donde el artista ya oscureció (Lb bajo: rincones, bajo techo) actúan sobre todo la AO y poco sol.
    float g = smoothstep(p.bakedLo, p.bakedHi, Lb);
    float3 darkArea = lerp(1.0.xxx, dyn, 0.35) * lerp(1.0.xxx, aoMB, 0.65);
    float3 m = lerp(1.0.xxx, lerp(darkArea, dyn, g), p.strength);
    return clamp(m, p.minM, p.maxM);
}

float3 ApplyRelight(float3 cGamma, float3 m, float fog /* 0 en forward */) {
    m = lerp(m, 1.0.xxx, fog);                                       // diferido: la niebla ya estaba aplicada
    float3 mg = pow(m, 1.0 / 2.2);                                   // el color N64 está codificado en gamma
    float lum = dot(cGamma, float3(0.299, 0.587, 0.114));
    float3 up   = max(mg - 1.0, 0.0) * (1.0 - smoothstep(0.55, 1.0, lum));   // no aclarar lo ya claro
    float3 down = min(mg - 1.0, 0.0);
    return cGamma * (1.0 + up + down);
}
// Normalización opcional por frame: e = media(luma original) / media(luma modulada) del frame anterior
// (LuminanceHistogramCS/HistogramAverageCS), suavizada en el tiempo; multiplicar m por pow(e, kNorm), kNorm ≈ 0.5.
// Interiores: sunWeight = 0 (solo hemisférica + AO) + luces locales sumadas a dyn con la misma normalización.
// Calibración: suelo al sol a 45° sin sombra -> dyn ≈ 1.05; a la sombra -> ≈ 0.4; con strength 0.7 la sombra
// queda ≈ 22 % más oscura en gamma (visible pero no negra).
```

### 17.9 Follaje: normal esférica, viento y translucidez

```hlsl
float3 FoliageNormal(float3 P, float3 cardN, float3 C /* centro del bbox de la tarjeta */, float3 V, float3 up) {
    float3 s = P - C;
    s -= up * (dot(s, up) * 0.4);                         // elipsoide: copa más alta que ancha
    float3 nCard = cardN * (dot(cardN, V) >= 0 ? 1 : -1); // cara hacia la cámara
    return normalize(lerp(nCard, normalize(s), 0.7));
}

float3 WindOffset(float3 P, float3 base, float3 up, float height, float t) {
    float h = saturate(dot(P - base, up) / max(height, 1e-3));
    float phase = frac(sin(dot(base, float3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2831;
    float gust = 0.6 + 0.4 * sin(t * 0.9 + phase * 0.5);
    float sway = sin(t * 1.3 + phase) * 0.7 + sin(t * 2.9 + phase * 1.7) * 0.3;
    float3 off = gWindDir * (sway * gust * gWindAmp * height * h * h);   // anclado en la base
    float3 r = (P + off) - base;
    return base + normalize(r) * length(P - base) - P;                    // conservar longitud (Crysis)
}
// En RSPProcessCS/VS: clip += mul(ViewProj, float4(WindOffset(...), 0)); el mismo offset en el pase de sombras.

float3 FoliageTranslucency(float3 N, float3 L, float3 V, float vis, float3 leafColor, float3 sunColor) {
    float3 lt = L + N * 0.3;                                  // distorsión
    float t = pow(saturate(dot(V, -lt)), 6.0) * 1.0;          // potencia, escala (DICE 2011)
    return leafColor * sunColor * (t * vis);
}
```

### 17.10 Ruido y reproyección por cámara

```hlsl
float IGN(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

float2 ReprojectUV(float2 uv, float deviceZ) {            // geometría estática; dinámicos: usar dstVel
    float4 clip  = float4(uv * float2(2, -2) + float2(-1, 1), deviceZ, 1);
    float4 world = mul(gInvViewProj, clip);  world /= world.w;
    float4 prev  = mul(gPrevViewProj, world);
    return prev.xy / prev.w * float2(0.5, -0.5) + 0.5;
}
// Aceptar la historia si |zHistoria - zReproyectada| / z < 0.03 y recortarla al min/max 3x3 del vecindario.
```

---

## 18. Resumen de licencias

Compatibilidad con RT64 (MIT, © 2024 RT64 Contributors):
- **MIT, BSD-2/3, zlib, CC0, Unlicense**: copiar conservando el aviso de copyright y licencia.
- **Apache-2.0** (Bend SSS, Filament, Khronos PBR Neutral, CMAA2, DiligentFX, Intel OutdoorLightScattering; Bevy y
  Tony McMapface ofrecen MIT o Apache-2.0): compatible; conservar encabezados, incluir el texto de la licencia e
  indicar los archivos modificados; incluye una concesión de patentes.
- **No copiar**: GPL/LGPL, CC BY-SA, CC BY-ND, CC BY-NC(-SA), licencias propietarias o "All rights reserved", código
  sin licencia, Unity Companion License, EULA de Unreal, licencias no comerciales.

| Recurso | Licencia | Veredicto |
|---|---|---|
| XeGTAO, ASSAO (Intel) | MIT | usable |
| FidelityFX CAS, CACAO (repos sueltos), FidelityFX SDK v1.1.x | MIT | usable |
| FidelityFX SDK 2.x (rama `main`) | licencia de solo binarios para la mayoría de archivos | no usar |
| Bend Studio SSS | Apache-2.0 | usable |
| MJP Shadows, BakingLab (ACES de Hill) | MIT | usable |
| Hillaire / UnrealEngineSkyAtmosphere | MIT (© Epic Games) | usable |
| Bruneton 2017, Hosek-Wilkie 1.4a | BSD-3-Clause | usable |
| Khronos PBR Neutral (código) | Apache-2.0 | usable |
| AgX mínimo (Wrensch) | MIT | usable (el repo original de Sobotka no tiene licencia) |
| ACES de Narkowicz | CC0 o MIT | usable |
| Tony McMapface, Bevy | MIT o Apache-2.0 | usable |
| SMAA | estilo MIT | usable |
| CMAA2 | Apache-2.0 | usable |
| FXAA 3.11 | "All rights reserved" sin concesión | evitar |
| SSRT3 (visibility bitmask) | MIT | usable |
| Filament, DiligentFX, Intel OutdoorLightScattering | Apache-2.0 | usable |
| bgfx | BSD-2-Clause | usable |
| Godot, three.js, Sascha Willems, Wicked Engine, KinoBloom, MS DirectX samples | MIT | usable |
| SlightlyMad VolumetricLights | BSD-3-Clause | usable |
| xBR, Super-xBR, ScaleFX, MMPX (libretro) | MIT por archivo | usable |
| Deband de ReShade (haasn) | MIT | usable |
| Deband de libretro (mpv) | GPL-2.0+/LGPL-2.1+ | no |
| Ruido azul de Christoph Peters | CC0 | usable |
| NVIDIA STBN | no comercial | no |
| HBAO+ | NVIDIA Source Code License (1-Way Commercial) | solo idea |
| Spartan Engine | Spartan Engine License 1.0 (no comercial) | solo idea |
| GLideN64 | GPL-2.0 | solo idea |
| Unity Graphics/PostProcessing | Unity Companion License | solo idea |
| LearnOpenGL (código) | CC BY-NC 4.0 | solo idea |
| GPU Gems (código) | © NVIDIA, sin licencia explícita | reimplementar |
| qUINT, iMMERSE, METEOR, RTGI, LUT.fx de Marty | propietarias / sin licencia | solo idea |
| AstrayFX | CC BY-ND 4.0 / CC BY-SA 4.0 | solo idea |
| Lilium HDR | GPL-3.0 | solo idea |
| dh-reshade-shaders | GPL-2.0 | solo idea |
| fubax-shaders | CC BY-NC-SA | solo idea |
| ZenteonFX | AGNYA (propietaria) | solo idea |
| SweetFX, prod80, Glamarye, OtisFX, FXShaders | MIT | usable |
| CShade, Daodan | BSD-3-Clause | usable |
| Insane-Shaders, NiceGuy-Shaders, CorgiFX | CC0-1.0 | usable |

---

## 19. Referencias primarias

Las URL de repositorios están en las tablas de §2.9, §3.5, §4.4, §7.2, §12.8, §13.1 (`https://github.com/<repo>`) y
§18. Aquí van los papers, charlas y artículos de origen.

- Sombras: Castaño, "Shadow Mapping Summary – Part 1" (http://the-witness.net/news/2013/09/shadow-mapping-summary-part-1/);
  MJP, "A Sampling of Shadow Techniques" (https://therealmjp.github.io/posts/shadow-maps/); Microsoft, "Common
  Techniques to Improve Shadow Depth Maps" y "Cascaded Shadow Maps" (https://learn.microsoft.com/en-us/windows/win32/dxtecharts/);
  Zhang et al., GPU Gems 3 cap. 10 (https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-10-parallel-split-shadow-maps-programmable-gpus);
  Fernando, PCSS (https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf); Bavoil, GDC 2008
  (https://developer.download.nvidia.com/presentations/2008/GDC/GDC08_SoftShadowMapping.pdf); Holbert, normal offset
  (GDC 2011, https://www.realtimerendering.com/blog/gdc-2011-links/).
- Sombras de contacto: Aldridge/Bend Studio, SIGGRAPH 2023 (https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/);
  Epic (https://dev.epicgames.com/documentation/en-us/unreal-engine/contact-shadows-in-unreal-engine); Karabelas
  (https://panoskarabelas.com/blog/posts/screen_space_shadows/).
- AO, normales y GI: Jimenez et al. 2016, GTAO
  (https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf);
  Intel, ASSAO (https://www.intel.com/content/www/us/en/developer/articles/technical/adaptive-screen-space-ambient-occlusion.html);
  Turánszki (https://turanszkij.wordpress.com/2019/09/22/improved-normal-reconstruction-from-depth/); atyuwen
  (https://atyuwen.github.io/posts/normal-reconstruction/); Therrien et al., visibility bitmask (https://arxiv.org/abs/2301.11376).
- Cielo, niebla y god rays: Hillaire, EGSR 2020 (https://sebh.github.io/publications/egsr2020.pdf); Quílez, "fog"
  (https://iquilezles.org/articles/fog/); Mitchell, GPU Gems 3 cap. 13
  (https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process);
  Sousa, "Crysis Next-Gen Effects" (https://www.slideshare.net/TiagoAlexSousa/crysis-nextgen-effects-gdc-2008); Vos,
  GPU Pro 5 (http://gameenginegems.com/gemsdb/article.php?id=1220).
- Bloom, tonemap y AA: Bjørge, SIGGRAPH 2015
  (https://community.arm.com/cfs-file/__key/communityserver-blogs-components-weblogfiles/00-00-00-20-66/siggraph2015_2D00_mmg_2D00_marius_2D00_slides.pdf);
  Jimenez, SIGGRAPH 2014 (http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare);
  Pranckevičius 2026 (https://aras-p.info/blog/2026/10/01/Fast-blur-with-animated-radius/); Wrensch, AgX mínimo
  (https://iolite-engine.com/blog_posts/minimal_agx_implementation); Narkowicz, ACES
  (https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/).
- Follaje y retro: Golus 2017 (https://bgolus.medium.com/anti-aliased-alpha-test-the-esoteric-alpha-to-coverage-8b177335ae4f);
  Wyman y McGuire 2017 (https://casual-effects.com/research/Wyman2017Hashed/index.html); Sousa, GPU Gems 3 cap. 16
  (https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis);
  Barré-Brisebois y Bouchard, GDC 2011
  (https://colinbarrebrisebois.com/2011/03/07/gdc-2011-approximating-translucency-for-a-fast-cheap-and-convincing-subsurface-scattering-look/);
  normales esféricas (https://forums.unrealengine.com/t/spherical-normals-for-trees-blender/98732,
  http://www.aversionofreality.com/blog/2022/8/7/stylized-tree-shader); N64brew, RDP
  (https://n64brew.dev/wiki/Reality_Display_Processor/Pipeline); ruido azul CC0 (https://momentsingraphics.de/BlueNoise.html).
- APIs y móvil: Microsoft, alpha-to-coverage y `SV_Coverage`
  (https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-blend-state); Arm GPU
  Best Practices (https://developer.arm.com/documentation/101897/latest/).
- ReShade en N64: preset "Unrealistcally Realistic"
  (https://www.moddb.com/mods/unrealistcally-realistic-for-majoras-mask-and-ocarina-of-time).
