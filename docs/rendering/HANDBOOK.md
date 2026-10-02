# Handbook de gráficos: MegaMan64Recompiled + RT64

Guía de referencia para trabajar en los gráficos del port: compilar, probar, depurar, entender RT64 y no
volver a investigar lo que ya se sabe. Mantener este documento al día con cada hallazgo.

Documentos relacionados (misma carpeta):
- `ROADMAP.md`: objetivo actual, prompt refinado y estado de cada línea de trabajo.
- `techniques-research.md`: investigación de técnicas raster (sombras, AO, god rays, bloom, ReShade, licencias).
- `remake-research.md`: reemplazo de texturas y modelos, herramientas de reescalado, experimentos de remake.
- `enhanced-lighting.md`: diseño e implementación de la iluminación raster mejorada (sombras, AO, normales, MSAA).
- `post-effects.md`: bloom, rayos de luz, CAS, grading, viñeta y dither de la escena iluminada.
- `procedural-sky.md`: cielo procedural (atmósfera, sol, nubes 2.5D) que reemplaza el cielo 2D.

---

## 1. Repositorios, ramas y commits

| Ruta | Remoto propio (`fork`/`origin`) | Rama | Upstream |
|---|---|---|---|
| `.` (MegaMan64Recompiled) | `origin` = github.com/randygil/MegaMan64RecompiledVR, `myfork` | `vr` | MegaMan64Recomp/MegaMan64Recompiled |
| `lib/rt64` | `fork` = github.com/randygil/rt64-mm64vr | `mm64vr` | rt64/rt64 |
| `lib/rt64/src/contrib/plume` | `fork` = github.com/randygil/plume-mm64vr | `mm64vr` | renderbag/plume |

- Orden de commit cuando cambian submódulos: **plume → rt64 → repo principal** (el principal guarda el puntero de `lib/rt64`).
- Commits locales por funcionalidad, mensaje en inglés con el estilo existente. **Nunca** `Co-Authored-By: Claude`
  ni "Generated with Claude Code" (regla global del usuario). No hacer push salvo que se pida.
- `src/res/bluenoise/LDR_64_64_64_RGB1.h` pesa 6 MB (ruido azul del path tracer); es normal.
- `git status` del repo principal muestra `M lib/rt64` mientras el submódulo tenga commits nuevos sin registrar:
  hacer commit en rt64 y luego en el principal con el puntero actualizado.

## 2. Entorno y compilación (Windows)

- Toolchain portable: `C:\Users\Usuario\Devel\tools\build_env.bat` (MSVC portable en `tools\msvc`, LLVM en
  `C:\Program Files\LLVM\bin`, cmake/ninja de pip).
- Script de compilación: `C:\Users\Usuario\Devel\tools\mm64_build.bat [config] [target]`
  (por defecto `x64-Release` y `MegaMan64Recompiled`). Configura con Ninja + clang-cl la primera vez.
  - Desde Bash: `cmd //c "C:\Users\Usuario\Devel\tools\mm64_build.bat"`.
  - Configuraciones existentes: `out/build/x64-Release` (escritorio), `out/build/x64-VR` (`-DRECOMP_VR=ON`),
    `out/build/android-arm64` (ver memoria `android-vr-toolchain`, `tools/android_build.sh`).
- Una compilación incremental que solo toca RT64 tarda de 1 a 3 minutos; tocar un header compartido (`src/shared/*.h`)
  recompila muchos shaders.
- **No compilar dos veces a la vez en el mismo directorio de build** (ninja se corrompe). Un solo agente compila.
- Para validar un `.cpp` de RT64 sin tocar el build: `cmd //c "C:\Users\Usuario\Devel\tools\rt64_compile_check.bat <archivo.cpp> [dir de includes extra]"`
  compila solo esa unidad con los flags reales (imprime `COMPILE CHECK OK/FAILED`). Los headers de blobs de shaders
  nuevos (`shaders/<Nombre>.hlsl.spirv.h` / `.dxil.h`) los genera CMake; para la prueba basta un placeholder con
  `extern const char <Nombre>BlobSPIRV[4]; extern const size_t <Nombre>BlobSPIRV_size;` dentro de `extern "C"`.
- Shaders: RT64 compila HLSL en tiempo de build con su propio DXC: `lib/rt64/src/contrib/dxc/bin/x64/dxc.exe`.
  - Opciones (ver `lib/rt64/CMakeLists.txt` ~l.124): común `-I lib/rt64/src`; SPIR-V `-spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout`;
    PS `-E PSMain -T ps_6_3`; VS `-E VSMain -T vs_6_3 -fvk-invert-y`; CS `-E CSMain -T cs_6_3`; RT `lib_6_3` + `-D RT_SHADER`.
  - Se registran en CMake con `build_pixel_shader`, `build_vertex_shader`, `build_compute_shader`, `build_ray_shader`;
    el binario queda embebido como arreglo C (`${CMAKE_BINARY_DIR}/src/shaders/<Nombre>.hlsl.spv.h` / `.dxil.h`).
  - Para validar un shader suelto sin compilar todo:
    `lib/rt64/src/contrib/dxc/bin/x64/dxc.exe -spirv -fspv-target-env=vulkan1.0 -fvk-use-dx-layout -I lib/rt64/src -E CSMain -T cs_6_3 archivo.hlsl -Fo out.spv`
  - CMake solo seguía el `.hlsl` principal; el path tracer y PostProcess/Debug/Im3D ya dependen de todos los `.hlsli` y
    `src/shared/*.h`. Si se agregan shaders que incluyen headers compartidos, añadir la dependencia igual
    (un shader viejo con layout desactualizado produjo una imagen gris con viñeta enorme).
- No editar fuentes con `Set-Content` de PowerShell (agrega BOM). En heredocs de Bash, `\\n` se colapsa: usar la
  herramienta Edit para cadenas C con `\n`.

## 3. Probar el juego

**Antes de lanzar el juego**: si el usuario dijo que necesita la PC, no lanzar nada (los scripts roban el foco,
el ratón y el teclado). Solo editar y compilar, y avisar.

- Directorio de prueba: `C:\Users\Usuario\Downloads\MegaMan64Recompiled-RT2` (exe + assets + dlls). `rt_run.ps1`
  copia ahí el exe recién compilado y la carpeta `assets`.
- Configuración del usuario: `%LOCALAPPDATA%\MegaMan64Recompiled\graphics.json` (API, MSAA, resolución,
  `pt_option`, `pt_effects_option`, `pt_sky_option`...). El usuario la cambia mientras se prueba (F2, menú):
  restaurar solo lo que cambie la prueba. Copia original: `graphics.json.rtbackup`.
- Guardado: `%LOCALAPPDATA%\MegaMan64Recompiled\saves`. El autoload carga la ranura 1.

Scripts en `C:\Users\Usuario\Devel\tools` (PowerShell; llamarlos con `powershell -ExecutionPolicy Bypass -File ...`):

| Script | Qué hace |
|---|---|
| `rt_run.ps1 [-Rt 0/1] [-Wait 30] [-Out png] [-Validation]` | Mata instancias previas, copia exe/assets, arranca con `MM64_AUTOSTART`/`MM64_AUTOLOAD` (y `RT64_RAYTRACING=$Rt`), espera y captura. Devuelve `running` o `crashed`. stdout/stderr en `game_out.txt`/`game_err.txt` del directorio de prueba. |
| `shot.ps1 [-Out png] [-HoldKey vk -HoldMs ms]` | Captura solo la ventana del juego con `PrintWindow` (24 bits, sin agujeros de alfa). Reduce a 960 px de ancho salvo `SHOT_FULL=1`. |
| `rt_compare.ps1 [-Warp area,entrada]` | Captura con path tracing, alterna con `MM64_RT_TOGGLE_FILE` y vuelve a capturar el mismo frame en raster; arma una imagen lado a lado `shots\cmp.png`. |
| `rt_tune.ps1 -Sets "label|NOMBRE valor;NOMBRE valor"` | Una sola ejecución, cambia el archivo de tuning en vivo (`RT64_RT_TUNING_FILE`) y captura cada set. |
| `rt_perf.ps1 -Sets ...` | Igual pero reporta el tiempo de frame (`RT64_PRINT_FRAME_TIME`) de cada set. |
| `rt_rotate.ps1 -Dx 400`, `rt_pitch.ps1 -Dy 400`, `rt_sweep.ps1` | Giran la cámara moviendo el ratón y capturan (necesitan foco; tocan Alt para tomarlo). |
| `keys.ps1 -Keys "0D,wait:1500,20:500,click:0.5,0.6"` | Envía teclas/clics a la ventana del juego (solo si es la ventana activa). `move:x,y` solo mueve el cursor y `wheel:-3` gira la rueda (negativo = hacia abajo): sirve para desplazar la lista de opciones del menú. |
| `rt_menu.ps1` | Abre el menú de configuración (Esc) y lo captura. Con `keys.ps1 -Keys "click:0.31,0.09"` se pasa a la pestaña Graphics (el clic falla si llega mientras el menú se abre: mandarlo en otra llamada); la lista de opciones se desplaza con `move:0.3,0.5,wheel:-3`. |
| `light_run.ps1 [-Tuning "N v;N v"] [-Warp a,e]` | Arranca con la iluminación raster (`RT64_LIGHTING=1`, PT apagado) y captura. |
| `light_tune.ps1 -Sets @("label|N v;N v",...) [-Warp a,e] [-Full] [-SettleMs 1500]` | Una ejecución, varios sets de tuning en vivo; captura y tiempo de frame (CPU y GPU) por set. Con `RT64_LIGHT_PRINT=1` deja en `game_err.txt` las escenas de iluminación. |
| `light_tune_api.ps1 -Api D3D12 -Sets @(...)` | Igual que `light_tune.ps1` con otra API gráfica; restaura `graphics.json` al terminar. |
| `vr_tune.ps1 -Sets @(...) [-Area n]` | Igual que `light_tune.ps1` con el build VR de escritorio en modo debug (los dos ojos lado a lado, sin casco; `Downloads\MegaMan64Recompiled-VR`). Con warps de área a veces termina en el menú de ítems; sin warp (bosque) funciona. |
| `area_scan.ps1 -Areas @("14,0","26,0",...)` | Un arranque por área con el debug warp; captura cada una y arma `shots\scan_sheet.png` (para buscar dungeons). |
| `gallery\build.py` | Arma la página de comparaciones antes/después (`template.html` + `config.json`, capturas de `shots\final\*.png` recortadas y embebidas en JPEG). Publicada como artefacto "Kattelox con luz nueva" (claude.ai/artifact/EUFQ2QLpsoECQGFvhF129B); para actualizarla, volver a capturar, editar `config.json` y republicar el mismo archivo. |
| `desktop_vr_build.sh` | Compila los parches (zig como compilador MIPS), corre N64Recomp y el build VR de escritorio. Si solo cambia C++, basta `cfg_vr.bat` (ver el script). |
| `city_test.ps1`, `vr_run.ps1` | Pruebas del build VR de escritorio (simulador o modo debug). |

Atajos dentro del juego: **F2** alterna path tracing, **F3** la iluminación raster mejorada, **F6** los árboles 3D
(opción "Trees"); Esc abre el menú. Con `developer_mode` RT64 usa además F1 (inspector), F2, F3 (RDRAM) y **F4 (packs
de texturas)**: por eso los árboles no usan F4.

Árboles 3D (`patches/tree3d.c`, experimento E3 de `remake-research.md`): los datos salen de
`C:\Users\Usuario\Devel\tools\upscale\proto\tree3d_gen.py --area 3 [--records 39,41] [--set CORE_SCALE=0.8]`, que
escribe `patches/tree3d_area3.c` y vistas previas en `tree3d_out\`. Después: `make` en `patches` (con `mips-cc.sh`),
`N64Recomp.exe patches.toml` y el build. Comparar en el juego con F6 entre dos capturas.

Los scripts que reciben arreglos (`-Sets @(...)`) se llaman con `& script.ps1 ...` desde la herramienta PowerShell;
`powershell -File` no pasa arreglos. Cada llamada de la herramienta es un proceso nuevo: las variables `$env:` no
persisten entre llamadas.

Comparar con números: `RT64_PRINT_FRAME_TIME=1` imprime en stderr el promedio de 120 frames
(`Frame render time: X ms, GPU Y ms (RT on|off)`): X es el tiempo del frame de render (CPU que espera al GPU) e Y el
tiempo de GPU medido con timestamps. Con `RT64_PRINT_FRAME_TIME=2` además imprime el tiempo de GPU de cada pase
(`GPU passes (ms): uploads, rsp, world vertices, smooth normals, shadow map, raster, gbuffer, ao, compose, sky, post...`).
Los marcadores se ponen con `gpuMarker(commandList, "nombre")` (`render/rt64_tuning.h`): cada uno mide desde el
anterior. Los scripts respetan un `RT64_PRINT_FRAME_TIME` ya puesto (`$env:RT64_PRINT_FRAME_TIME = "2"; & light_tune.ps1 ...`).
El valor impreso es el promedio de los últimos 120 frames: con 30 fps (VR debug) hacen falta ~9 s por set
(`-SettleMs 9000`) o se lee el set anterior.

Para combinar capturas en una cuadrícula (comparar sets) basta PIL:
`python -c "from PIL import Image; ..."` pegando las imágenes de `shots\` en una sola (ver ejemplos en el historial).

## 4. Variables de entorno

Juego / host (`src/`):

| Variable | Efecto |
|---|---|
| `MM64_AUTOSTART=1` | Salta la pantalla de inicio del launcher. |
| `MM64_AUTOLOAD=1` | Presiona Start/A con tiempos fijos para cargar la ranura 1 sin foco (`src/game/controls.cpp`). |
| `MM64_VR_WARP=<area>[,<entrada>[,<carga>]]` | Reemplaza el área de la N-ésima carga. Vistas útiles: 4,0 Apple Market (interior con NPCs), 5,0 calle de la ciudad, 15,0 casa de Roll, 16,0 mar; dungeons (se congelan sin Mega Man, cámara fija): 14,0 pared azul, 14,1 pasillo de piedra con raíces, 14,2 sala con agua, 20,0 metal azul, 26,0 ruina rosada, 26,1 sala blanca. No hay guardados dentro de una dungeon (las tres partidas disponibles empiezan junto al Flutter). |
| `MM64_RT_TOGGLE_FILE=<ruta>` | Si se crea ese archivo, alterna el path tracer (para comparar el mismo frame). |
| `MM64_RT_VIEW_AXIS_SIGNS=x,y,z` | Signos de ejes para la rotación de cámara leída de la RAM (depuración). |
| `RT64_RT_PRINT_VIEW=1` | Imprime traslación de vista, área, sol encendido/apagado. |

RT64 (path tracer y generales):

| Variable | Efecto |
|---|---|
| `RT64_RAYTRACING=1` | Arranca con el path tracer activo (fuerza Vulkan si la API es Auto). |
| `RT64_RT_TUNING_FILE=<archivo>` | Archivo `NOMBRE valor` por línea, recargado en vivo; sobreescribe cualquier `enhancementValue`. |
| `RT64_PRINT_FRAME_TIME=1` | Tiempo de render promedio cada 120 frames. |
| `RT64_RT_PASSES=<máscara>` | Pases RT despachados (1 primario, 2 directa, 4 indirecta, 8 reflexión, 16 refracción). |
| `RT64_RT_SCALE`, `RT64_RT_VIS` (6 directa, 4 difusa, 2 normales, 8 indirecta), `RT64_RT_DEBUG` (1 sin sombras, 2 NdotL, 4 normales de cara, 128 tablero de posiciones de mundo) | Depuración del path tracer. |
| `RT64_RT_PRINT_STATS`, `RT64_RT_PRINT_LIGHTS`, `RT64_RT_NO_TAA`, `RT64_RT_FORCE_INDOOR` | Depuración. |
| `RT64_RT_D3D12=1` | Permite RT en D3D12. Colgaba el GPU; probablemente por el bug de plume de la sección 9 (ya corregido), pero **no se volvió a probar**. |
| `PLUME_D3D12_DEBUG=1/2` | Capa de depuración de D3D12 (2 = validación en GPU). Un TDR con la capa activa dejó procesos imposibles de matar: evitar. |
| `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` | Validación de Vulkan (`rt_run.ps1 -Validation`). |

Valores de tuning del path tracer (todos `RT64_RT_*`, ver `rt64_framebuffer_renderer.cpp` y `rt64_state.cpp`):
AO, VOLUMETRIC, SUN_DISC, BLOOM, SHARPEN, VIGNETTE, SATURATION, CONTRAST, BUMP, WATER_REFLECTION, EMISSIVE,
SKY*, CLOUD*, FOLIAGE_WIND, FOLIAGE_DETAIL, SPRITE_VOLUME, SUN_AZIMUTH/ELEVATION/INTENSITY, INDOOR_*, OUTDOOR_AMBIENT,
MODEL_/SCENERY_ SPECULAR/GLOSS, SMOOTH_NORMALS.

El sol estimado (compartido por el path tracer, la iluminación raster y el cielo procedural) usa
`GameConfiguration::sunAzimuthDegrees` = 35° y `sunElevationDegrees` = **32°** (antes 22°: las sombras largas
tapaban media calle de la ciudad). Se sobreescriben con `RT64_RT_SUN_AZIMUTH` / `RT64_RT_SUN_ELEVATION`.

Iluminación raster, post y cielo: `RT64_LIGHT_*` (tabla en `enhanced-lighting.md`), `RT64_POST_*`
(`post-effects.md`), `RT64_SKY_*` (`procedural-sky.md`). Todos se editan en vivo con el archivo de tuning.

## 5. Datos de Mega Man 64 (específicos del juego)

Estos datos viven **solo en el host** (`src/main/rt64_render_context.cpp`, `src/game/`); RT64 recibe pistas genéricas.

- Es un port de Mega Man Legends (PSX): matemática estilo GTE. La cámara va **horneada en las matrices de modelo**:
  la matriz de vista de RT64 es identidad y el "mundo" de RT64 es el espacio de ojo PSX (+X derecha, **+Y abajo**, +Z adelante).
- Matriz de vista del juego: `0x801D4760`, 3x3 s16 en 4.12 (fila mayor) y traslación s32 x3 en `+0x14`.
  El host la lee en cada `send_dl` y la pasa con `Application::setWorldViewRotation/Translation`.
- Área cargada: s16 en `0x801BC450` (clave de escena); `0x801BC438/9` es la petición de carga (la usa el debug warp).
- El cielo se dibuja como rectángulos 2D texturizados antes de la proyección 3D (pool de dibujo 3); los pools >= 8 son el HUD.
- Interiores/dungeons: no dibujan cielo y muchos no tienen techo (el sol se filtraría): el sol solo se activa si hay un
  rectángulo de fondo de más de medio framebuffer antes de la primera proyección perspectiva, recordado por área.
- Nunca usa iluminación del RSP (`G_LIGHTING` apagado, `vertexLightCount 0`): sin normales de vértice. Los vértices
  sin luz guardan el color en el hueco de la normal: usar normales solo si el vértice tiene luces RSP.
- Las direcciones de luz RSP que envía el juego son relativas a la cámara: nunca usarlas para el sol.
- Árboles: tarjetas cruzadas sin luz con `cvgXAlpha` (alpha test). El mar del área 6 es parte del fondo 2D.
- Los personajes (Mega Man, NPCs) se dibujan con el combiner `TEXEL0` puro en 2 ciclos (`FCFFFFFF FFFCF238`, sin
  SHADE): no sirve "sin SHADE = emisivo" para detectar pantallas o lámparas, porque los personajes quedarían sin luz.
  El terreno usa `SHADE*TEXEL0` (luz horneada en los vértices).
- El juego corre a 30 fps; RT64 interpola a la tasa de la pantalla si `rr_option` es Display.
- Huesos de Mega Man: 0 torso, 1 cabeza, 2-4 brazo derecho, 5-7 izquierdo, 8 cadera, 9-11 / 12-14 piernas.
- Actor de Mega Man: `0x802049B0`; posición s16 (unidades del mundo, -Y arriba) en +0x14/+0x16/+0x18, yaw en
  `PLAYER_YAW` (ver `patches/vr.c`). El host la pasa a RT64 con `setFocusPosition` para la linterna de interiores.
- **VR**: el juego dibuja cada ojo en una mitad del framebuffer (dos proyecciones perspectiva, una escena de
  iluminación por ojo). `patches/vr.c` quita el cielo 2D (pool 3) porque en el casco se desliza distinto que el mundo y
  marea; en su lugar `vr_draw_background` pinta el color de la niebla. Como RT64 ya no ve el cielo, el parche avisa con
  `recomp_vr_report_sky()` → `zelda64::renderer::report_sky_background()` → `Application::setSkyBackgroundHint(true)`:
  la escena se trata como exterior (sol, sombras) y recibe el **cielo procedural**, que sí queda fijo en el mundo.

## 6. Arquitectura de RT64 (lo necesario para tocar el render)

Flujo de un frame:

1. **HLE** (`src/hle/`): `State` interpreta los display lists. Agrupa draw calls en `Workload` → `FramebufferPair`
   (un color + depth del N64) → `Projection` (tipo `Perspective`, `Orthographic`, `Rectangle`, `Triangle`) → `GameCall`
   (`callDesc` con `otherMode`, combiner, tiles, `rspLit`...). Las luces estimadas (sol) se agregan por proyección
   en `State::updateDrawStatusAttribute`/`finalize` (ver `rt64_state.cpp`, bloque "Add estimated sun light").
2. **WorkloadQueue::threadRenderFrame** (`src/hle/rt64_workload_queue.cpp` ~l.292): por cada frame renderizado
   (puede haber varios por frame del juego por interpolación):
   - `RSPProcessor` (compute `RSPProcessCS`): posiciones de pantalla N64 (`screenPosBuffer`, float4 con w),
     coordenadas de textura (`genTexCoordBuffer`), color sombreado (`shadedColBuffer`).
   - `VertexProcessor` (compute `RSPWorldCS`): posiciones/normales/velocidades de mundo (`worldPos/Norm/VelBuffer`).
     **Solo corre si `prevFrame.matched`** (requiere frame matching: `targetRate > 0` o RT activo).
   - `FramebufferRenderer::addFramebuffer` por cada par: recorre proyecciones y crea un `InstanceDrawCall` por llamada
     (`IndexedTriangles` para perspectiva/orto, `RegularRect`, `RawTriangles`, `FillRect`, `VertexTestZ`, `Raytracing`),
     agrupados en `RasterScene` (lista de índices) o `RaytracingScene`. `renderIndicesVector` e `instanceDrawCallVector`
     se llenan en el mismo orden: el índice de instancia es el `renderIndex` del push constant.
   - `endFramebuffers` (sube render indices/params, elige la escena RT), `recordSetup` (subidas, RSP, world, BLAS/TLAS),
     `recordFramebuffer` por par (`submitRasterScene` / `submitRaytracingScene`).
3. **Raster**: `submitRasterScene` (`rt64_framebuffer_renderer.cpp` ~l.608). Vertex buffers: pos (screenPos), uv,
   color; index buffer `faceIndices` (R32). Push constants `RasterParams { renderIndex, screenScale, screenOffset }`.
   Descriptor sets: 0 común (`FrParams`, `instanceRenderIndices`, `instanceRDPParams`, `RDPTiles`, `GPUTiles`,
   `DynamicRenderParams`, y en RT los buffers de mundo), 1 y 2 texturas (caché), 3 framebuffer (`FbParams`,
   `gBackgroundColor`, `gBackgroundDepth`).
   - Pipelines: shaders especializados por `ShaderDescription` compilados en runtime (`RasterShaderCache`) o el
     **ubershader** (`RasterShaderUber`, `DYNAMIC_RENDER_PARAMS`) mientras compilan. Blending **dual source**
     (`SV_TARGET0` color, `SV_TARGET1` factor), así que **no se puede agregar MRT** al pase raster.
   - Profundidad: `D32_FLOAT`, `DEPTH_WRITE` o `DEPTH_READ` (decals leen `gBackgroundDepth`). Al cambiar se rehace el
     framebuffer (`submitDepthAccess`). MSAA opcional (`Texture2DMS`).
   - `RasterVS` convierte la posición de pantalla N64 a NDC: `ndc = (screen - res/2) / (res.x/2, -res.y/2)`, luego
     `ndc * screenScale + screenOffset` (ajuste de aspecto/widescreen) y el viewport de hardware es todo el target.
4. **RT** (si está activo): la primera proyección perspectiva compatible del par con `depthWrite` se trazó en vez de
   rasterizarse; las proyecciones raster intermedias se renderizan como "interleaved rasters" en targets aparte y se
   componen (`ComposePS`), luego `PostProcessPS` al target real.
5. **VI** (`rt64_vi_renderer`, present queue): escala y presenta el framebuffer N64 elegido.

Convenciones de proyección N64 en RT64: `RSPProcessCS` hace `ndc = tfPos.xyz / (w, -w, w)` y
`screen = ndc * viewport.scale + viewport.translate`; la z de pantalla es la profundidad del hardware.

## 7. Path tracer existente (resumen)

Ver memoria `rt64-path-tracing` y el commit `aef5443` de `lib/rt64`. Archivos: `render/rt64_raytracing_{resources,shader_cache}`,
`shaders/RaytracingLibrary.hlsl`, `Ray.hlsli`, `TemporalAACS.hlsl`, `BloomCS.hlsl`, `res/bluenoise`. Solo Vulkan
(D3D12 colgaba, ver sección 9). Costo: ~9,8 ms/frame a 960x576 en la RTX 4070 SUPER con todos los efectos (demasiado:
el usuario lo apagó). Opciones de menú: Path Tracing On/Off (F2), Effects Off/Subtle/Full, Sky Enhanced/Original.
**Effects** y **Sky** también controlan la iluminación raster: la intensidad de los efectos escala el post (bloom,
rayos, CAS, grading, viñeta) y Sky Original apaga el cielo procedural raster (`getEnhancementIntensity()`,
`isProceduralSkyEnabled()` en `rt64_tuning.h`).

## 8. Portabilidad (otros decomps de N64 y ports de PS1)

Regla: **todo lo nuevo debe ser agnóstico al juego**.
- RT64 solo consume pistas genéricas del host: rotación/traslación mundo→vista (`setWorldViewRotation/Translation`),
  clave de escena (`setSceneKey`), "el sol requiere cielo de fondo" (`setSunRequiresSkyBackground`), "hay un cielo que
  RT64 no ve" (`setSkyBackgroundHint`, p. ej. porque el host lo quitó en VR), "dónde está el jugador"
  (`setFocusPosition`, en el espacio de la geometría), y las que se agreguen (luces de
  interiores, dirección del sol, etc.). Nada de direcciones de RAM ni ids de área en RT64.
- Para otro juego N64 en RT64: implementar en el host la lectura de su cámara/escena y llamar a las mismas APIs.
  Juegos que no hornean la cámara en las matrices ya tienen una matriz de vista válida en RT64.
- Para PS1 (otro renderer): reutilizar los shaders y algoritmos (shadow map estable, AO, composición, cielo,
  bloom, CAS) que deben quedar en archivos con entradas explícitas (profundidad, matrices, buffers de vértices) y sin
  dependencias del emulador del RDP. Documentar las entradas de cada pase en `enhanced-lighting.md`.

## 9. Problemas conocidos y lecciones

- `lightCounts` es uint8 y `lightIndices` uint16 por vértice (leerlos como uint32 provocó un bucle enorme y un TDR).
- El viewport RT debe usar ratios de clip unitarios (los del juego lo agrandaban: zoom).
- Niebla/transparencias del path tracer se mezclan en sRGB (`ComposePS`).
- DXC 1.7 en Vulkan declara imágenes RW como rgba32f: usar `[[vk::image_format]]`. También lista todos los payloads en
  cada entry point (parcheado al cargar en el shader cache).
- Primarios del path tracer nunca aceptan hits opacos directos (se perdían decals coplanares).
- Probar shaders nuevos del path tracer con `RT64_RT_PASSES=0x1` y `RT64_RT_SCALE=0.5` primero.
- `PrintWindow` captura el alfa del swap chain: por eso `shot.ps1` guarda en 24 bits.
- Cambiar el cálculo de jitter por píxel con pasos adaptativos hacía que TAA dejara estelas en las nubes.
- **Texturas escritas desde compute** necesitan `RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS`:
  Vulkan solo pone `VK_IMAGE_USAGE_STORAGE_BIT` con STORAGE y D3D12 solo permite UAV con UNORDERED_ACCESS; con uno solo
  las escrituras fallan en silencio en el otro backend (el AO salía negro).
- Buffers usados como vertex buffer (p. ej. `worldPosBuffer` en el shadow pass) necesitan `RenderBufferFlag::VERTEX`.
- Las proyecciones de RT64 están en convención de vector fila: en HLSL `mul(M, v)` con la matriz subida tal cual de
  hlslpp equivale a `v * M` en la CPU (`hlslpp::mul(v, M)`).
- El pase raster usa *dual source blending* (`SV_TARGET0/1`): no se le puede agregar otro render target (MRT); por eso
  los datos extra (normales) se obtienen volviendo a dibujar la geometría (*replay*).
- En Bash, los heredocs largos con comillas a veces fallan ("unexpected EOF"): escribir el script con la herramienta
  Write y ejecutarlo (`python archivo.py`). Hay un helper de parches en el scratchpad de la sesión (`patchlib.py`).
- **Finales de línea**: con `core.autocrlf=true` git normaliza los archivos guardados con LF en el índice, pero algunos
  de RT64 están guardados con CRLF (`git ls-files --eol`: `i/crlf`, p. ej. `rt64_framebuffer_renderer.cpp`). Si se
  editan con `sed -i` (Git Bash los deja en LF) el diff abarca el archivo entero: volver a CRLF antes del commit
  (`python -c` que reemplace `\n` por `\r\n`) y revisar `git diff --stat`. Los scripts de Python en modo texto
  escriben CRLF en Windows, que coincide.
- **plume D3D12, samplers inmutables**: `D3D12DescriptorSet` reservaba un hueco del heap de vistas por cada sampler
  inmutable, pero la root signature los excluye de las tablas: todos los descriptores posteriores quedaban corridos
  (G-buffer y AO rotos solo en D3D12). Corregido en `plume_d3d12.cpp` (commit de rt64 `6b8c2f1`). Es la causa más
  probable de que el path tracer colgara el GPU en D3D12 (sin verificar). La iluminación raster ya se ve igual en
  D3D12 y Vulkan.
- No crear texturas ni framebuffers en mitad de la grabación de un command list que todavía los usa en el frame
  (el `copyColor` de la iluminación los recreaba al cambiar de formato): crearlos en `finish()`, antes de grabar.
- **"Una superficie sale oscura"**: antes de buscar un bug, mirar `RT64_LIGHT_DEBUG 3` (sombra) y `7` (distancia al
  oclusor del shadow map: rojo = hay algo hacia el sol, azul = el píxel tiene normal guardada) y probar otra
  elevación del sol. La calle de la ciudad (warp 5) estaba a la sombra de un edificio detrás de la cámara con el sol
  a 22°: era correcto. Las normales de la vista 2 están en el espacio de la geometría (ojo PSX, +Y abajo): el suelo
  sale morado `(0,-1,0)`, no verde.
- `RT64_LIGHT_PRINT=1` imprime cada 120 frames las escenas de iluminación (rect, sol, cámara, casters, texel); si una
  superficie no está en ninguna escena, o hay más escenas de las esperadas, ahí se ve.
- Las técnicas en espacio de pantalla (GTAO, sombras de contacto) fallan con superficies vistas casi de canto: en Apple
  Market una pared junto a la cámara recibía una franja negra del marco de una puerta. Remedios aplicados: ignorar
  capas a menos de 4 unidades de la superficie, desvanecer el contacto cuando la luz roza la superficie, contacto de la
  linterna solo en suelos y un AO más suave (radio 120, fuerza 0,8). Comprobar siempre con `DEBUG 6` y `8`.
- Un "pixel shader caro" con MSAA casi siempre es por leer el depth buffer multisample: medir con
  `RT64_PRINT_FRAME_TIME=2` antes de optimizar a ciegas.
- **Barreras** (hallazgos de una revisión de código):
  - Los buffers subidos con `shaderUploader` (render indices, frame params, tablas nuevas) solo pasan a lectura en
    `commandListAfterBarriers`, al **final** de `recordSetup`: un compute del setup que los lea necesita su propia
    barrera antes del dispatch.
  - En D3D12, `SHADER_READ` con etapa `GRAPHICS` es `PIXEL_SHADER_RESOURCE`: si luego lo lee un compute, la barrera
    debe ser `GRAPHICS_AND_COMPUTE` (`ALL_SHADER_RESOURCE`). En Vulkan la etapa de destino también tiene que incluir
    compute. plume emite la barrera aunque no cambie el layout (Vulkan), así que repetirla con otra etapa es válido.
  - Un buffer persistente que se lee en frames siguientes (análisis del cielo) puede no haberse escrito nunca
    (VR, interiores, primer frame): marcar qué entradas son válidas y no leer las demás.
  - `State` tiene su propio `FramebufferRenderer` que graba en el hilo del juego: el estado global de herramientas como
    `gpuMarker` debe ser `thread_local`.
- `mm64_build.bat` usa 6 procesos (`MM64_BUILD_JOBS`); con 24, y luego con 12, clang-cl se quedó sin memoria (WSL ocupa 9 GB y los agentes corren waifu2x). Si aparece `LLVM ERROR: out of memory`, el exe NO se actualizó: revisar la salida antes de probar.
- Probado y descartado: un pre-pase que copiaba la profundidad (la muestra más lejana) a un depth buffer de una
  muestra para que el *replay* del G-buffer tuviera early-Z. En escritorio no ganó nada (el G-buffer no está limitado
  por overdraw) y en VR con MSAA 4x salió más caro: leer un depth buffer MSAA en un shader es caro.
