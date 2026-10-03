# Remake gráfico experimental de Mega Man 64: reemplazo de texturas y modelos

Investigación, factibilidad y plan para el experimento de "remake" de MegaMan64Recompiled + RT64, centrado en **reemplazo de assets** (texturas, modelos, materiales). La iluminación y el posproceso (sombras, AO, bloom, cielo procedural raster) se investigan aparte en `techniques-research.md` / `enhanced-lighting.md`; aquí solo aparecen donde se cruzan con los assets (mapas de normales, emisivos, follaje).

Fecha: 2026-10-02. Rama `vr` del repo principal y `mm64vr` de `lib/rt64` (commit `aef5443`). Las referencias `archivo:línea` apuntan a esos commits; `RecompiledFuncs/funcs_N.c:línea` es la salida de N64Recomp ya generada en el repo.

## 0. Resumen

- **Texturas HD: funcionan hoy sin tocar código.** RT64 identifica cada textura con un XXH3 de los bytes de TMEM (más las entradas de paleta usadas y los parámetros del tile), busca el hash en `rt64.json` y escala las coordenadas por la proporción entre la imagen nueva y la original. El recomp carga packs `.rtz` (o carpetas con `mod.json`, o `.nrm` con `rt64.json` dentro) desde `%LOCALAPPDATA%\MegaMan64Recompiled\mods\`. Para volcar texturas: `"developer_mode": true` en `graphics.json`, F1, pestaña Textures.
- **Herramientas listas en `C:\Users\Usuario\Devel\tools\upscale\`:** `texture_packer`/`texture_hasher` de RT64 compilados aparte; un decodificador de volcados de RT64 a PNG y un puerto en Python del hash y de las cargas de TMEM de RT64, verificados contra el C++ real (300/300 hashes y 300/300 secuencias de carga idénticas); generador de `rt64.json`/`mod.json`; pipeline de escalado por lotes con relleno circular, alfa limpio para recortes, BC7 con mips y mapas de normales.
- **Pack de todos los modelos generado sin jugar.** Los modelos de MM64 son display lists F3DEX2 precompiladas dentro de un archivo LZSS en la ROM (445 modelos). Reproduciendo sus cargas de textura con el código portado de RT64 se calcularon offline **2577 hashes** (1831 con la paleta propia de cada DL + variantes de paleta) y se armó un pack de prueba escalado con waifu2x: `C:\Users\Usuario\Devel\tools\upscale\rt64\mm64_hd_models_w2x\mm64_hd_models_w2x.rtz` (60 MB, zstd, con low mip cache). **Validado en el juego el 2026-10-02** (ver 2.10): el cuerpo de Mega Man, Data y los personajes salen con las texturas nuevas; las caras de Mega Man van en un pack aparte.
- **El terreno es el problema:** cada quad de terreno carga solo la ventana de texeles que usa (`LOADTILE`), así que un área produce cientos de "texturas" diminutas (607 a 1019 ventanas distintas por área, muchas de 2x3 texeles). Hay tres soluciones (sección 2.8): reproducir offline las DL de terreno y recortar cada ventana de una página escalada completa (recomendada), canonicalizar las cargas con un parche, o enseñarle a RT64 a reemplazar páginas completas.
- **Escaladores con licencia segura:** waifu2x-ncnn-vulkan (MIT, el más fiel: error 2,04/255), Real-ESRGAN ncnn (BSD-3/MIT), xBRZ/hqx (salidas libres), 4x-PixelPerfectV4 y NMKD Siax (WTFPL), PBRify (CC0). Evitar UltraSharp/UltraMix/Remacri (CC BY-NC-SA). En la RTX 4070 SUPER: 2577 texturas escaladas 4x en 10 s y convertidas a BC7 con mips en 74 s.
- **Reemplazo de modelos: el camino más factible es del lado del juego.** Cada hueso de cada actor se dibuja con un `gSPDisplayList(*(sp+0x14))` en `func_800805F8` (`0x80081218`): un `[[patches.hook]]` ahí puede sustituir la DL de cualquier (modelo, LOD, hueso) por una nueva en memoria extendida, conservando matrices, animación, interpolación, paletas y los hashes de textura. Los árboles son tarjetas cruzadas dentro de las DL de terreno y se pueden sustituir al cargar el área. RT64 no tiene reemplazo de modelos (su README dice "Details to be determined").
- **Top 5 para empezar:** (1) validar en el juego el pack offline de modelos; (2) pipeline offline de terreno por páginas; (3) árboles 3D procedurales (prototipo `tree_gen.py` listo); (4) cielo en HD; (5) materiales por hash (emisivos, reflejos) cargando automáticamente los presets que el path tracer ya soporta, y mapas de normales en los packs.
- **Hallazgos colaterales importantes:** el BSS de los parches termina en `0x810F2820`, dentro de la zona de mods/heap que empieza en `0x81000000` (corromperá cualquier `recomp_alloc` o mod de código); `mod_game_id = "mm"` es el mismo id que usa Zelda64Recomp para Majora's Mask; los destellos de daño reescriben la TLUT y cambian el hash (la textura HD "parpadea" a la original mientras dura el destello).

## 1. Alcance y fuentes

| Tema | Fuente principal |
|---|---|
| Reemplazo de texturas | Código de `lib/rt64` (caché de texturas, hasher, base de datos, shaders, depurador, herramientas) y del host (`src/main`, `src/game`, `lib/N64ModernRuntime`) |
| Formato de modelos y dibujo | `RecompiledFuncs/*.c`, `MegaMan64RecompSyms`, parches existentes (`patches/*.c`, `us.rev1.toml`) y lectura directa de la ROM (`MegaMan64.us.rev1.z64`) con herramientas propias |
| Precedentes | Repos y documentación públicos (URLs en cada fila) |
| Escaladores | LICENSE reales de cada repo, binarios descargados y probados en esta PC |

Reglas respetadas: no se modificó ningún archivo fuente del repo, no se ejecutó el juego, todo lo nuevo vive en `C:\Users\Usuario\Devel\tools\upscale\` (y este documento).

## 2. Reemplazo de texturas en RT64, de punta a punta

Todo lo de esta sección sale del código de este repositorio (fork `lib/rt64`, rama `mm64vr`, commit `aef5443`), no de documentación externa. Las rutas son relativas a la raíz del repo salvo que se indique otra cosa.

### 2.1 Cómo identifica RT64 una textura (el hash)

- **Dónde se calcula.** Cada vez que una llamada de dibujo usa un tile, `State::fullSyncFramebufferPairTiles` (`lib/rt64/src/hle/rt64_state.cpp:640-656`) sube la textura al caché: si el tile apunta a una copia de framebuffer no hay hash (es un *tile copy*), si el tile puede leer más bytes de los que caben en TMEM se usa la ruta *raw TMEM* y en el caso normal se llama a `TextureManager::uploadTexture` (`lib/rt64/src/hle/rt64_rdp_tmem.cpp:52-66`), que llama a `TMEMHasher::hash(...)` con la versión actual.
- **Algoritmo** (`lib/rt64/src/common/rt64_tmem_hasher.h:48-198`, `CurrentHashVersion = 5` en la línea 37): XXH3 de 64 bits sobre los **bytes de TMEM** (los 4 KB de memoria de texturas del RDP, no la RDRAM), en este orden:
  1. Los bytes de la textura desde `tile.tmem << 3`. Si la línea del tile (`line << 3`) es más ancha que los bytes realmente dibujados por fila, se hashea **fila por fila** (v2) y en las filas impares se respeta el intercambio de palabras de 32 bits que hace el hardware (v4). Si el rango pasa del final de TMEM, da la vuelta al principio.
  2. Las texturas RGBA32 hashean también la mitad alta de TMEM (el hardware guarda RG abajo y BA arriba).
  3. Si hay TLUT (texturas CI4/CI8 con paleta), se hashean **solo las entradas de paleta que la textura usa de verdad** (v5: se arma un bitset con los índices leídos, `rt64_tmem_hasher.h:72-85` y `145-181`). Con CI4 se usa la sub‑paleta `tile.palette` (16 entradas de 8 bytes en `0x800 + palette*0x80`); con CI8, las 256 entradas.
  4. Al final se agregan `width`, `height` (u16), `tlut` (u32: `0x8000` = RGBA16, `0xC000` = IA16), `line` (u16), `siz` y `fmt` (u8) (`rt64_tmem_hasher.h:190-195`).
- **Qué ancho y alto entran al hash.** No es el tamaño "real" de la imagen en RDRAM sino el área que el tile puede muestrear: `sampleWidth = min(ancho del tile si hay clamp, 1 << masks)` e igual para el alto (`rt64_state.cpp:284-291`). Consecuencias: la misma imagen cargada con otro tamaño de tile, otra máscara u otra paleta da **otro hash**.
- **Raw TMEM.** Si `requiresRawTMEM` es verdadero (el tile puede leer más de 4 KB, típico de máscaras o clamps enormes, `rt64_tmem_hasher.h:200-207`), el hash es simplemente XXH3 de los 4 KB completos más offset y tamaño (`rt64_rdp_tmem.cpp:26-37`) y la textura no se decodifica a una imagen; se puede reemplazar por hash, pero esos hashes son inestables (dependen de basura en TMEM) y la herramienta `texture_hasher` los ignora.
- **Fondos S2DEX.** Solo para microcódigo S2DEX (BG/sprites grandes) existe un segundo esquema que hashea la RDRAM (`lib/rt64/src/hle/rt64_rdp.cpp:687-716`). Mega Man 64 usa F3DEX2 (fifo 2.08) y L3DEX2, así que en la práctica todo pasa por el hash de TMEM.
- **Hashes Rice.** RT64 no los puede calcular en tiempo real; solo sirven para reutilizar packs viejos de otros emuladores mediante `texture_hasher --rice` (ver 2.6). Para este juego no hay packs Rice útiles conocidos, así que conviene trabajar directamente con hashes RT64.

### 2.2 Qué pasa en el renderer cuando hay reemplazo

- **Búsqueda.** El hilo de subida (`TextureCache::uploadThreadLoop`, `lib/rt64/src/render/rt64_texture_cache.cpp:968-1259`) llama a `addResolvedPaths` (`:1289-1313`), que vuelve a hashear con la versión de cada base de datos cargada (si un pack viejo usa v3 o v4, RT64 recalcula ese hash para él) y recorre los packs **del último al primero**; el primero que tenga el hash gana.
- **Carga.** Según la operación (`stream`, `preload`, `stall`) el archivo se lee en un hilo de streaming (`:406-454`), se decodifica (DDS con `ddspp`, PNG con `stb_image`, `:932-966`) y se sube a la GPU. Mientras llega la versión completa se usa la del *low mip cache* si existe (`:1123-1160`).
- **Escala de coordenadas.** Al asociar el reemplazo se guarda `textureScale = ancho_reemplazo / ancho_original` y lo mismo en alto (`:280`). En el shader, `uvCoord *= gpuTile.tcScale` (`lib/rt64/src/shaders/TextureSampler.hlsli:315`) y el clamp/wrap/mirror se emula con las máscaras multiplicadas por esa escala (`TextureSampler.hlsli:73-95`). Por eso el reemplazo debe ser un **múltiplo entero** del tamaño de muestreo original y con la misma relación de aspecto (2x, 4x, 8x).
- **Medio texel.** Con `shift: "half"` el shader suma 0,5 texel antes de escalar (`TextureSampler.hlsli:310-313`); es lo correcto para texturas con filtrado bilineal. Para texturas con *point sampling* o rectángulos en modo copy (HUD, texto, cielo en rects) conviene `"none"`; el propio depurador propone `None` en esos casos (`lib/rt64/src/gui/rt64_debugger_inspector.cpp:1225-1229`).
- **Mipmaps.** Solo los DDS aportan mipmaps (los PNG siempre tienen 1 nivel). Con mipmaps RT64 usa samplers nativos con filtrado anisotrópico cuando el direccionamiento del tile lo permite, y si no, dos niveles de muestreo RDP (`TextureSampler.hlsli:345-395`).
- **Memoria.** El pool de reemplazos es 2/3 de la VRAM dedicada, mínimo 512 MB (`lib/rt64/src/hle/rt64_application.cpp:383-385`); con la RTX 4070 SUPER (12 GB) son unos 8 GB. Las texturas sin uso se expulsan por LRU; las precargadas y el low mip cache quedan fijos.
- **Interacción con el path tracer de este fork.** Cuando un tile está reemplazado (`highRes`), se desactivan el suavizado de texturas de baja resolución (`smoothMagnify`, `TextureSampler.hlsli:339`) y la simulación de baja precisión de UV. El relieve derivado del brillo de la textura (`bumpStrength`) toma muestras a 1 texel **original** de distancia (`lib/rt64/src/shaders/RaytracingLibrary.hlsl:286-292`), así que con texturas 4x el relieve sigue siendo grueso: es un argumento a favor de soportar mapas de normales reales (sección 5).
- **Comparar A/B.** Con el modo desarrollador activo, **F4** activa o desactiva todos los reemplazos (`rt64_application.cpp:722-725`). Ojo: dentro del depurador, F4 también es "Pause".

### 2.3 Formato del pack

Un pack es una carpeta o un zip con extensión `.rtz` que contiene `rt64.json`, las imágenes y opcionalmente `rt64-low-mip-cache.bin` (nombres en `lib/rt64/src/common/rt64_replacement_database.cpp:12-17`).

```json
{
    "configuration": {
        "autoPath": "rt64",
        "configurationVersion": 3,
        "hashVersion": 5,
        "defaultOperation": "stream",
        "defaultShift": "half"
    },
    "operationFilters": [ { "wildcard": "hud/*", "operation": "preload" } ],
    "shiftFilters":     [ { "wildcard": "hud/*", "shift": "none" } ],
    "extraFiles": [ "mod.json", "thumb.png" ],
    "textures": [
        { "hashes": { "rt64": "ca4777b14312d6d2", "rice": "" }, "path": "terrain/apple_market_bricks" },
        { "hashes": { "rt64": "de2d56d57253468d", "rice": "" }, "path": "", "shift": "none", "operation": "preload" }
    ]
}
```

- `configuration` (`rt64_replacement_database.cpp:309-329`): `hashVersion` debe ser ≤ 5 o el pack se ignora en silencio (`rt64_texture_cache.cpp:1520`). Si `configurationVersion < 3`, el `defaultShift` por defecto pasa a ser `none` (compatibilidad con packs viejos).
- `textures`: cada entrada mapea un hash a una ruta **sin extensión** (si se pone, se ignora). RT64 busca primero `.dds` y luego `.png` (`:199-222`). **Toda textura reemplazada tiene que estar listada**: `autoPath` solo sirve para encontrar el archivo cuando `path` está vacío (con `"rt64"`, el archivo se debe llamar `<hash>.dds/png`; con `"rice"`, con el formato de nombres de Rice), pero no agrega entradas por sí solo (`:148-243`).
- `operationFilters` / `shiftFilters`: comodines `*` y `?` sobre la ruta relativa, se aplican en orden y no pisan lo definido en una entrada (`:112-146`).
- `extraFiles`: archivos que `texture_packer` mete en el `.rtz` con deflate en vez de zstd. Es la forma correcta de incluir `mod.json` y `thumb.png`, porque el cargador de mods (`lib/N64ModernRuntime/librecomp/src/mod_manifest.cpp:82-102`, miniz) no sabe descomprimir zstd.
- **Formatos de imagen.** DDS con cualquier formato que mapee `toRenderFormat` (`rt64_texture_cache.cpp:581-733`): RGBA8, BC1-BC7, flotantes, etc.; solo texturas 2D con `arraySize == 1` (`:747`). PNG se carga como RGBA8 sin mipmaps. Recomendación: **BC7_UNORM con mipmaps** (`texconv -f BC7_UNORM -m 0`). No usar variantes `_SRGB`: el pipeline de RT64 trabaja con los colores del N64 tal cual, y un formato sRGB los linealizaría en el sampler.
- **El `.rtz`.** Es un zip normal; RT64 acepta entradas sin compresión, deflate y zstd (método 93) (`lib/rt64/src/common/rt64_filesystem_zip.cpp:80-95, 137-149`). Dentro del zip las rutas son sensibles a mayúsculas (`makeCanonical` no hace nada, `:170-173`), por eso `texture_packer` valida mayúsculas antes de empaquetar.
- **Low mip cache.** Archivo con los mips pequeños (≤ 96x96) de todos los DDS en streaming, para que no haya *pop‑in* (`lib/rt64/src/tools/texture_packer/texture_packer.cpp:163-271`). `--create-pack` **falla si no existe**, incluso en packs solo PNG (en ese caso el archivo queda vacío y no pasa nada).
- **Bug menor encontrado.** Al guardar desde el depurador, `to_json(ReplacementShiftFilter)` escribe la clave `"operation"` en vez de `"shift"` (`rt64_replacement_database.cpp:379-382`), así que un `shiftFilters` editado y guardado desde "Save directory" pierde su valor al recargar. Si se usan filtros de shift, conviene editarlos a mano en el JSON.

### 2.4 Cómo carga los packs MegaMan64Recompiled

- **Carpeta de mods:** `%LOCALAPPDATA%\MegaMan64Recompiled\mods\` (`program_id` en `include/zelda_config.h:10`, ruta en `src/game/config.cpp:145-166`, subcarpeta `mods` en `lib/N64ModernRuntime/librecomp/src/recomp.cpp:84-104`). Ya existe y hoy está vacía.
- **Registro:** `src/main/main.cpp:773-783` registra un tipo de contenido "texture pack" cuyo archivo marcador es `rt64.json` (con `allow_runtime_toggle = true`) y el contenedor `.rtz`, que **no exige manifiesto**. Sin `mod.json`, el id del mod es el nombre del archivo sin extensión (`mod_manifest.cpp:762-797`).
- **Otras formas válidas:** una **carpeta** dentro de `mods` también es un mod, pero ahí `mod.json` es obligatorio (`lib/N64ModernRuntime/librecomp/src/mods.cpp:814-830`); y un `.nrm` (mod de código) puede incluir `rt64.json` en su raíz y se detecta como pack a la vez (el contenedor `.nrm` acepta todos los tipos de contenido, `mods.cpp:932-933`). Esto permite publicar un mod de código y sus texturas en un solo archivo.
- **Campos obligatorios de `mod.json`:** `game_id` (= `"mm"`, el `mod_game_id` registrado en `src/main/main.cpp:384-395`), `id`, `display_name`, `version`, `authors` y `minimum_recomp_version` (≤ `"0.9.1"`, la versión del proyecto en `src/main/main.cpp:68`) (`mod_manifest.cpp:530-600`).
- **Activación y prioridad:** al arrancar, todos los mods encontrados se activan (`src/main/main.cpp:788-807`, "TODO load all mods"); también existe el menú de mods heredado de Zelda64Recomp (`src/ui/ui_mod_menu.cpp`). Al cambiar algo, `RT64Context::check_texture_pack_actions` (`src/main/rt64_render_context.cpp:539-597`) recarga todos los packs activos ordenados para que **el mod que está más arriba en la lista gane** ante un mismo hash.
- **Interruptor por mod:** si un mod declara en su `config_schema` una opción enum de dos valores con id `_recomp_texture_pack_enabled` (`include/zelda_render.h:17`), el usuario puede encender o apagar sus texturas HD desde la configuración del mod (`rt64_render_context.cpp:619-646, 662-690`).

### 2.5 Modo desarrollador, volcado y reemplazo en vivo

1. Con el juego cerrado, editar `%LOCALAPPDATA%\MegaMan64Recompiled\graphics.json` y poner `"developer_mode": true` (`src/game/config.cpp:115, 130`; el valor llega a RT64 en `src/main/rt64_render_context.cpp:306`). Hoy está en `false`.
2. En el juego, **F1** abre el inspector de RT64 (los atajos se instalan con un hook de ventana solo en modo desarrollador, `rt64_application.cpp:599-690`). F2 activa o desactiva el raytracing, F3 muestra la RDRAM y F4 los reemplazos.
3. Pestaña **Textures** (`rt64_state.cpp:2337-2460`): "Load pack" / "Load directory" / "Load packs" / "Load directories", "Save directory", "Start dumping textures", "Remove unused entries" y "Unload textures".
4. **Volcado:** "Start dumping textures" pide una carpeta y, mientras está activo, escribe por cada hash nuevo (`rt64_rdp_tmem.cpp:68-190`):
   - `<hash>.v5.tmem`: los 4096 bytes de TMEM.
   - `<hash>.v5.tile.json`: el tile (fmt, siz, line, tmem, palette, cms/cmt, máscaras, shifts, uls/ult/lrs/lrt), `width`, `height` y `tlut`.
   - `<hash>.v5.rice.rdram/.rice.json` y, con paleta, `.rice.palette.rdram/.json`: solo sirven para calcular hashes Rice.
   RT64 **no genera PNG** de los volcados (solo decodifica en la GPU); para eso está `rt64_dump_to_png.py` (ver 2.6).
5. **Reemplazo en vivo:** cargar el pack como **una sola carpeta** (no `.rtz`, no varias), pausar (botón Pause), hacer clic derecho sobre el objeto para listar las llamadas de dibujo, desplegar "Texture tile #n" y pulsar **Replace**; la imagen debe estar dentro de la carpeta del pack. Ahí mismo están "Copy hash", el combo de Shift/Operation y "Dump TMEM" (`rt64_debugger_inspector.cpp:255-350, 1184-1245`). Luego "Save directory" escribe `rt64.json` (deja `rt64.json.old` como respaldo, `rt64_texture_cache.cpp:1591-1632`).

### 2.6 Herramientas (compiladas y probadas en esta PC)

| Herramienta | Qué hace | Estado |
|---|---|---|
| `texture_packer.exe` (RT64) | `--create-low-mip-cache` y `--create-pack [--deflate] [--store] [--threads n]` | Compilada aparte con clang-cl en 18 s, sin tocar el árbol de build del juego: `C:\Users\Usuario\Devel\tools\upscale\rt64\bin\` (script `build_tools.bat`). Probada: acepta el `rt64.json` generado y produce un `.rtz` con zstd. |
| `texture_hasher.exe` (RT64, GPL por el código de Rice) | `<carpeta> --rice` (agrega hashes Rice a partir de volcados) y `--upgrade` (recalcula hashes de una base vieja) | Compilada en la misma carpeta. Para este juego casi no hace falta: los volcados ya vienen nombrados con el hash RT64. |
| `hash_check.exe` | Calcula el hash v5 de volcados usando el `rt64_tmem_hasher.h` real | Solo para validar el puerto a Python. |
| `rt64_tmem.py` | Decodificador de TMEM (puerto 1:1 de `TextureDecoder.hlsli`) y puerto del hash v5 | **300 de 300** hashes aleatorios idénticos a los del C++ (`selftest.py`), decodificación ida y vuelta correcta en RGBA16, RGBA32, IA8, I4, CI4+RGBA16 y CI8+IA16. |
| `rt64_dump_to_png.py` | Volcado → `<hash>.png` + `index.csv` (formato, paleta, wrap S/T, espejo, tipo de alfa, colores, duplicados exactos), vistas previas escaladas y, con `--group`, subcarpetas `wrap\ mirror\ cutout\ clamp\` según los modos de direccionamiento reales del tile | Probado con volcados sintéticos. |
| `rt64_tmem.py` (cargas) | Puerto de `loadWord` / `loadToTMEMCommon` / `loadBlock` / `loadTile` / `loadTLUT` de `lib/rt64/src/hle/rt64_rdp.cpp:368-587` | **300 de 300** secuencias aleatorias de cargas producen la misma TMEM que una copia literal del código de RT64 (`bin\load_check.cpp`, `test_loads.py`). |
| `mm64_model_hashes.py` | Lee los 445 modelos de la ROM (con `..\mm64_extract\mm64arc.py`), reproduce las cargas de TLUT y textura de cada DL con el estado base del juego (TLUT RGBA16) y escribe `<hash>.png` + `model_hashes.csv` (modelo, paleta, desplazamientos, tamaño, hash) | 13084 dibujos, **2577 hashes únicos** (1831 con la paleta de la DL) en 10,6 s. Las imágenes decodificadas son las texturas correctas del juego (`model_hashes_sheet.png`). |
| `make_rt64_pack.py` | Genera `rt64.json` y `mod.json` a partir de imágenes nombradas con su hash (más alias por CSV) y opcionalmente un `.rtz` con deflate | Probado; `texture_packer` acepta la base generada. |
| `demo_pipeline.py` | Demo de punta a punta sin el juego: volcado sintético → PNG → escalado → `rt64.json` → BC7 con `texconv` (con `--keep-coverage 0.5` para el recorte del árbol) → `texture_packer` | Corre en ~1,3 s; salida en `C:\Users\Usuario\Devel\tools\upscale\rt64\demo_out\`. |

Todos los scripts están en `C:\Users\Usuario\Devel\tools\upscale\rt64\` y necesitan Python 3.12 con `pillow`, `numpy` y `xxhash` (ya instalados).

**Pack de prueba de todos los modelos (ya generado, sin probar en el juego):**

```
cd C:\Users\Usuario\Devel\tools\upscale\rt64
python mm64_model_hashes.py model_hashes_out
python ..\scripts\batch_pipeline.py model_hashes_out mm64_hd_models_w2x --method w2x-cunet --scale 4 --noise 0 --wrap clamp --alpha auto --dds --keep-coverage
python make_rt64_pack.py mm64_hd_models_w2x --id mm64_hd_models_w2x --name "MM64 HD models (waifu2x, experimental)" --author "Randy Gil" --version 0.0.1
bin\texture_packer.exe mm64_hd_models_w2x --create-low-mip-cache
bin\texture_packer.exe mm64_hd_models_w2x --create-pack
```

Resultado: `mm64_hd_models_w2x\mm64_hd_models_w2x.rtz` (60 MB, 2577 DDS BC7 con mips, low mip cache de 26 MB, `mod.json` con deflate). Comparación antes/después: `C:\Users\Usuario\Devel\tools\upscale\rt64\model_w2x_compare.png`. Para probarlo: con el juego cerrado, copiar el `.rtz` a `%LOCALAPPDATA%\MegaMan64Recompiled\mods\`, entrar a cualquier zona con personajes y alternar con F4 (modo desarrollador). Si una textura no cambia, comparar en el inspector (F1, clic derecho sobre el personaje, "Texture tile #0", "Copy hash") contra `model_hashes.csv`. (Se corrigió de paso `scripts\batch_pipeline.py` para llamar a `texconv` por tandas: con 2577 archivos la línea de comandos superaba el límite de Windows.)

### 2.7 Receta para armar un pack de Mega Man 64

**Modelos (personajes, enemigos, objetos, cuerpo de Mega Man):** no hace falta volcar nada; usar `mm64_model_hashes.py` como en 2.6. Solo las caras/bocas de Mega Man (se descomprimen cada cuadro en los segmentos 4/5) y las variantes teñidas de los destellos requieren volcado en el juego.

**Todo lo demás (terreno, cielo, HUD, menús, efectos):**

1. **Preparar el modo desarrollador** (2.5, paso 1) y crear `C:\mm64hd\dump\` y `C:\mm64hd\pack\`.
2. **Volcar:** F1 → Textures → "Start dumping textures" → `C:\mm64hd\dump`. Recorrer el juego (Apple Market, ciudad, ruinas, Sub-Cities, jefes, menús, HUD). El volcado es incremental y se puede repetir en varias sesiones; conviene archivarlo.
3. **Convertir a PNG agrupado por modo de direccionamiento:**
   `python C:\Users\Usuario\Devel\tools\upscale\rt64\rt64_dump_to_png.py C:\mm64hd\dump C:\mm64hd\png --group --scale 4 --verify`
   En `index.csv`, `alpha = cutout` marca recortes, `same_pixels_as` agrupa hashes con píxeles idénticos (pueden compartir archivo vía `--alias-csv`), y las ventanas de terreno aparecen como cientos de PNG minúsculos (ver 2.8).
4. **Escalar** cada grupo con las opciones que corresponden (sección 3):
   ```
   cd C:\Users\Usuario\Devel\tools\upscale\scripts
   python batch_pipeline.py C:\mm64hd\png\wrap   C:\mm64hd\pack\wrap   --method w2x-cunet --scale 4 --wrap wrap   --dds
   python batch_pipeline.py C:\mm64hd\png\mirror C:\mm64hd\pack\mirror --method w2x-cunet --scale 4 --wrap mirror --dds
   python batch_pipeline.py C:\mm64hd\png\cutout C:\mm64hd\pack\cutout --method w2x-cunet --scale 4 --wrap clamp --alpha cutout --dds --keep-coverage
   python batch_pipeline.py C:\mm64hd\png\clamp  C:\mm64hd\pack\clamp  --method w2x-cunet --scale 4 --wrap clamp --dds
   ```
   Para HUD y texto, `--method xbrz` o el `.pth` de PixelPerfectV4 con `run_pth_model.py`; para un look remasterizado del terreno, HDCube o Siax. Los archivos conservan el nombre `<hash>.png/.dds`; se pueden mover a subcarpetas temáticas siempre que el nombre empiece con el hash.
5. **Generar la base de datos y el manifiesto:**
   `python C:\Users\Usuario\Devel\tools\upscale\rt64\make_rt64_pack.py C:\mm64hd\pack --id mm64_hd_textures --name "Mega Man 64 HD Textures" --author "Tu nombre" --version 0.1.0`
   Si hay HUD/fuentes, agregar a mano en `rt64.json` un `shiftFilters` `{"wildcard": "hud/*", "shift": "none"}` (y no guardarlo luego desde el depurador, por el bug de 2.3).
6. **Probar en vivo** cargando `C:\mm64hd\pack` con "Load directory" en el inspector (permite corregir hashes, shift y operación en caliente y guardar).
7. **DDS:** `batch_pipeline.py --dds` ya escribe BC7_UNORM con mips. Para imágenes retocadas a mano: `C:\Users\Usuario\Devel\tools\upscale\bin\texconv.exe -nologo --ignore-srgb -f BC7_UNORM -m 0 -y -r:keep -o C:\mm64hd\pack C:\mm64hd\pack\*.png` (`--keep-coverage 0.5` para recortes; `--ignore-srgb` evita conversiones de gamma si el PNG trae metadatos sRGB).
8. **Empaquetar:**
   `C:\Users\Usuario\Devel\tools\upscale\rt64\bin\texture_packer.exe C:\mm64hd\pack --create-low-mip-cache` y luego `... --create-pack` → `C:\mm64hd\pack\pack.rtz`.
9. **Instalar:** con el juego cerrado, copiar el `.rtz` a `%LOCALAPPDATA%\MegaMan64Recompiled\mods\` (o arrastrarlo al instalador de mods). Durante el desarrollo, una carpeta con `mod.json` dentro de `mods` también sirve y evita reempaquetar.

### 2.10 Validación en el juego y caras de Mega Man (2026-10-02)

- **Pack de modelos:** instalado en `mods`, el juego lo activa solo al arrancar. En una corrida en el punto de guardado
  85 de las 474 texturas que se usaron estaban en el pack (el resto es terreno, cielo, HUD y efectos) y todas se
  reemplazaron: el pecho, las hombreras y la placa de la espalda de Mega Man pasan de píxeles borrosos a bordes limpios,
  y Data (el mono) pierde los dientes de sierra de las orejas.
- **Caras:** los ojos y las bocas de Mega Man se descomprimen en cada cuadro (`func_8008498C` → dos llamadas a
  `func_80028D30`) en dos búferes: ojos en `0x801FF3B8` y bocas en `0x801D7FC8`, CI4 de 64x32 (las bocas traen el
  número de cada una en las esquinas, que no se ven). Para volcarlas todas sin jugar: `RT64_DUMP_TEXTURES=<carpeta>`
  (RT64 vuelca desde el arranque, como "Start dumping textures") y `MM64_FACE_CYCLE=1` (el parche de `draw_megaman`
  muestra cada índice de ojos y boca dos cuadros, hasta la cantidad de `0x800CE54C`). En ~10 s salen 18 ojos y 23 bocas;
  se identifican por la dirección de RDRAM del `.rice.json` de cada volcado. Pack: `mm64_hd_faces_w2x.rtz` (41
  texturas, 825 KB), escaladas con waifu2x como el cuerpo para que combinen.
- **waifu2x contra Real-ESRGAN anime (x4plus_anime_6B)** en las caras: waifu2x respeta el dibujo (contornos grises,
  colores originales); Real-ESRGAN queda más nítido pero cambia el arte (contornos negros gruesos, una boca de color
  se vuelve una línea negra). Para MM64 se eligió waifu2x. Los modelos de Upscayl para anime (Remacri, UltraMix) tienen
  licencia CC BY-NC-SA y los escaladores por difusión inventan detalle distinto en cada textura: descartados.
- **Terreno (`mm64_hd_terrain_all_w2x.rtz`, 163 MB):** en el bosque casi no cambia (el pasto ya es borroso) y los
  arbustos de bloques de píxeles quedan con aspecto de acuarela. No se deja instalado.
- Los packs listos están en `Downloads\MegaMan64Recompiled-HD-Packs\` (se instalan copiándolos a
  `%LOCALAPPDATA%\MegaMan64Recompiled\mods\` con el juego cerrado; se apagan desde el menú de mods).

### 2.8 Advertencias de texturas HD (propias de MM64 y generales)

**Propias de Mega Man 64** (de la lectura del código y de la ROM, sección 4):

- **Terreno fragmentado en ventanas.** Edificios, calles y dungeons son tiles de terreno cuyas DL hacen un `LOADTILE` por quad con **solo la ventana de texeles que usa** de una página de texturas estilo VRAM de PSX (`SETTIMG` CI 8 bits sobre la página entera; búsqueda de páginas por (x, y, w, h) en `func_8002C9BC`, `RecompiledFuncs/funcs_15.c:6023`). Un área tiene 1745 cargas y 607 ventanas distintas; otra, 1386 cargas y 1019 ventanas, muchas de 2x3 texeles. Para RT64 cada ventana es una textura con su propio hash. Opciones:
  1. **Replay offline + página completa (recomendada):** igual que con los modelos, reproducir las DL de terreno de cada área desde la ROM (tablas en `0x800AC750` / `0x800AC7F4`, cargador `func_80035CA0`, `RecompiledFuncs/funcs_2.c:3954`), calcular el hash de cada ventana, escalar **la página entera** una sola vez y recortar de ella la imagen de cada ventana (x4). No toca el juego y evita costuras porque todas las ventanas salen de la misma imagen. Requiere reconstruir el estado que pone el código de terreno (`func_8007E8D4`, `funcs_23.c:429`, y las DL de material estáticas en `0x800BDA30`–`0x800BDB58`). 3 a 5 días.
  2. **Canonicalizar cargas con un parche:** al cargar el área, reescribir cada `LOADTILE` para que cargue un bloque alineado fijo de la página que contenga la ventana (ajustando `SETTILESIZE`), de modo que muchos quads compartan pocos hashes estables. Menos entradas en el pack, pero toca el render del juego.
  3. **Reemplazo por página en RT64:** generalizar el mecanismo de S2DEX (`RDP::loadTileReplacementCheck`, `rt64_rdp.cpp:687-716`, hash de RDRAM) para buscar la página completa y desplazar las coordenadas por el origen de la ventana. El más general (serviría a otros juegos) y el más caro.
- **Destellos de daño:** cuando el tinte del actor no es neutro, `func_800805F8` copia y tiñe la TLUT cada cuadro (`0x80080B78`–`0x80080D10`), así que el hash cambia y durante el destello se ve la textura original. Arreglo posible del lado del juego: teñir con el color primitivo o de entorno del combiner en vez de reescribir la paleta.
- **Caras y bocas de Mega Man:** se descomprimen cada cuadro (`func_8008498C`, `funcs_9.c:1961`) en los segmentos 4/5; cada expresión es un hash estable, pero hay que volcarlas en el juego.
- **Modelos con UV animadas** (ids 0xFD, 0x126–0x128, 0x167) reescriben sus vértices cada cuadro; no afecta al hash.
- **Cielo:** `func_8002E8FC` (`funcs_21.c:5714`) dibuja rectángulos de texturas CI4 cargadas con `LOADBLOCK` (pool 3); son franjas que deben escalarse como un panorama único y recortarse igual.

**Generales:**

- **Tamaño y aspecto:** el reemplazo se escala respecto del tamaño de muestreo (`sampleWidth x sampleHeight`), no del tamaño de la imagen fuente. Si el tile es más grande que la imagen, el PNG decodificado muestra basura en los bordes y el reemplazo debe conservar esa disposición.
- **Factores enteros:** el wrap/mirror usa `round(mask * tcScale)`; con escalas no enteras aparecen costuras o saltos al repetir.
- **Costuras entre texturas vecinas:** con filtrado bilineal y clamp, cada textura se filtra sola. Las piezas que el juego dibuja lado a lado (cielo en franjas, paneles de HUD, mosaicos de terreno con texturas distintas) deben escalarse juntas y recortarse después, o se verán bordes.
- **Paletas:** el hash incluye las entradas de paleta usadas. Variantes de color (enemigos recoloreados, el mismo muro con otra paleta) son hashes distintos; si el juego anima la paleta o la oscurece con fundidos al estilo PSX, cada estado es un hash nuevo y no es práctico reemplazarlos todos.
- **Texturas que no se pueden reemplazar:** *tile copies* (efectos que leen el framebuffer), texturas *raw TMEM*, texturas generadas por la CPU cada cuadro y cualquier cosa que cambie de contenido continuamente.
- **Alfa:** el recorte lo decide el modo de dibujo del juego (`alpha compare` con umbral, o `cvg_x_alpha` con MSAA, `lib/rt64/src/shaders/RasterPS.hlsl:186-210`). El alfa escalado debe quedar binario y sin halos (sangrar el color dentro de las zonas transparentes antes de escalar). El path tracer trata como follaje los recortes verticales sin luz (`RaytracingLibrary.hlsl:226-240`) y les agrega viento y detalle procedural, y eso se mantiene con las texturas nuevas.
- **Shift:** texturas bilineales → `half`; HUD, fuentes, rects en modo copy y texturas con point sampling → `none`.
- **Formatos sRGB:** no usarlos (ver 2.3).
- **Pop-in:** usar `stream` con low mip cache; `preload` solo para HUD o texturas pequeñas que aparecen de golpe.
- **Versiones del hash:** si en el futuro el upstream sube `CurrentHashVersion`, `texture_hasher --upgrade` recalcula los hashes de la base usando los volcados guardados, así que **conviene archivar los volcados**.

### 2.9 Materiales: lo que ya existe y lo que falta

- **El formato de pack de RT64 solo reemplaza el color (albedo + alfa).** No hay mapas de normales, rugosidad ni emisivos en `rt64.json` (tampoco en upstream: no hay issues ni PRs al respecto).
- **Pero el fork ya tiene un sistema de materiales del path tracer, sin uso:** con `RT_ENABLED`, `State` mantiene bibliotecas de presets (`lib/rt64/src/hle/rt64_state.h:123-130`): *draw calls* identificadas por `DrawCallKey` (hasta 8 hashes de TMEM + combiner + othermode + geometry mode, con máscaras; `lib/rt64/src/preset/rt64_preset_draw_call.h`), *materiales* con `ExtraParams` (`lib/rt64/src/shared/rt64_extra_params.h`: `selfLight` = emisivo, `reflectionFactor`, `roughnessFactor`, `refractionFactor`, `specularColor/Exponent`, `uvDetailScale`, `lightGroupMaskBits`, etc.) y *luces puntuales* que se pueden adjuntar a un material y siguen la matriz del objeto (`rt64_state.cpp:1614-1626` y `:1732-1760`). Se editan en las pestañas "Materials", "Draw calls" y "Lights" del inspector y se guardan con "Save library" (`lib/rt64/src/preset/rt64_preset.h:42-115`), pero **no se cargan solos al arrancar**.
- **Propuesta mínima:** que `loadReplacementDirectories` lea también un `rt64-materials.json` opcional del pack (mismo esquema que esas bibliotecas) y lo vuelque en `drawCallLibrary` / `materialLibrary` / `lightsLibrary`. Con eso un pack podría declarar, por hash, qué texturas emiten luz (antorchas, pantallas, lava), cuáles reflejan (agua, metal pulido) y qué luces acompañan a un objeto, sin tocar el juego. Es la versión RT64 del `texture_mods.json` de sm64rt.
- **Mapas de normales/rugosidad:** requieren cambios en el renderer: campos nuevos por entrada (`"normal"`, `"orm"`, `"emissive"` o una convención `<ruta>_n`, `<ruta>_orm`, `<ruta>_e`), cargar esas texturas junto con el albedo, un índice más por `GPUTile` y usarlos en `RaytracingLibrary.hlsl` en lugar del relieve por luminancia (que hoy mide 1 texel *original*, `:286-292`, y queda grueso con texturas 4x). En el raster original no aportan nada (el combiner del N64 no ilumina por píxel); sirven para el path tracer y para la iluminación raster mejorada que se está diseñando en paralelo (ver `docs/rendering/ROADMAP.md`).

## 3. Herramientas de escalado (probadas en esta PC)

Todo quedó en `C:\Users\Usuario\Devel\tools\upscale\` (binarios en `bin\`, modelos en `models\`, scripts en `scripts\`, comparaciones en `samples\`). No se tocó el entorno global de Python: los paquetes extra (spandrel, hqx, torchvision) están en `pylib\` (instalados con `pip --target`).

### 3.1 Licencias (leídas de los LICENSE / cabeceras reales)

Regla general: la licencia del **programa** (GPL, AGPL, LGPL) no se transmite a las imágenes que produce. Lo que puede restringir el uso de las salidas es la licencia de los **pesos** del modelo (algunos son CC BY-NC-SA, es decir, no comerciales y con obligación de compartir igual).

| Herramienta / modelo | Código | Pesos | ¿Salidas publicables en un pack? |
|---|---|---|---|
| Real-ESRGAN (xinntao/Real-ESRGAN) | BSD-3-Clause | Publicados en el mismo repo sin licencia aparte (OpenModelDB los lista como BSD-3) | Sí |
| realesrgan-ncnn-vulkan (xinntao/Real-ESRGAN-ncnn-vulkan) | MIT (ncnn: BSD-3) | Los de Real-ESRGAN | Sí |
| waifu2x-ncnn-vulkan (nihui) | MIT | Modelos de nagadomi/waifu2x, MIT | Sí |
| Upscayl (app y backend upscayl-ncnn) | AGPL-3.0 | Mixtos (abajo) | Depende del modelo |
| chaiNNer | GPL-3.0 (solo la herramienta) | — | Sí |
| spandrel (cargador de modelos de chaiNNer) | MIT | — | Sí |
| texconv / DirectXTex | MIT | — | Sí |
| xBRZ | GPL-3.0 (el wrapper xbrz.py es AGPL-3.0) | — | Sí |
| ScaleFX, Super-xBR (libretro/slang-shaders) | Cabeceras estilo MIT | — | Sí |
| hqx | LGPL-2.1 | — | Sí |

Modelos que trae Upscayl: **evitar** UltraSharp, UltraMix y Remacri (CC BY-NC-SA 4.0); "High Fidelity" es 4xHFA2k (CC BY 4.0, exige atribución); "Digital Art" es literalmente el archivo de Real-ESRGAN x4plus-anime (BSD-3); "Standard" y "Lite" no documentan licencia (evitar). En `upscayl/custom-models` cada modelo tiene la suya: NMKD Siax y Superscale (WTFPL), general v3 y animevideov3 (BSD-3), LSDIR, Nomos8kSC y HFA2k (CC BY 4.0), uniscale_restore (NC-SA, evitar), unknown-2.0.1 (desconocida, evitar).

Modelos permisivos de OpenModelDB (verificados contra su `data/models/*.json`): pixel art: **4x-PixelPerfectV4** (WTFPL), 1x-ArtClarity (WTFPL), 4x-Skyrim-Alpha (CC0); texturas de juego: 4x-GameAI-2-0 y 4x-Ground (WTFPL), 4x-TextureDAT2-otf (CC BY 4.0), **PBRify_Remix** (CC0, confirmado en su repo). **4x-HDCube**: OpenModelDB dice CC0, pero el LICENSE del repo es propio: el autor no reclama derechos sobre las salidas (se pueden usar), pero el modelo no se puede redistribuir. **No usar** para un pack público: 1x-N64clean, el modelo 4x-xbrz, Faithful, Fatal-Pixels, UltraSharp ni Remacri (NC o NC-SA).

**Para publicar un pack sin dudas legales:** waifu2x (MIT), Real-ESRGAN (BSD-3), xBRZ/hqx (salidas libres), PixelPerfectV4/Siax (WTFPL), PBRify (CC0); con atribución, HFA2k / LSDIR / TextureDAT2 (CC BY 4.0).

### 3.2 Binarios descargados y verificación de GPU

- `bin\realesrgan-ncnn-vulkan-20220424-windows.zip` (Real-ESRGAN v0.2.5.0) → `bin\realesrgan\realesrgan-ncnn-vulkan.exe` con los modelos x4plus, x4plus-anime y animevideov3.
- `bin\waifu2x-ncnn-vulkan-20250915-windows.zip` → `bin\waifu2x\waifu2x-ncnn-vulkan-20250915-windows\`.
- `bin\texconv.exe` y `bin\texdiag.exe` (DirectXTex, may2026).
- `bin\xbrz\xbrz.dll` (compilado desde las fuentes de xBRZ con `g++ -O2 -std=gnu++17 -shared -static`).
- Modelos extra: `models\ncnn\` (Siax, LSDIRCompactC3, HFA2k) y `models\pth\` (HDCube, HDCube-Compact, PixelPerfectV4, PBRify UpscalerSPANV4 / NormalV3 / RoughnessV2 / Height), con SHA256 iguales a OpenModelDB. Los `.pth` corren con spandrel sobre el torch 2.9.1 (solo CPU) ya instalado.
- Ambas herramientas ncnn detectan la GPU con `-g 0`: `[0 NVIDIA GeForce RTX 4070 SUPER] fp16-p/s/a=1/1/1` (driver 616.92, 12 GB).

Comandos probados:

```
bin\realesrgan\realesrgan-ncnn-vulkan.exe -i IN -o OUT -n realesrgan-x4plus-anime -s 4 -g 0 -f png
bin\waifu2x\waifu2x-ncnn-vulkan-20250915-windows\waifu2x-ncnn-vulkan.exe -i IN -o OUT -m models-cunet -n 0 -s 4 -g 0 -f png
bin\realesrgan\realesrgan-ncnn-vulkan.exe -i IN -o OUT -m models\ncnn -n 4x_NMKD-Siax_200k -s 4 -g 0
```

(La última ejecuta un modelo personalizado de Upscayl directamente con el binario de Real-ESRGAN.)

### 3.3 Tiempos medidos

Tiempo de reloj por proceso; cada proceso paga un costo fijo de arranque, por eso conviene procesar carpetas enteras.

| Motor / modelo | Costo fijo por proceso | Por imagen de 64 px | Por imagen de 256 px |
|---|---|---|---|
| Real-ESRGAN x4plus | 1,8 s | 43 ms | 602 ms |
| Real-ESRGAN x4plus-anime | 1,0 s | 13 ms | 168 ms |
| animevideov3 x4 | 0,74 s | 2,6 ms | 33 ms |
| waifu2x cunet x4 | 0,68 s | 4,6 ms | 87 ms |
| waifu2x anime x2 | 0,58 s | 0,9 ms | 8 ms |
| `.pth` tamaño ESRGAN (CPU) | — | 0,35 s | — |
| `.pth` SPAN / Compact (CPU) | — | 0,06–0,09 s | — |
| xBRZ (CPU) | — | 0,08 s | — |

Otros datos: la primera ejecución en frío fue más lenta (x4plus sobre 220 px: 3,33 s; waifu2x: 2,46 s); `texconv` BC7 con todos los mips de una textura de 1024² tarda 0,55 s en GPU y 11,1 s en CPU; la cadena completa de 5 texturas (escalado + DDS + normales) tardó 1,9 s. Un pack de ~2000 texturas pequeñas se procesa en minutos.

### 3.4 Calidad y fidelidad

Fidelidad medida como error absoluto medio (0–255) al reducir el resultado al tamaño original; más bajo = más fiel al arte original, no necesariamente más bonito:

| Método | Error |
|---|---|
| hqx | 1,97 |
| waifu2x cunet n0 x4 | 2,04 |
| waifu2x anime n0 x4 | 2,14 |
| xBRZ | 2,55 |
| PixelPerfectV4 / PBRify | 6,5 |
| animevideov3 | 6,7–6,9 |
| Real-ESRGAN x4plus | 7,75 |
| Real-ESRGAN x4plus-anime | 8,33 |
| HDCube (inventa detalle nuevo) | 8,58 |

- **Real-ESRGAN x4plus / x4plus-anime** aplanan el césped ruidoso de 32 px en un verde liso e inventan grietas oscuras; x4plus-anime además vuelve verde azulado el gris de las juntas.
- **waifu2x cunet** conserva el arte original y lo afila: es la opción "fiel".
- **HDCube y PBRify** agregan detalle de material creíble al terreno: es la opción "remasterizada".
- **PixelPerfectV4** deja íconos y texto muy limpios, casi vectoriales, pero empasta las texturas ruidosas.

Comparaciones visuales: `samples\compare_best.png`, `samples\compare_<textura>.png`, `samples\compare_pth_models.png`, `samples\compare_upscayl_custom_ncnn.png`, `samples\seam_zoom.png`, y las carpetas `samples\seamless\`, `samples\foliage\` y `samples\normals\`.

### 3.5 Scripts y técnicas

| Script (`scripts\`) | Qué resuelve |
|---|---|
| `batch_pipeline.py` | Carpeta de PNG → carpeta con las mismas rutas relativas; un solo proceso de GPU para todo; relleno circular o espejo para texturas que se repiten; alfa separado; DDS BC7 con mips (`texconv -f BC7_UNORM -m 0 -gpu 0`, con `-wrap` para las que se repiten); `--keep-coverage`; `--normals`. |
| `seamless_upscale.py` | Texturas que se repiten: rellena 8 píxeles del original con wrap (o `--mirror`), escala y recorta a exactamente k×tamaño. El índice de costura (diferencia en la costura ÷ diferencia típica entre vecinos; ~1 = invisible) bajó de 6,91 a 2,80 (ladrillo, x4plus), de 3,29 a 1,52 (césped, waifu2x) y de 19,98 a 1,05 (césped, x4plus-anime). |
| `foliage_alpha.py` | Recortes (árboles, rejas): meter RGBA tal cual en las herramientas ncnn da alfa suave y un halo oscuro (el brillo del borde cae a 0,71 del interior). Rellenando antes los texeles transparentes con el color vecino, escalando el alfa aparte con xBRZ y volviendo a umbralizar a 0,5, el borde queda en 0,97 y con contorno suave. |
| `normal_from_albedo.py` | Mapa de normales por Sobel sobre la luminancia (consciente del wrap, intensidad ajustable) y una heurística de rugosidad. PBRify NormalV3 / RoughnessV2 (CC0, 0,13 s por 256² en CPU) dan resultados más creíbles; ojo: sus normales usan la convención DirectX (verde invertido). |
| `run_pth_model.py` | Cualquier `.pth` de OpenModelDB vía spandrel. |
| `compare_models.py`, `benchmark.py`, `make_test_textures.py`, `upscale_lib.py` | Comparaciones, tiempos, texturas sintéticas de 5 bits por canal y utilidades (incluye xBRZ y hqx). |

`--keep-coverage 0.5` importa para recortes finos: la cobertura de una brizna de pasto en los mips cayó de 0,32 a 0 sin la opción y se mantuvo entre 0,36 y 0,5 con ella.

Integración con las herramientas RT64 de la sección 2: `rt64_dump_to_png.py --group` separa los PNG en `wrap\`, `mirror\`, `cutout\` y `clamp\` usando los modos de direccionamiento **reales** del tile (no la heurística de bordes), y cada carpeta pasa por `batch_pipeline.py` con las opciones correctas. Probado de punta a punta (volcado sintético → waifu2x → BC7 → `rt64.json` → `texture_packer` → `.rtz` con zstd, y `mod.json` con deflate): ver `C:\Users\Usuario\Devel\tools\upscale\rt64\demo_out\pipeline_w2x_compare.png`.

### 3.6 Recomendación por categoría

| Categoría | Modelo | Notas |
|---|---|---|
| Terreno y muros (fiel) | waifu2x cunet `-n 0 -s 4` | Siempre con relleno circular |
| Terreno y muros (aspecto remasterizado) | 4x-HDCube o NMKD Siax | Siempre con relleno circular; revisar a mano |
| Follaje en tarjetas | waifu2x cunet (o HDCube para el color) | Rellenar color en zonas transparentes, alfa con xBRZ + umbral 0,5, `--keep-coverage 0.5` |
| HUD, texto, íconos | 4x-PixelPerfectV4 o xBRZ | `shift: none` en el pack |
| Personajes | waifu2x cunet/anime, ruido 0–1 | Evitar modelos entrenados con fotos; x4plus-anime solo si se busca un look cel-shading |
| Cielo y degradados | animevideov3, o waifu2x con ruido 1–2 | Reduce el bandeado de 5 bits |

### 3.7 Trampas conocidas

- 4x de una textura de 32 px sigue siendo 128 px: para superficies que se ven de cerca (primera persona, VR) encadenar dos pasadas para llegar a 8x.
- Real-ESRGAN trata el *dithering* y el moteado como ruido y puede cambiar los tonos.
- Preescalar 2x con *nearest* antes de ESRGAN ayuda a íconos y texto pero destruye las texturas ruidosas.
- Los texeles transparentes de CI4 son negros: sin rellenarlos aparecen halos oscuros.
- Texturas con direccionamiento espejo necesitan relleno simétrico.
- Cada variante de paleta tiene otro hash y necesita su propia versión escalada.
- Recortar siempre a exactamente k× el tamaño original (RT64 escala las coordenadas por la proporción).
- BC7_UNORM, nunca la variante SRGB.
- El modo por mosaicos de ncnn (`-t`) no importa con texturas tan pequeñas.

## 4. Reemplazo de modelos

### 4.1 Cómo guarda Mega Man 64 sus modelos

El decomp (`lib/MegaMan64Decomp`) apenas tiene 4 archivos C, así que todo esto sale de `RecompiledFuncs`, de los parches existentes y de leer la ROM directamente con `C:\Users\Usuario\Devel\tools\upscale\mm64_extract\mm64arc.py` (lector de archivos + LZSS) y `export_model.py` (exportador OBJ + PNG). El formato se verificó contra los 445 modelos (`models_survey.json`).

- **Microcódigo:** F3DEX2 fifo 2.08 (`gspF3DEX2_fifoTextStart` en `0x800A4770`, cadena en la ROM en `0xAC988`; L3DEX2 también está). `draw_pools` carga uno de dos microcódigos según el pool (`patches/gfx_patches.c:233-241`: `0xA4770` = F3DEX2 y `0xA5B00`, probablemente L3DEX2). El caché de 32 vértices solo aplica al hardware real: RT64 acepta hasta 127 vértices por `G_VTX` y tiene 256 ranuras (`lib/rt64/src/hle/rt64_rsp.h:27`).
- **Compresión:** LZSS clásico de Okumura (N=4096, F=18, anillo desde `0xFEE`, cabecera de 4 bytes con el tamaño). Decodificador `func_80026C80` (`RecompiledFuncs/funcs_15.c:7638`); cargador de entradas de archivo `func_80028D30(idx, tabla, base, dst)` (`funcs_22.c:2373`), entradas `{u32 offset:24, u32 size}` con el bit 31 = comprimido.
- **Archivo de modelos:** ROM `0x2CAC60`, índice en `0x800BDF70`, 445 entradas; id de modelo → índice de archivo en la tabla `0x800C2DF4` (8 bytes por id). Cargador `func_80081398` (`funcs_16.c:10431`); `func_8007FB78` (`funcs_2.c:7223`) arma tres estructuras "LOD model" `{u8 type=1; u16 modelId @+2; u8 lodVariant @+4; void* blob @+8}` en `handle+0x70[0..2]`.
- **Blob de un modelo** (todo relativo al blob; cada bloque se mapea a un segmento):

| Offset de cabecera | Contenido | Segmento al dibujar |
|---|---|---|
| +04 / +08 | Bloque de display lists (offset / tamaño) | 0 |
| +0C / +10 | Arreglo de `Vtx` | 2 |
| +14 / +18 | Datos de textura CI4 | 1 |
| +1C / +20 | Tabla de huesos | — |
| +24 / +28 | Paletas RGBA5551 de 32 bytes (16 colores) | 3 (+ paleta × 32) |

- **Tabla de huesos:** `u32 count`, `u8 n`, `u8 variante→conjunto de DL[n]`, una tabla de punteros `G_DL` por hueso (offsets en el segmento 0) y registros de 8 bytes por hueso (flag de culling, flag de prim, modo, flag de tex‑edge, RGB de prim).
- **DL de un hueso** (ejemplo real, modelo 22, hueso 0): carga de paleta `SETTIMG seg3` + `SETTILE t7 tmem 0x100` + `LOADTLUT` de 16 colores; textura completa `SETTIMG CI 16b seg1` + `LOADBLOCK` (2048 bytes, `dxt 0x200`); `SETTILE` de render CI4 `line 4` con clamp en S y T y sin máscaras; `SETTILESIZE 0,0–63,63` (64x64); luego `G_VTX` del segmento 2 y `TRI1`/`TRI2`, y `ENDDL`. Cada submalla con otra textura repite la secuencia (otro desplazamiento de paleta, p. ej. `0x20`, `0x120`).
- **Números de los 445 modelos:** mediana de 133 vértices (máx. 3080, 149.227 en total) y 90 triángulos (máx. 1452, 89.585 en total); 242 modelos tienen un hueso y 56 usan el rig humanoide de 22 huesos (máx. 32); lotes `G_VTX` de 3 a 31 vértices; texturas casi siempre de 32x32 (914 de 1831) o 64x64 (624) CI4; de 1 a 19 paletas por modelo; solo 3 modelos fijan su propio combiner o render mode. El campo `cn` de los vértices trae valores tipo normal en algunos modelos y ceros en otros, pero la iluminación del RSP nunca se activa (coincide con `HANDBOOK.md` §5).
- **Animación:** solo rígida, una matriz por hueso compuesta en la CPU (`func_800306F0` / `func_800308E8`); sin mezcla de vértices. `func_8002C5A4` (parcheado en `patches/terrain_seams.c`) traspone y desplaza la traslación, `func_8002C420` (`funcs_0.c:7088`) convierte 4.12 a s15.16 con Y hacia abajo. El *bind pose* no está en el blob (los vértices ya vienen en el espacio de cada hueso).

### 4.2 Cómo se convierten en comandos en tiempo de ejecución

- **Cola de tareas:** `func_800283A8` encola en un pool (parcheada en `patches/gfx_patches.c:28`); `func_80028220` es la variante ordenada por profundidad; `func_800281C8` es el *bump allocator* por cuadro. Los parches ya agrandan el arena y las DL x16 (`gfx_patches.c:106-137`; límite de desborde `0x1D800*16` en `:227`), envuelven cada tarea en `recomp_interp(tag)` → `gEXMatrixGroup` (`:269-275`) y activan `gEXSetRDRAMExtended(TRUE)` cada cuadro (`:414`).
- **Actores:** `func_8003A2EC` (`funcs_11.c:8791`) descarta por el origen proyectado y elige LOD 0/1/2 por profundidad (umbrales `0x2BD`, `0x709`); por cada hueso compone la matriz PSX y llama a `func_800817BC` (`funcs_18.c:1855`, parcheada en `patches/tag_actors.c:193-231`), que reserva un `ActorBuffer` de 0x68 bytes y encola `func_800805F8`:

| Offset de `ActorBuffer` | Campo |
|---|---|
| +0 | hueso |
| +2 | modo de segmento |
| +4 / +8 | textura dinámica / TLUT dinámica |
| +C | LOD |
| +E | alfa |
| +10 | paleta (variante) |
| +14 | handle → LOD model |
| +18 | `Mtx` |
| +64 | tinte RGB555 (`0x4210` = neutro) |
| +66 / +67 | desplazamiento de UV |

- **`func_800805F8`** (`funcs_31.c:6578`) emite, en orden: `G_MTX` (modelview), chequeo de modelos especiales (`func_80082FA4`, `funcs_31.c:8625`, caras), `gSPSegment` 0/2/1/3 (el 3 = `blob + cabecera[0x24] + paleta*32`, verificado en `0x800808E8`–`0x800809AC`), reescritura opcional de UV, copia teñida de la TLUT si el tinte no es neutro, geometry mode `ZBUF|SHADE|FOG|SMOOTH` + cull‑back (o sin culling), color prim, render mode (FOG_SHADE_A + AA_ZB_OPA_SURF2 / TEX_EDGE2 / XLU_SURF2), combiner color = TEXEL0, alfa = TEXEL0 (× alfa prim al desvanecer), `gSPTexture 0x8000` y, **en `0x80081218`, `gSPDisplayList(*(sp+0x14))`** seguido de `gSPSegment(0, 0)` (verificado en el desensamblado). El estado base viene de `func_8008128C` (`funcs_26.c:1086`): 2 ciclos, TLUT RGBA16, bilineal.
- **Mega Man:** `func_80039FE0` (parcheada en `patches/mouse_camera.c:639`) → `func_80084870` (`funcs_20.c:3125`, pool 6) → el mismo `func_800805F8`. Huesos: 0 torso, 1 cabeza, 2-4 brazo derecho, 5-7 izquierdo, 8 cadera, 9-11 y 12-14 piernas (`HANDBOOK.md` §5). Cara y boca (`func_8008498C`) descomprimen texturas cada cuadro en los segmentos 4/5/3.

### 4.3 Terreno, edificios, árboles y cielo

- **Edificios y dungeons son tiles de terreno** de 512 unidades (`D_801B5088` `Tile[1710]`). Archivos por área: tipo `0x12` = arreglo de `Vtx` con colores horneados (segmento 5); tipo `0x11` = registros de tile de 0x28 bytes con 3 LOD, grupos de material de 0x14 bytes (flags + 4 offsets de DL) y sus DL; tipos `0x00/0x01` = páginas de textura estilo TIM. `func_8003912C` (`funcs_3.c:1611`) arma buffers de 0x48 bytes (`Mtx` + flags) en los pools 4/5/8; `func_8007E8D4` (`funcs_23.c:429`) emite por tile: `G_DL 0x800BD9D8` (estado base, combiner SHADE×TEXEL0, alfa prim 0x80), segmento 5 = vértices, segmentos 2/3/4 = DL de material estáticas (`0x800BDA30` opaco, `0x800BDA80` tex‑edge, `0x800BDB08` tex‑edge a dos caras, `0x800BDB30` opaco a dos caras, `0x800BDB58` translúcido), `G_MTX` y las DL de grupo. Las texturas del terreno se cargan por ventanas (2.8).
- **Árboles:** están dentro de las DL de grupo del terreno, después de `G_DL 0x03000000` (material tex‑edge a dos caras) y hasta `G_DL 0x04000000`. En el área g0 hay 282 triángulos recortados de 5396; en g4, 346, de los cuales 187 son planos verticales (tarjetas). Se identifican por: sección del segmento 3 + par de quads verticales cruzados + su ventana de textura. Del lado del renderer: tag `TERRAIN(x,z)` + render mode TEX_EDGE + sin culling + `cvgXAlpha` (el path tracer ya los detecta así, `RaytracingLibrary.hlsl:226`).
- **Objetos estáticos y efectos** (`func_8003AB60` y la tabla `0x800AC980`) pasan por el mismo camino de huesos que los actores.
- **Cielo:** `func_8002E8FC` (`funcs_21.c:5714`) dibuja rectángulos CI4 en el pool 3, antes de la proyección 3D.

### 4.4 Memoria y API de mods

| Rango | Uso |
|---|---|
| `0x80000000`–`0x803FFFFF` | El juego (solo usa 4 MB; nunca carga constantes ≥ `0x80400000`). Heap del juego `0x802592E0`–`0x803B5000` (~1,36 MB, áreas y modelos, `func_80025E3C`, `funcs_30.c:1750`); framebuffers en `0x803B5000`. |
| `0x80400000`–`0x807FFFFF` | **Libre** (los 4 MB del Expansion Pak que el juego no usa). |
| `0x80800000` | Handles de PI del runtime (`lib/N64ModernRuntime/librecomp/include/librecomp/addresses.hpp:13-18`). |
| `0x80801000`– | Código y datos de los parches (`patches/patches.ld`). |
| `0x81000000`– | Mods, y a continuación el heap de `recomp_alloc` (o1heap) hasta 512 MB (`addresses.hpp:20`, `src/heap.cpp:33-42`, `src/recomp.cpp:662,688`). |

- **RT64 y direcciones:** con RDRAM extendida activa, las direcciones K0 (`0x8xxxxxxx`) pasan sin máscara; las segmentadas y físicas se enmascaran a 16 MB (`lib/rt64/src/hle/rt64_rsp.cpp:110-127`, `rt64_rdp.cpp:239-246`). Datos por encima de 16 MB deben referenciarse con direcciones absolutas `0x8xxxxxxx`.
- **Bug latente:** el BSS de los parches termina en `0x810F2820` (`patches/patches.map`, `sVrTaskBackups` de 3 MB y el estado de `mouse_camera`), **pasado `mod_rdram_start = 0x81000000`**, y el heap se inicializa en `0x81000000` cuando no hay mods (`recomp.cpp:688`). El `ASSERT` de `patches/patches.ld:18` compara contra `0x80801000 + 16 MB` en vez de `0x81000000`. Hoy no explota porque nada llama a `recomp_alloc`, pero cualquier mod de código o uso del heap lo corromperá. Arreglo: achicar `sVrTaskBackups` o reservarlo con `recomp_alloc` al iniciar, y cambiar el `ASSERT` a `. < 0x81000000`.
- **API disponible para los parches de este repo:** `RECOMP_PATCH`, `[[patches.hook]]` e `[[patches.instruction]]` en `us.rev1.toml`, funciones del host vía `patches/syms.ld` (`0x8F0000xx`) registradas en `src/main/register_patches.cpp:8-13`. Para mods `.nrm` (N64ModernRuntime): `mod.json` (`mod_manifest.cpp:174-186`), `mod_binary.bin`, `mod_syms.bin`, `patch.bps` opcional (solo un mod puede traer uno, `mods.cpp:254-256, 1626-1652`), DLL nativas (`mods.cpp:99-339`) y secciones `patch`, `force_patch`, `export`, `event`, `import`, `callback`, `hook`, `hook_return`; los hooks se aplican recompilando en vivo la función enganchada (`mods.cpp:1800, 2053`). Exportaciones base: `recomp_alloc` / `recomp_free`, `recomp_get_mod_folder_path`, la API de configuración y `recomputil_*`.
- **Advertencia:** `mod_game_id = "mm"` (`src/main/main.cpp:389`) es el mismo id que usa Zelda64Recomp para Majora's Mask, así que el menú aceptaría mods de MM (y al revés). Conviene cambiarlo a algo propio (p. ej. `"mm64"`) antes de publicar mods.

### 4.5 Opciones para reemplazar modelos

**(a) Del lado del juego (parches) — muy factible, recomendado para empezar.**

- **(A1) Cambiar la DL de un hueso:** `[[patches.hook]] func = "func_800805F8_5B9F8"`, `before_vram = 0x80081218`. Claves: `MEM_HU(2, r22)` = modelId, `MEM_BU(4, r22)` = LOD, `MEM_HU(0, r18)` = hueso. Si la clave está en una tabla, escribir en `MEM_W(0x14, sp)` un puntero K0 a la DL nueva. Los segmentos 1 y 3 siguen apuntando a la textura y la TLUT originales, así que las variantes de paleta, los destellos y los hashes del pack HD siguen funcionando; la interpolación también, porque el cambio ocurre dentro de la misma tarea y del mismo grupo de matrices. Variante sin costo por cuadro: escribir los punteros K0 en la tabla de huesos justo después de que `func_80081398` carga el modelo.
- **(A2) Cambiar el blob completo:** apuntar el `+8` de la estructura LOD a un blob nuevo con el mismo formato (mismos huesos y en el mismo orden) en `0x80400000`–`0x807FFFFF` o en memoria de `recomp_alloc`. Permite texturas y paletas nuevas; los lotes pueden ser de hasta 127 vértices en RT64.
- **(A3) Árboles:** después de la carga de tipo `0x11` (`func_80035CA0`, `0x800361B0`–`0x800361D8`), recorrer las secciones del segmento 3 buscando tarjetas cruzadas, reemplazar un `TRI2` por un `G_DL` a una malla de árbol en espacio local del tile y los demás por `G_NOOP`. Mallas grandes pueden necesitar ampliar los límites de culling (actores en `0x8003A41C`–`0x8003A440`; el rango del terreno ya está parcheado en `us.rev1.toml`).
- **Texturas para mallas nuevas:** reutilizar los segmentos 1/3, o poner datos nuevos (≤ 4 KB de TMEM) en una dirección K0 y usar una textura "marcador" pequeña y única que el pack HD reemplace por hash (el patrón de [mm-mirror-shield-emblems](https://github.com/keanine/mm-mirror-shield-emblems)).

| Tarea | Esfuerzo | Riesgo |
|---|---|---|
| Cambiar un objeto (A1) | 1–2 días | Bajo |
| Mega Man suavizado (subdivisión offline por hueso, bordes fijos) | ~1 semana | Medio (costuras en articulaciones; cara/boca usan los segmentos 4/5) |
| Árboles procedurales (A3) | 3–5 días | Medio |
| Pipeline completo de remodelado | Semanas | Medio–alto (sin *bind pose* en el blob: hay que capturar las transformaciones de los huesos en tiempo de ejecución) |

**(b) Del lado del renderer (RT64) — posible, pero todo por hacer.** RT64 no tiene reemplazo de modelos: su README lo lista como función en desarrollo ("Model replacements. Details to be determined.") y describe el plan: "Asset replacement will leverage the fact that only one contiguous vertex and index array is used by RT64… model replacement will just consist of allocating chunks of the buffer directly for replacements in a native format" ([README](https://github.com/rt64/rt64)). Habría que: identificar la llamada (hash de vértices + hashes de textura, o el id de `gEXMatrixGroup`), cargar mallas de un "model pack", insertarlas en los buffers del workload con la transformación de la llamada original (`RSP::setVertexCommon` / `drawIndexedTri`, `rt64_rsp.cpp:502-1156`, que ya admite índices globales crudos), y cubrir raster, path tracer (BLAS), interpolación y depurador. Ventaja: formato moderno (glTF), materiales por píxel, sin límites del N64, reutilizable en otros juegos. Costo: 3 a 6 semanas.

**(c) Híbrido — el mejor objetivo a largo plazo.** El hook de `0x80081218` (o el de los árboles) no cambia geometría sino que emite una clave semántica (modelId, hueso, LOD), por ejemplo con un comando GBI extendido nuevo o codificada en el id del grupo de matrices, y RT64 sustituye esa llamada por una malla del pack. Cada tarea ya tiene un id estable de `gEXMatrixGroup` (`TAG_ACTOR` + hueso, partes de Mega Man `0x4D4547xx`, cara `0x4D454743`, `TAG_TERRAIN(x,z)`, etc., `patches/tag_helper.h`), pero esos ids dependen de la ranura del actor en memoria, no del tipo de modelo: por eso conviene la clave semántica.

### 4.6 Recomendación

1. Empezar por **(A1)/(A3)**: son baratos, usan infraestructura que ya existe (hooks de `us.rev1.toml`, memoria libre, RDRAM extendida activa) y conservan todo lo demás (animación, interpolación, VR, paletas, packs HD).
2. Construir el conversor **OBJ/glTF → DL de MM64** sobre `export_model.py` (ida) para tener la vuelta; con eso, mallas suavizadas o remodeladas entran por (A1)/(A2).
3. Diseñar (c) solo cuando haya arte nuevo (con materiales) que justifique salir del formato del N64.

### 4.7 Cómo lo resuelven otros proyectos

| Proyecto | Mecanismo de reemplazo de modelos | Qué se puede copiar para MM64 |
|---|---|---|
| **Zelda64Recomp – ModelReplacer** ([repo](https://github.com/YAZ64MT/ModelReplacer), `src/zproxy_manager.c` l.143-190) | Hook de entrada y salida en `DmaMgr_ProcessRequest`; cuando el juego termina de copiar un objeto a RAM, sobrescribe cada display list registrada con un `gSPBranchList` hacia una DL "proxy" en memoria del mod. Prioridad según el orden de mods. | El patrón "redirigir la DL original a una DL nueva en memoria extendida" encaja con el modelo de tareas de MM64. |
| **Zelda64Recomp – PlayerModelManager / GlobalObjects / FSModels** ([PMM](https://github.com/YAZ64MT/PlayerModelManager), [GlobalObjects](https://github.com/YAZ64MT/GlobalObjects), [FSModels](https://github.com/YAZ64MT/PlayerModelManager_FSModels)) | API `registerModel` / `setSkeleton` / `setDisplayList`; el esqueleto debe tener los mismos huesos que el original. GlobalObjects carga objetos con `recomp_alloc` y reescribe las direcciones segmentadas (`G_DL`, `G_VTX`, `G_SETTIMG`) a punteros globales. FSModels usa una DLL nativa ("extlib") para leer `.zobj` desde `appdata`. | Reemplazo por hueso respetando la jerarquía original (MM64 también anima con matrices rígidas por hueso); cargar mallas desde archivos externos con una extlib. |
| **Zelda64Recomp – 3D Grottos** ([grottos.c](https://github.com/laurhinch/mm-3d-grottos/blob/main/src/grottos.c)) | Genera geometría (anillos) en tiempo de ejecución y la emite en lotes de `gSPVertex`. | Precedente directo de los árboles procedurales generados por código. |
| **Zelda64Recomp – Textured Stars / 3DItems / mirror shield** ([stars](https://github.com/danielryb/MMRecompTexturedStars/blob/main/src/textured_stars.c), [3DItems](https://github.com/garrettjoecox/ProxyMM_RecompMods), [emblems](https://github.com/keanine/mm-mirror-shield-emblems)) | El recomp base exporta funciones para sustituir la DL del cielo de estrellas; 3DItems cambia la función de dibujo de un actor; el escudo inyecta texturas SD desde código y trae un `rt64.json` que mapea **el hash de esas texturas SD** a DDS en HD. | 1) Exponer "hooks de contenido" propios (cielo, árboles) para que mods externos los usen. 2) Texturas nuevas: inyectar una versión SD pequeña y darle la HD por hash. |
| **Banjo: Recompiled – Asset Expansion Pak** ([repo](https://github.com/DarioSamo/BKRecompAssetExpansionPak), CC0) | `RECOMP_PATCH` de `assetcache_get` y una API `register_replacement(asset_id, ptr)` que sirve assets en formato nativo desde un heap propio (o1heap de 8 MB). | Reemplazo por **id de asset** (estable) en vez de por dirección. En MM64 el equivalente es identificar el modelo por archivo/desplazamiento dentro de sus datos. |
| **SoH / 2Ship2Harkinian** ([Shipwright](https://github.com/HarbourMasters/Shipwright), [2s2h](https://github.com/HarbourMasters/2ship2harkinian), [libultraship](https://github.com/Kenix3/libultraship), [retro](https://github.com/HarbourMasters64/retro/blob/main/docs/texture-packs.md)) | El decomp portado referencia cada asset por ruta (`__OTR__objects/...`); libultraship resuelve `CRC64(ruta)` en archivos `.otr/.o2r` y los posteriores pisan a los anteriores; prefijo `alt/` para assets alternativos activables con Tab; opcodes propios (`G_DL_OTR_HASH`, `G_VTX_OTR_HASH`, `G_SETTIMG_OTR_HASH`). Modelos hechos con Blender + fork de fast64. Texturas HD con escala entera. | Funciona porque hay un decomp completo con assets extraídos. MM64 no lo tiene (ver 4.1), así que este camino no aplica todavía. |
| **Starship (SF64)** ([repo](https://github.com/HarbourMasters/Starship), [Torch](https://github.com/HarbourMasters/Torch)) | Igual que SoH: `.o2r` en `mods/`; hay cambios de naves en GameBanana. | Ídem. |
| **Perfect Dark (port de PC)** ([repo](https://github.com/fgsfdsfgs/perfect_dark)) | Reemplaza archivos y segmentos de la ROM por archivos externos (`files/`, `segs/`, `--moddir`); texturas como binarios N64; sin HD en upstream. | Reemplazo "a nivel de archivo de datos": útil si se decodifica el formato de MM64. |
| **sm64coopdx – DynOS / Render96** ([dynos_mgr_actor.cpp](https://github.com/coop-deluxe/sm64coopdx/blob/main/data/dynos_mgr_actor.cpp) l.198-245, [Render96ex](https://github.com/Render96/Render96ex)) | Mapa "geo layout original → GraphNode personalizado"; los packs se compilan desde la salida de fast64. | Mapa de puntero/ID original → modelo nuevo, con packs de datos externos. |
| **RT64 upstream** ([README](https://github.com/rt64/rt64)) | "Model replacements. Details to be determined." El plan escrito: reservar trozos del buffer único de vértices/índices para mallas de reemplazo en formato nativo, reutilizar las transformaciones de la llamada original y dejar que el compute del RSP las procese. No hay issues sobre normal maps ni PBR. | Si se implementa en nuestro fork, seguir ese diseño facilita converger con upstream. |
| **sm64rt (RT64 legacy)** ([repo](https://github.com/DarioSamo/sm64rt), MIT) | `texture_mods.json` con `normalMapMod`, `specularMapMod`, `reflectionFactor`, `refractionFactor`, `selfLight`, `uvDetailScale` por textura; luces adjuntas a objetos en `geo_layout_mods.json`. | Esquema listo para materiales (normal/rugosidad/emisivo) en nuestros packs, cambiando la clave por el hash RT64. |
| **Mega Man Legends (PSX)** ([wiki MML1](https://gitlab.com/megamanlegends/mml1-psx/-/wikis), [visor/exportadores](https://megamanlegends.gitlab.io/), [decomp MML1](https://github.com/ChrisNonyminus/mml1), [recomp MML2 PS1](https://github.com/alexbeavs-ps1-ports/mega-man-legends-2-recomp)) | Formatos documentados: EBD (modelos con tabla de huesos, 3 LOD, por miembro `{tri_count, quad_count, vert_count, ..., tri_ofs, quad_ofs, image_coords, palette_coords, vert_ofs}`, vértices s16, caras de 12 bytes con 4 pares UV u8 y 4 índices u8, piel rígida un hueso por miembro, texturas CLUT 4 bits en páginas 256x256), PBD (jugador), TIM, STG (terreno con LOD). Exportadores a glTF/DAE (GPLv3). | El port de N64 no conserva EBD: sus modelos son DL F3DEX2 ya precompiladas (4.1), pero con la misma lógica (piel rígida por hueso, 3 LOD, texturas CLUT de 4 bits, terreno por tiles con páginas tipo VRAM). La wiki ayuda a interpretar los datos de origen. |

No existe ningún pack HD de Mega Man 64 para RT64 ni GLideN64 (la comunidad de Thunderstore `mega-man-64-recompiled` tiene 0 paquetes); los packs HD de la versión PSX (Bl4ckH4nd, VierockHD) no declaran licencia y sus hashes son de VRAM de PSX, así que no sirven ni legal ni técnicamente.

## 5. Experimentos para "diseñar mejores modelos" sin artista

Escala usada: esfuerzo para un agente de código con este repo; riesgo = probabilidad de que no luzca bien o de romper algo; impacto = mejora visual percibida. Todos respetan VR (dos viewports), interpolación a la tasa de la pantalla y los dos renderers (raster y path tracer).

### E1. Pack HD de modelos generado offline (prototipo hecho)

- **Qué:** las 2577 texturas de los 445 modelos con sus hashes RT64 calculados desde la ROM, escaladas con waifu2x y empaquetadas (2.6).
- **Prototipo mínimo:** ya existe `mm64_hd_models_w2x.rtz`. Falta: instalarlo, comprobar en el inspector que los hashes coinciden (si fallan, revisar el estado de TLUT o el tile de render en `mm64_model_hashes.py`), y comparar modelos por categoría (caras: waifu2x anime; máquinas y Reaverbots: HDCube/Siax; ítems: PixelPerfectV4).
- **Esfuerzo / riesgo / impacto:** ½–1 día / bajo / alto (todos los personajes, enemigos, objetos y el cuerpo de Mega Man).
- **Cuidado:** las texturas son atlas (cara al lado del pelo); escalar por regiones o revisar a mano los bordes si aparecen sangrados de color.

### E2. Terreno HD por páginas

- **Qué:** reproducir offline las DL de terreno de cada área, calcular los hashes de todas las ventanas y generar cada ventana recortando una página escalada completa (2.8, opción 1).
- **Prototipo mínimo:** extender `mm64arc.py` para cargar un área (Apple Market, área 4), reconstruir las páginas de textura (tipos `0x00/0x01`), reproducir sus DL de grupo con el estado de `func_8007E8D4` y validar con un volcado real de esa área (los hashes de las ventanas deben aparecer entre los volcados).
- **Esfuerzo / riesgo / impacto:** 3–5 días / medio (reconstruir bien el estado de las DL de material) / muy alto (edificios, calles y dungeons son casi toda la pantalla).

### E3. Árboles 3D procedurales (prototipo de malla hecho)

- **Qué:** reemplazar las tarjetas cruzadas por árboles con tronco ramificado y racimos de hojas.
- **Prototipo hecho:** `C:\Users\Usuario\Devel\tools\upscale\proto\tree_gen.py` genera un árbol (tronco cilíndrico ramificado de 3 niveles + racimos de 3 tarjetas recortadas) y escribe `tree.obj`, `tree_preview.png`, `tree_stats.txt` y **`tree_f3dex2.c`** con los `Vtx` (Y hacia abajo con `--psx-y`) y una DL F3DEX2 en lotes de ≤ 32 vértices: 450 triángulos de corteza en 13 lotes + 168 de hojas en 11 lotes, 749 vértices emitidos (~12 KB) y ~370 comandos, contra 4 triángulos de la tarjeta original.
- **Siguiente paso:** hook de (A3) al cargar el área que, por cada par de tarjetas detectado, emite `G_DL` a una variante del árbol (semilla por posición, escala según el tamaño de la tarjeta) en espacio local del tile; hojas con la textura de la tarjeta original (segmento/ventana existentes, así el pack HD también aplica) y corteza con una textura marcador reemplazada por hash.
- **Esfuerzo / riesgo / impacto:** 3–5 días / medio (detectar todas las variantes de árbol; culling; costo despreciable en PC pero vigilar Quest) / muy alto en exteriores.
- **Alternativa barata (1–2 días):** "más tarjetas": 3–4 tarjetas por árbol con rotación y escala aleatorias y un tinte por racimo; mejora el volumen sin geometría nueva.
- **Interacción con la iluminación:** las hojas siguen siendo recortes verticales sin luz, así que el path tracer les aplica viento, detalle y volumen como hoy; el ROADMAP pide además sombras con alpha test, que funcionan igual con la malla nueva.

- **Hallazgos (2026-10-02)** al buscar los árboles del bosque del guardado (área 3; `RT64_RT_PRINT_VIEW=1` imprime el
  área): el terreno del sub 0 tiene 6 ranuras por registro (0/3, 1/4 y 2/5 son iguales de a pares). En la sección de
  material de dos caras (`G_DL 0x03000000`) de la ranura 0 hay 204 triángulos, 184 verticales, en quads de 192–416 ×
  320 unidades; en las ranuras 1/2, quads de 192–416 × 640 y 92 quads horizontales de 720 (recortes planos). Con
  Mega Man de ~150 unidades, los árboles grandes del bosque parecen más altos que 640, así que probablemente no son
  esas tarjetas sino objetos estáticos (camino de huesos, `func_8003AB60`), que se cambiarían con el hook (A1). Entre los
  modelos de 1 hueso y pocos triángulos que revisé (`models_survey.json`: 76–90, 97, 102–109, 312–317) no hay árboles
  (son cristales, nubes de humo, paneles).
- **Resuelto**: un print temporal en la clasificación de follaje de la iluminación raster mostró que todo el follaje
  del bosque usa el tag `TERRAIN(x,z)` (tipo `0xA`) y quads de 2 triángulos, así que los árboles **son terreno**. En el
  archivo de terreno del área 3, los registros 39–54 (y otros) llevan **una sola tarjeta cada uno**, partida en dos
  mitades (y −640..−320 y −320..0) de 192 o 384 de ancho: el 41 y el 42 son la misma tarjeta en el eje z y en el x;
  45–48 son cuatro tarjetas alrededor de un centro (desplazadas ±96); 53/54, diagonales. Un árbol se arma colocando
  varios registros en la misma posición del arreglo de tiles (`D_801B5088`, `Tile[1710]`). Por eso no aparecen pares
  cruzados dentro de un mismo registro (`tree_detect.py` encontró 1 de 292 quads).
- Siguiente paso para (A3): decodificar el arreglo de tiles para encontrar las posiciones con varios registros de
  tarjeta y reemplazarlas como unidad (por ejemplo, dibujar el árbol 3D desde el primer registro y dejar en `G_NOOP`
  los quads de los demás). Las DL de esas tarjetas viven en el archivo de terreno cargado; su puntero queda en
  `0x800BD9A0` (y en `0x801AF434`) y el de los vértices (tipo `0x12`, segmento 5) en `0x800BD9A4` (`0x801AF430`), lo
  que escribe `func_80035CA0` en `0x800361CC`/`0x800361A0`. Scripts: `tree_survey.py` y `tree_detect.py` en
  `C:\Users\Usuario\Devel\tools\upscale\proto\`.
- **Colocación de los registros, decodificada (2026-10-02)** con `tile_layout.py` (solo lee la ROM). **Corrige el
  hallazgo anterior**: cada celda dibuja un solo registro y cada registro de árbol trae el árbol entero más su suelo;
  no hay registros apilados en la misma posición. El detector anterior perdía las tarjetas diagonales y las de una
  cara (material por defecto del grupo: con el flag `0x4000` el segmento 4 es `0x800BDA80`, tex‑edge de una cara).
  - Archivos del área (`func_80035CA0`): tipo `0x02` → copiado a `0x801B5080`: instancias de 12 bytes
    `{s16 x, y, z, variante; u32 desplazamiento}` (la 0 es vacía; al cargar se les suma `[0x802055A0]`; hasta `0x800`).
    `D_801B5088` apunta al campo puntero de la entrada 0, así que el `Tile` de `tag_terrain.h` está desplazado 8 bytes
    (`Tile.v` es la posición de la entrada siguiente; el parche de las nubes de ovl25 funciona porque lo usa así).
    Tipo `0x03` → copiado a `0x80210CB0`: `u32 n` + n ventanas `{u8 x0, z0, ancho, alto; u32 puntero}` (puntero −
    `0x80164000` = rejilla de `u16` dentro del mismo archivo). Tipo `0x04` → `[0x802055A0]`: 48 bytes por registro
    (colisión); **registro = desplazamiento / 48**. Tipo `0x05` → `[0x80204000]` (tabla de celdas especiales) y tipo
    `0x0F` → `[0x800CEA24]` (texturas 16×16 CI4 de los tiles planos). El `0x12` siempre se carga antes que el `0x11`.
  - El *init* del overlay del área (área 3 = ovl7, `0x802251B4`) toma la ventana número `[0x801BC439]` (la entrada):
    rejilla → `0x801ACA40`, ancho → `0x802049AE`, alto → `0x801D8C7A`, x0 → `0x80206B40`, z0 → `0x801BC65A` y
    `0x80210B5C` = `0x801B5080`. El área 3 tiene 9 ventanas: E0 (40×37), E1 (40×36) y E2 (52×28) son tres tramos del
    bosque (las filas 1–11 de E2 repiten las 24–34 de E1: zona de transición) y E3–E8 son interiores pequeños junto al
    origen.
  - Dibujo (`func_8003912C`), por celda visible `v = rejilla[fila][col]`: `0` = vacía; bit 15 apagado = instancia `v`
    (posición absoluta de la instancia; rotación = `u16` del registro en `+0x26` & 3; slot 3 si la variante es 0 y 0
    si no, porque el LOD está apagado en `us.rev1.toml`); bit 15 encendido = centro de la celda
    (`(col + x0) × 512 − 0x7F00`, igual en z), altura `((v >> 8) & 0x1F) × 128`, rotación `v & 3` y conjunto
    `(v >> 2) & 0x1F`: con el bit 7 apagado es un tile plano de 2×2 texturas 16×16 (`func_800887B4` cerca,
    `func_80088DD0` lejos; los conjuntos salen de datos del overlay copiados a `0x802046B8`) y con el bit 7 encendido,
    un registro de la tabla `0x05`. Rotación = `Ry(r × 90°)` (`func_800319B4`: `x' = c·x + s·z`, `z' = −s·x + c·z`).
    Los registros con `flags[0] & 0xC000` ocupan 2 o 4 celdas y `func_80038550` marca las vecinas en el bitmap
    `0x8017AA20` para no dibujarlos dos veces. Tag `TERRAIN(col, fila)` por celda; buffer de 0x48 = `Mtx` + posición
    en `+0x40` + `+0x46` (registro `& 0x1FF`, modo en los bits 9–11, slot en los 12–14).
  - Bosque del área 3 (todas las instancias con rotación 0 y variante 0): **star3** (registros 39–40, 184
    colocados): 3 tarjetas de dos caras de 384–417 × 640 cruzadas en el centro, textura s176–223 t160–254 de la página
    `(0x2C0, 0x100)` con la CLUT `(0x80, 0x1F3)`; **T** (41–60, 151 colocados): tarjeta de una cara de 384 + media
    tarjeta de dos caras de 192, tronco en el cruce (desplazado hasta 192 hacia el borde de la celda), s128–175 o
    s176–223; **paredes de bosque** (23–38, 162 tarjetas): una cara, 512 o 724 de largo, con una hilera de árboles
    pintada (s128–254 t0–95, CLUT `(0x80, 0x1F2)`), no son árboles sueltos; **setos** (110–118): tarjetas de dos caras
    de 64–144 de alto. Cada tarjeta son dos quads (y −640..−320 y −320..0), cada uno con su ventana de 48×47 texeles;
    el tronco está en la ventana de abajo (unos 8×14 texeles centrados). Los mismos tipos aparecen en las áreas 8, 10,
    11, 16, 17, 18, 22, 24, 27 y 30.
  - Verificación: en `map.png` el camino de tierra recto de E2 (tiles planos del conjunto 9, x ≈ 6400,
    z ≈ 16400–19500) cruza el suelo de bosque con árboles a ambos lados, las paredes miran hacia adentro en los bordes
    y las laderas forman crestas continuas.
  - Uso: `python tile_layout.py <área> [--entrance N] [--story S] [--px 32] [--labels]` en
    `C:\Users\Usuario\Devel\tools\upscale\proto\`. Escribe en `layout_out\areaNN\`: `layout.txt` (archivos y
    direcciones, ventanas, catálogo de registros, detalle de cada registro de árbol con su entrada de grupo, sus DL y el
    `TRI2` de cada quad, y la rejilla con el registro de cada celda), `trees.csv` (una fila por árbol colocado),
    `map.png` (vista cenital texturizada) y `cards.png` (texturas de las tarjetas).
- **Implementado (2026-10-02), variante "núcleo"** (más simple que el plan de abajo): cada registro de árbol del área 3
  (21: star3 39–40 y T 41–60) conserva sus tarjetas y recibe un **núcleo sólido**: una superficie de revolución de 12
  segmentos que sigue la silueta pintada nivel por nivel (cintura, mitad de la ladera, máximo y una repisa que vuelve
  hacia adentro), con la textura de la misma ventana (UV en zigzag cada 4 segmentos, cerca de la densidad de la
  tarjeta), sombra de vértice 165 arriba → 115 abajo y un tronco. Material explícito `0x800BDB30` (opaco a dos caras).
  - Generador: `tree3d_gen.py` (usa `tile_layout.py`; escribe `patches/tree3d_area3.c` con `Vtx`, DL y la tabla
    `{registro, offset de la entrada de grupo, dl1, dl2, DL del núcleo}`, y vistas previas con un rasterizador propio).
  - Parche: `patches/tree3d.c`, llamado cada cuadro desde `func_800276EC` (sin gancho nuevo en `us.rev1.toml`): si la
    entrada todavía tiene sus DL originales, cambia `dl2` por una lista generada: **una copia de la `dl2` original**
    (solo usa direcciones de segmento, así que funciona en cualquier lugar) **con un `G_DL` al núcleo antes de su
    `G_ENDDL`** (o solo el `G_DL` si la entrada no tenía `dl2`). En el modo de fundido 2, `func_8007E8D4` no emite la
    `dl2` sino que se la pasa al recorrido de CPU `func_8007D798`, que descarta los quads muy cerca de la cámara: con la
    copia, los quads de las tarjetas siguen pasando por ese descarte (un primer envoltorio `{G_DL dl2, G_DL núcleo}` los
    escondía detrás de un `G_DL`, que el recorrido copia sin seguir, y se dibujaban pegados a la cámara; lo encontró la
    revisión de código). El núcleo no se descarta. Se guarda la base del archivo con la que se parcheó cada entrada,
    así que una entrada parcheada para otra base también se reconoce. Cada carga de área trae una copia nueva del
    archivo y se vuelve a parchear sola; con la opción apagada vuelve la `dl2` original. Funciona igual en VR (misma
    tarea de celda, mismo grupo de matrices).
  - Opción **"Trees"** (3D / Original, por omisión Original, **F6**), `graphics.json` `trees_option`, función del host
    `recomp_get_3d_trees_enabled` (`0x8F000104`).
  - Problemas resueltos en el camino: líneas oscuras en los cruces tarjeta/núcleo (el replay del G-buffer no tenía
    prueba de profundidad y ganaba la última superficie dibujada: ahora tiene su propio D32); núcleos con normales
    planas (pasaban el límite de 256 triángulos por draw call de las normales suaves en Medium: subió a 512); tapas
    con el winding invertido; aspecto de "melón" a rayas con un anillo por fila (cambiado por anillos por nivel).
  - Costo: dentro del ruido de medición en el bosque (~0,1–0,15 ms de GPU con la iluminación Medium).
  - **Veredicto:** desde la cámara habitual (a ras del suelo, detrás de Mega Man) las tarjetas con las normales de
    esfera de la iluminación se ven más naturales; el núcleo se lee como una pagoda de discos apilados y, a distancia,
    con bandas. Como las tarjetas se cruzan en el centro, la mitad delantera de cualquier núcleo tapa el centro de las
    tarjetas: con escala 0,62 quedaba un huso oscuro dentro del árbol pintado. Queda como opción experimental; ayuda
    vista desde arriba y en VR.
  - Siguientes ideas: (1) árbol completo con racimos de hojas (`tree_gen.py`) que reemplace las tarjetas (el plan de
    abajo); (2) UV planar para el núcleo (de frente coincidiría con la tarjeta que tapa); (3) "más tarjetas" giradas;
    (4) las demás áreas con bosque (8, 10, 11, 16, 17, 18, 22, 24, 27, 30): `tree3d_gen.py --area N` y una tabla por
    área en `tree3d.c`.
- **Plan del parche para (A3)** (C en `patches/`, compilado con zig; el de reemplazo completo, todavía sin implementar):
  1. **Gancho**: `[[patches.hook]]` en `func_80035CA0_110A0` con `before_vram = 0x800361D4` (solo la rama del tipo
     `0x11`; `$v0` = archivo de terreno y los vértices ya están cargados) que llame a
     `tree3d_on_terrain_loaded(terrain)`, guardando y restaurando `ctx` como los demás ganchos. Cada carga de área
     vuelve a parchear (el archivo es una copia nueva en el heap) y no hay costo por cuadro.
  2. **Detección** (la misma regla que `tile_layout.py`): recorrer los registros (el arreglo termina en el primer
     arreglo de grupos o DL, ignorando los campos de los grupos con `0x8000`) y, en cada grupo de sus 6 slots, buscar
     `TRI2` con 4 vértices sobre 2 puntos xz, alto ≥ 200, material tex‑edge (después de `G_DL 0x03000000`, o el
     material por defecto con el flag `0x4000`) y ancho < 448. Si forman star3, T o cruz de 2 con alto ≥ 400, es un
     árbol: centro = cruce, base = y máxima, alto, radio y los bloques de textura de 7 comandos (`E7`, `F5` tile 7,
     `E6`, `F4`, `E7`, `F5` tile 0, `F2`) de las dos mitades de la tarjeta completa. Las paredes y las tarjetas sueltas
     no se tocan (en las áreas 2, 4, 9 y 20 hay tarjetas sueltas que no son árboles).
  3. **Emisión**, una vez por registro de árbol, en un pool fijo `0x80600000`–`0x8067FFFF` (memoria libre del
     Expansion Pak, 4.4; reservarlo en esa tabla y reiniciarlo en cada carga): DL del árbol = `G_DL 0x03000000`
     (hojas, tex‑edge a dos caras) + bloque de la ventana de arriba + `G_VTX` (dirección K0) y triángulos de la copa +
     bloque de la ventana de abajo + copa baja + `G_DL 0x02000000` (tronco, opaco a dos caras, para que el detector de
     follaje no lo mueva) + tronco con UV en la tira del tronco + `G_DL 0x04000000` + `G_ENDDL`. UV = texel × 64
     (`gSPTexture 0x8000`), siempre dentro de la ventana cargada (clamp). La página y la TLUT las pone `func_8007E8D4`
     por grupo, así que basta copiar los bloques y los hashes del pack HD no cambian. Mallas: variantes de `tree_gen.py`
     precalculadas en espacio unitario y escaladas al alto y radio de cada registro; unos 300 triángulos y 5 KB por
     registro (área 3: 21 registros, ~100 KB).
  4. **Ocultar las tarjetas y enganchar el árbol**: en los registros de árbol del área 3 las DL del grupo 0 solo
     tienen quads de tarjeta, así que basta escribir en la entrada del grupo `dl1 = 0` y `dl2 = envoltorio − base del
     archivo` (resta de 32 bits), con envoltorio = `{G_DL árbol, G_ENDDL}` (más un `G_DL` a la `dl2` original si tenía
     otra geometría; en una DL mixta, cambiar solo los `TRI2` de tarjeta por `E7000000 00000000`). **Nunca `G_NOOP`
     ni opcodes nuevos dentro de `dl1`/`dl2`/`dl4`**: en los modos 2–4, `func_8007E8D4` recorre esas DL en la CPU
     (`func_8007D798`, `func_8007DA78`, `func_8007E024`), que solo entienden `01 06 D9 DE DF E2 E7` y bloques `F5` de
     0x30 bytes y con cualquier otro opcode quedan en un bucle infinito (no avanzan el puntero); además cuentan quads en
     `0x8017C1F8`/`0x8017C1FC` y marcan bits en `0x8017C200` (0x70 bytes) y `0x8017C270` sin límite, que una malla
     grande desbordaría. Como copian los `G_DL` tal cual sin seguirlos, la malla del árbol queda fuera de su alcance.
  5. **VR e interpolación**: la DL del árbol se ejecuta dentro de la tarea de la celda, después de su `G_MTX` y con el
     mismo `gEXMatrixGroup TERRAIN(col, fila)`, así que RT64 la interpola y la repite en cada ojo igual que a las
     tarjetas; los vértices son estáticos (nada cambia entre ojos ni entre cuadros). Si se quiere viento, que sea en el
     shader.
  6. **Fase 2 (variedad y LOD)**: un gancho en `func_8007E8D4` antes de `0x8007EF70` (con `s1`/`s2` ya calculados)
     que, si el grupo es de árbol, cambie `s2` por una variante elegida con un hash de la posición de la instancia
     (`buffer +0x40`/`+0x44`) y por una versión simple según la profundidad.
  - **Riesgos**: el fundido cerca de la cámara (modos 3/4) solo actúa sobre `dl1`, así que el árbol 3D en `dl2` se
    dibuja opaco aunque tape a Mega Man; las copas de los árboles T, con el tronco hasta a 64 del borde, invaden la
    celda vecina o la pared pintada (desplazar el centro hacia adentro o aplanar la copa trasera); la colisión (archivo
    `0x04`) sigue siendo la original; el culling por centro de celda puede hacer saltar copas más anchas que 208; costo
    en Quest (unos 100 árboles visibles × 300 triángulos); y los 4 MB libres no están reservados formalmente.

### E4. Cielo en HD

- **Qué:** reemplazar las franjas CI4 del cielo (pool 3, `func_8002E8FC`) por un panorama pintado o escalado.
- **Prototipo mínimo:** volcar el cielo de 2 áreas, reconstruir el panorama ordenando las franjas por posición de pantalla (inspector), escalarlo entero (animevideov3 o waifu2x con ruido 1–2 contra el bandeado), recortarlo en las mismas franjas y empaquetar con `shift: none`.
- **Esfuerzo / riesgo / impacto:** 1–2 días / bajo–medio (costuras entre franjas, el hook de estiramiento de `us.rev1.toml`) / alto en exteriores. El cielo procedural raster del ROADMAP es complementario: este experimento mejora el modo "cielo original".

### E5. Materiales por hash: emisivos, reflejos y luces adjuntas

- **Qué:** cargar automáticamente desde el pack un `rt64-materials.json` con las bibliotecas de presets que el fork ya tiene (2.9): emisivo (`selfLight`) para lámparas, pantallas y lava; reflejo para agua y metal; luces puntuales que siguen a objetos (antorchas).
- **Prototipo mínimo:** en `TextureCache::loadReplacementDirectories` (o en el host al recargar packs) leer el archivo y volcarlo en `State::drawCallLibrary` / `materialLibrary` / `lightsLibrary`; autorar 5 materiales de un dungeon con el inspector ("Materials", "Draw calls", "Lights") y guardarlos.
- **Esfuerzo / riesgo / impacto:** 1–2 días para la carga automática + autoría / bajo / alto en dungeons con path tracing; la iluminación raster nueva debería leer el mismo dato (coordinar con ese trabajo).
- **Hallazgos para la iluminación raster (2026-10-02):** el brillo emisivo por color de la composición (`LightingEmissiveCS`,
  apagado por defecto, ver `enhanced-lighting.md`) no distingue las lámparas de los estandartes. Marcar texturas por hash
  tampoco alcanza solo: en las dungeons de MM64 las lámparas están **pintadas dentro de la textura de la pared** (las
  naranjas de 20,0 son unos píxeles de la ventana del metal), así que un flag por textura haría brillar la pared entera.
  Haría falta un material con **clave de color** (brillan los texeles cercanos a un color dado: el G-buffer ya muestrea la
  textura en los recortes y podría escribir ese peso) o un **mapa emisivo** en el pack, o las presets por draw call que
  el fork ya tiene para el path tracer, autoradas con el inspector. Un barrido por contenido de las 19 811 ventanas del
  pack HD (`C:\Users\Usuario\Devel\tools\upscale\terrain\material_candidates.py`: fracción de texeles brillantes y
  coloridos o blancos, hojas de contacto en `out\materials\`) devuelve sobre todo ventanas de color liso (azul, blanco,
  naranja), paredes blancas y tiras de paleta: sirve para revisar a mano, no para decidir solo.

### E6. Mapas de normales y rugosidad en los packs

- **Qué:** campos nuevos por textura (`normal`, `orm`, `emissive` o sufijos `_n`, `_orm`, `_e`), cargados junto al albedo y usados por el path tracer en lugar del relieve por luminancia; generados con `normal_from_albedo.py` o con PBRify NormalV3/RoughnessV2 (CC0; convención DirectX, invertir el verde si hace falta).
- **Prototipo mínimo:** solo normales, solo path tracer: un índice extra en `GPUTile`, lectura en `RaytracingLibrary.hlsl` (ya calcula `tangentU/tangentV` en `:400-425`), y un pack de prueba con 10 texturas de muro.
- **Esfuerzo / riesgo / impacto:** 1–2 semanas / medio (plumbing de texturas en caché, costo del shader) / medio–alto (superficies que reaccionan a la luz; no aporta nada al raster original).

### E7. Personajes suavizados offline

- **Qué:** subdividir (Loop o Catmull‑Clark adaptado a triángulos) cada malla de hueso con los bordes fijos, interpolando UV, y volver a escribirla como DL.
- **Prototipo mínimo:** con `export_model.py` exportar a Mega Man y 2 NPC, subdividir en Python, renderizar antes/después en una hoja de comparación y decidir antes de tocar el juego. Si convence: conversor OBJ → DL (lotes ≤ 127 vértices), tabla de (modelId, hueso) y hook (A1).
- **Esfuerzo / riesgo / impacto:** ~1 semana / medio (costuras en articulaciones, el look "derretido" puede chocar con el arte de bordes duros; cara y boca van por otro camino) / medio.
- **Por qué offline y no en el renderer:** todos los modelos están en la ROM y no tienen normales ni iluminación del RSP; PN‑triangles o teselado en RT64 costaría semanas y aplicaría a ciegas a todo.

### E8. Texturas de detalle procedurales

- **Qué:** a corta distancia, mezclar micro‑detalle (ruido, celular, vetas) según la clase del material, asignada por hash en la metadata del pack (piedra, pasto, madera, metal). El path tracer ya lo hace para el follaje (`foliageDetailAt`, `RaytracingLibrary.hlsl:123-140, 271-282`); generalizarlo y llevarlo al raster.
- **Esfuerzo / riesgo / impacto:** 3–5 días / bajo–medio (parpadeo si no se atenúa con la distancia y los mips) / medio, sobre todo en primera persona y VR.

### E9. Herramienta de reemplazo de modelos completos (OBJ/glTF → blob)

- **Qué:** conversor que toma un OBJ/glTF por hueso (con el rig de 22 huesos) y produce un blob MM64 completo (A2) con texturas CI4/RGBA16 nuevas, más la entrada en el pack HD.
- **Prototipo mínimo:** reemplazar la cabeza de Mega Man (hueso 1) por una esfera de prueba y verificar animación e interpolación.
- **Esfuerzo / riesgo / impacto:** 1–2 semanas / medio–alto (*bind pose* ausente: capturar las matrices de cada hueso en tiempo de ejecución) / depende del arte disponible.

### E10. Mallas de reemplazo en RT64 (híbrido c)

- Solo cuando haya arte con materiales modernos. Diseño en 4.5 (c). 3–6 semanas.

## 6. Hoja de ruta priorizada

**Fase 0 — Validación y saneamiento (½–1 día)**
1. Instalar `C:\Users\Usuario\Devel\tools\upscale\rt64\mm64_hd_models_w2x\mm64_hd_models_w2x.rtz` en `%LOCALAPPDATA%\MegaMan64Recompiled\mods\` con el juego cerrado; activar `developer_mode` en `graphics.json`; comprobar con F4 y el inspector (E1). Capturas antes/después con `C:\Users\Usuario\Devel\tools\shot.ps1` o `rt_compare.ps1`.
2. Corregir el solapamiento del BSS con `0x81000000` (`patches/patches.ld`, `patches/vr.c`, `patches/mouse_camera.c`) antes de cualquier uso de `recomp_alloc`.
3. Decidir el `mod_game_id` definitivo (`src/main/main.cpp:389`).

**Fase 1 — Texturas (1–2 semanas)**
4. E2: pipeline offline de terreno por páginas (Apple Market primero, luego todas las áreas).
5. E4: cielo HD.
6. HUD, fuentes y menús: volcado + xBRZ/PixelPerfectV4 con `shift: none`; caras y bocas de Mega Man por volcado.
7. Elegir el modelo de escalado por categoría con capturas A/B (sección 3.6) y publicar un pack v0.1 (`texture_packer --create-pack`).

**Fase 2 — Geometría (2–3 semanas)**
8. Hook (A1) con una tabla (modelId, LOD, hueso) → DL, archivo nuevo `patches/model_swap.c` + `[[patches.hook]]` en `us.rev1.toml` (`func_800805F8_5B9F8`, `before_vram = 0x80081218`).
9. E3: árboles 3D (hook tras `func_80035CA0`, mallas de `tree_gen.py`).
10. E7: suavizado offline de personajes (si la hoja de comparación convence).

**Fase 3 — Materiales y ambiente (2–3 semanas, coordinado con la iluminación raster)**
11. E5: carga automática de presets de materiales desde el pack; emisivos y luces de dungeons.
12. E6: mapas de normales/rugosidad en el formato del pack y en el path tracer.
13. E8: detalle procedural por clase de material.

**Fase 4 — Exploratorio**
14. E9: conversor de modelos completos; E10: model packs en RT64 si aparece arte nuevo.

## 7. Hallazgos colaterales

| Hallazgo | Dónde | Consecuencia | Acción sugerida |
|---|---|---|---|
| BSS de los parches pasa `0x81000000` | `patches/patches.map` (fin en `0x810F2820`), `patches/patches.ld:18` | Corrupción del heap de `recomp_alloc` o de mods de código | Achicar o mover `sVrTaskBackups`; `ASSERT(. < 0x81000000)` |
| `mod_game_id = "mm"` | `src/main/main.cpp:389` | Mods de Majora's Mask aceptados por MM64 (y al revés) | Usar un id propio |
| Todos los mods se fuerzan a activos al arrancar | `src/main/main.cpp:788-807` | Un pack no se puede dejar desactivado entre sesiones | Respetar `mods.json` |
| `to_json(ReplacementShiftFilter)` escribe `"operation"` | `lib/rt64/src/common/rt64_replacement_database.cpp:379-382` | "Save directory" pierde los `shiftFilters` | Corregir a `"shift"` (también vale para upstream) |
| `texture_packer --create-pack` exige `rt64-low-mip-cache.bin` | `lib/rt64/src/tools/texture_packer/texture_packer.cpp:479-485` | Packs solo PNG necesitan correr antes `--create-low-mip-cache` (genera un archivo vacío) | Documentado en 2.3 |
| `batch_pipeline.py` pasaba todos los archivos a `texconv` en una sola línea | `C:\Users\Usuario\Devel\tools\upscale\scripts\batch_pipeline.py` | Falla con miles de texturas (límite de Windows) | Ya corregido (tandas de ~24 K caracteres) |

## 8. Apéndice: archivos creados y cómo usarlos

| Ruta | Contenido |
|---|---|
| `C:\Users\Usuario\Devel\tools\upscale\rt64\rt64_tmem.py` | Decodificador de TMEM, hash v5 y cargas de TMEM de RT64 en Python |
| `...\rt64\selftest.py`, `...\rt64\test_loads.py` | Pruebas contra el C++ de RT64 (`bin\hash_check.exe`, `bin\load_check.exe`) |
| `...\rt64\rt64_dump_to_png.py` | Volcados de RT64 → PNG + `index.csv` (`--group`, `--verify`, `--scale`) |
| `...\rt64\make_rt64_pack.py` | `rt64.json` + `mod.json` (+ `.rtz` deflate opcional) |
| `...\rt64\mm64_model_hashes.py` | Hashes y PNG de todas las texturas de modelos desde la ROM |
| `...\rt64\demo_pipeline.py` + `demo_out\` | Demo de punta a punta con volcados sintéticos |
| `...\rt64\model_hashes_out\` | 2577 PNG + `model_hashes.csv` |
| `...\rt64\mm64_hd_models_w2x\mm64_hd_models_w2x.rtz` | Pack de prueba de todos los modelos (waifu2x x4, BC7) |
| `...\rt64\model_hashes_sheet.png`, `model_w2x_compare.png` | Hojas de muestra y antes/después |
| `...\rt64\bin\` | `texture_packer.exe`, `texture_hasher.exe`, `hash_check.exe`, `load_check.exe` y los `.bat`/`.cpp` para recompilarlos |
| `C:\Users\Usuario\Devel\tools\upscale\scripts\` | Pipeline de escalado (sección 3.5) |
| `C:\Users\Usuario\Devel\tools\upscale\bin\`, `models\` | Real-ESRGAN, waifu2x, texconv, xBRZ y modelos permisivos |
| `C:\Users\Usuario\Devel\tools\upscale\samples\` | Comparaciones, tiempos y métricas de fidelidad |
| `C:\Users\Usuario\Devel\tools\upscale\mm64_extract\` | Lector del archivo de modelos, LZSS, exportador OBJ/PNG, encuesta de 445 modelos, extractor de MIPS de `RecompiledFuncs` |
| `C:\Users\Usuario\Devel\tools\upscale\proto\tree_gen.py` + `tree_out\` | Árbol procedural: OBJ, DL F3DEX2, vista previa |

Comandos de verificación:

```
cd C:\Users\Usuario\Devel\tools\upscale\rt64
python selftest.py        # hash v5 + decodificación (300 casos contra hash_check.exe)
python test_loads.py      # cargas de TMEM (300 casos contra load_check.exe)
python demo_pipeline.py   # pipeline completo con volcados sintéticos
```

## 9. Terreno HD por páginas: prototipo offline (Apple Market)

Implementación de E2 (opción 1 de 2.8) para el área 4, entrada 0 (la de `MM64_VR_WARP=4,0`). Todo vive en `C:\Users\Usuario\Devel\tools\upscale\terrain\`; no se ejecutó el juego ni se compiló el proyecto.

**Cómo carga y dibuja el terreno el juego** (leído en `RecompiledFuncs`):

- Al cargar un área (`func_80073510`), `func_80036888` vuelve a subir 9 TIM globales (archivo en `0x800AC898`), `func_80036340(área, 0)` carga el sub 0 del grupo del área (terreno tipo 0x11 y vértices 0x12) y una segunda llamada carga el **banco de texturas compartido (grupo 0x26) sub `byte 0x801ACA5B − 1`** y después el sub `byte 0x801ACA59` del área. Los dos bytes los calcula `func_80072E10` según área, entrada y estado de la historia; para Apple Market entrada 0 valen 2 y 1: banco sub 1 (33 archivos de páginas y CLUT) y un sub de objetos sin texturas.
- Un archivo de página (tipo 0x00, estilo TIM) registra su imagen como nodo {x, y, w, h, puntero} (`func_8002C8EC`; subir otra vez el mismo rectángulo reemplaza el puntero, y en el banco gana la última de dos subidas a (0x2C0,0x100) y a (0x340,0x100)). Su CLUT pasa por `func_800870A0`, que **reescribe cada entrada 0x0000 con `(suma de las 16 entradas >> 4) & ~1`** antes de guardar un puntero por cada 16 colores en las tablas `0x8017CBF0` / `0x8017D0F0` (las filas de un bloque de 16×8 quedan en x consecutivas). El hash de la paleta depende de ese arreglo.
- Por grupo, `func_8007E8D4` carga una TLUT de 16 colores desde la CLUT `(flags >> 16) & 0x7FFF` (`func_8007BFC8`), hace `SETTIMG` CI 8b sobre la página `x = (flags & 0xF) << 6`, `y = (flags & 0x10) << 4 | (flags & 0x800) >> 2` (64×256 o 32×128 unidades de 16 bits según el bit 31) y llama a las DL del grupo, que cargan la ventana con `LOADTILE` a TMEM 0 y la dibujan con un tile CI4, clamp, paleta 0. Solo se hashea el tile 0.
- El overlay de cada área sale de la tabla `0x800BD340` (`func_8007BEA0`). El de Apple Market (ovl9) no toca páginas, CLUT ni el archivo de terreno; ovl3/4/16/19/22 copian bloques entre páginas (`func_8002FDF4`) y otros overlays buscan CLUT para modificarlas (ovl23, por ejemplo, mezcla dos paletas en una tercera).

**Resultado (Apple Market):** 84 registros, 1004 grupos y 5088 dibujos dan **946 ventanas únicas**, todas con su página y su CLUT resueltas en el banco (8 páginas, 59 combinaciones página×CLUT; las páginas decodificadas muestran los carteles de Jetlag Bakery, Akbar Toy Store o Tailor Chinos con colores coherentes).

- Verificación offline: 946/946 hashes idénticos con el hasher C++ de RT64 (`hash_check.exe`) y 946/946 al reconstruir cada TMEM con la copia literal del cargador de RT64 (`load_check.exe`). La decodificación de cada ventana coincide con su recorte de la página en 900 casos; en 45 la DL del juego carga más palabras por fila que `line` y la última columna muestra el primer texel de la fila siguiente (defecto original de un texel que el recorte corrige) y 1 lee un texel pasado el borde derecho. Otras 2 ventanas (16 dibujos) leen la fila 128 de páginas de 128 filas, es decir memoria del heap: su hash no se puede predecir y quedan fuera.
- **Probado en el juego (2026-10-02)**: con el pack en `mods`, Apple Market se ve con las texturas HD (puertas, carteles de las tiendas, banderines, letrero de la entrada y suelo), sin costuras visibles entre ventanas. No se hizo un volcado para medir el porcentaje exacto (`validate_terrain_dump.py`), pero todo lo que se ve en la vista del warp quedó reemplazado. El pack se desinstaló después de la prueba.
- Pack: `...\terrain\out\area04_apple_market\mm64_hd_terrain_area04_w2x\mm64_hd_terrain_area04_w2x.rtz` (10 MB, 946 DDS BC7 con mips en `terrain/area04/`, low mip cache y `mod.json`, igual que el pack de modelos). Cada página se escala una sola vez (waifu2x cunet ×4, relleno de borde, alfa de 1 bit con xBRZ y umbral) y cada ventana se recorta de ella, así que las vecinas quedan continuas. Antes/después: `preview_pages.png` y `preview_windows.png` en `out\area04_apple_market\`.

**Uso:**

```
cd C:\Users\Usuario\Devel\tools\upscale\terrain
python build_terrain_pack.py --area 4                    # replay, verificaciones, escalado y pack (~50 s)
python build_terrain_pack.py --area 4 --method siax      # otro escalador de upscale_lib.py
python survey_areas.py                                   # cobertura de todas las áreas
python validate_terrain_dump.py C:\mm64hd\dump_area04    # contra un volcado real de RT64
python validate_terrain_dump.py --hash <hash>            # hash copiado del inspector (F1)
powershell -ExecutionPolicy Bypass -File install_pack.ps1 [-Uninstall]
```

Prueba en el juego: con el juego cerrado, copiar el `.rtz` a `%LOCALAPPDATA%\MegaMan64Recompiled\mods\` (o usar `install_pack.ps1`, que no hace nada si el juego está abierto), poner `"developer_mode": true` en `graphics.json`, entrar con `MM64_VR_WARP=4,0` y alternar con F4. Para medir la coincidencia: F1 → Textures → "Start dumping textures", recorrer el área, detener el volcado y pasar la carpeta a `validate_terrain_dump.py`, que informa qué porcentaje de las ventanas de terreno volcadas está en el pack y clasifica cada falla (ventana desconocida = replay de DL; índices distintos = página; paleta distinta = CLUT o arreglo de entradas 0x0000, usado por 195 ventanas marcadas en `windows.csv`).

**Pendiente para todas las áreas:** según `survey_areas.py`, 28 áreas tienen terreno en el sub 0 y en 26 se resuelven todas las páginas y CLUT con la entrada 0 y el sub del banco que mejor cubre (unas 11.700 ventanas). Falta (1) portar `func_80072E10` por área, entrada y estado de la historia en lugar de elegir el sub del banco por cobertura (varias áreas se resuelven con más de un sub y la paleta puede cambiar); (2) las áreas 5, 6 y 12 (ciudad) no tienen terreno en el sub 0 y las 1 y 23 quedan incompletas; (3) los grupos con el bit 0x8000 (quads procedurales, `func_8007CC94` / `func_8007C450`); (4) páginas y CLUT animadas por overlays (cada cuadro es otro hash: congelar la animación o generar cada cuadro). El pase de sombra (TLUT negra `0x800BD9B4`) no se reemplaza a propósito. Riesgos: mezcla de subtexturas vecinas del atlas en los bordes de ventanas de 2 o 3 texeles y las 2 ventanas que dependen del heap.

### 9.1 Todas las áreas: puerto de `func_80072E10`, secuencias de carga y pack combinado

Continuación de la sección 9 (2026-10-02). Todo vive en `C:\Users\Usuario\Devel\tools\upscale\terrain\`: `mm64_areas.py` (puerto y secuencias), `build_all_terrain.py` (pipeline completo con un solo comando) e `install_all.ps1`. No se ejecutó el juego, no se compiló el proyecto y no se instaló nada en `mods`.

**Qué lee y qué escribe `func_80072E10`** (`a0 = 0x801BC3E8`, bloque de estado del juego):

- Lee el área (`s8 0x801BC438`, la petición de carga; el área cargada es `s16 0x801BC450`), la entrada (`s8 0x801BC439`), el byte de progreso de la historia (`s8 0x801BC43A`, comparado contra 0..12) y banderas de evento del arreglo de bits `0x802055A8` (`func_8006033C`: `byte[n >> 3] & (0x80 >> (n & 7))`). Solo cuatro áreas consultan banderas: 5 (`0x001 0x002 0x200 0x204 0x206 0x212 0x213`), 6 (`0x630`), 11 (`0x01B 0x01C`) y 12 (`0x030`).
- Escribe `0x801ACA58` = área, `0x801ACA59` = sub del área de la segunda llamada (por omisión la tabla de bytes `0x800B37FC[área][entrada + 1]`; 0 o 0xFF = no hay segunda llamada), `0x801ACA5A` = sub de la primera llamada cuando difiere del área (solo el área 12, con 1) y `0x801ACA5B` = sub del banco (grupo 0x26) + 1.
- Banco por área: 3 → sub 0 (entradas 0 a 2), 4 → 1 (E0), 5 → 2 (E0 y E1), 6 → 3, 4 o 5 (E0, E1, E2), 7 → 6 (E5), 8 → 7 (E0), 10 → 8 (E0), 11 → 9 o 10, 13 → 11 (E0 y E5), 15 → 12 (E2), 16 → 13 (E0), 19 → 17 (sub 1 o 2), 22 → 14, 27 → 16 (sub 1 o 7). Además `func_80036340` carga **siempre todo el banco sub 15 para el grupo 12**. El resto de las áreas no usa banco.
- `func_80036340(grupo, sub)`: con sub 0 usa `0x801ACA5A` si difiere de `0x801ACA58`; el grupo 12 carga primero el banco 15; si sub != 0 y `0x801ACA5B` != 0 carga el banco `0x801ACA5B − 1` y lo pone en 0; al final carga el sub del grupo. El último archivo tipo 0x11 cargado es el terreno (`func_80035CA0` guarda un solo puntero, `0x801AF434` / `0x800BD9A0`; el 0x12 es el de vértices).
- **Reentrada**: `func_80073828` corre justo después de `func_80073510`, vuelve a llamar a `func_80072E10` y, si `0x801ACA59` difiere del sub actual (`s16 0x801BC452`), carga banco + sub sin liberar nada. Si el área no cambió (otra entrada de la misma área) es la única carga, así que las páginas y CLUT de la entrada anterior siguen ahí.
- **Cargas de overlays** con argumentos constantes (`func_80036340` / `func_800739B0`): área 5 → sub 4; 6 → 11 y 9; 11 → banco 10 + subs 7 y 2, banco 9 + subs 5 y 6; 13 → 4; 23 → 2 y 1; 25 → 3 y 4; 26 → 4 a 8 (salas con terreno propio); 30 → 1.
- La partida de prueba (flash, 0x2000 bytes por ranura, `loadSaveData` copia 0x1800 a `0x80193C70` y `func_80074A9C` pasa los offsets 0x94/0x95/0x96 a `0x801BC438..A`) está en **área 3, entrada 2, historia 0**: carga `3.0 + banco 0 + 3.1`. Es el principio del juego, no una partida a mitad de camino.

`mm64_areas.area_sequences(área)` recorre todas las entradas de la tabla, la historia −1..12 y todas las combinaciones de banderas del área, y produce 100 secuencias "frescas" (cambio de área), 244 de reentrada (fresca + banco/sub de otra entrada) y 28 con cargas de overlay; se descartan las cargas sin texturas ni terreno. Las frescas aportan todos sus grupos resueltos; las derivadas solo los grupos que ninguna fresca resuelve, para no multiplicar variantes que no se ven. `mm64_terrain.area_load_sequence` también usa el puerto (con historia 0 por omisión), y `build_terrain_pack.py --area 4` sigue dando las mismas 946 ventanas. `survey_areas.py` queda obsoleto: su "mejor banco" por cobertura era incorrecto (por ejemplo, el banco 15 trae su propio terreno y reemplazaba el de las áreas 11, 19 y 27).

**La ciudad.** Las áreas 5 y 6 no tienen terreno en el sub 0 porque lo guardan en el banco: el área 5 dibuja con el terreno del banco sub 2 (E0 y E1; el sub 0 del área aporta 23 páginas y el paquete de CLUT) y con terrenos propios en los subs 3 (E2) y 5 (E4); estos dos toman sus CLUT `(0x80,0x1F3)`, `(0x80,0x1F2)`, `(0,0x1FD)`... del banco 2, así que solo se resuelven al llegar caminando desde E0/E1 (reentrada). El área 6 usa los terrenos de los bancos 3, 4 y 5 (E0, E1, E2), del sub 4 (E4) y del sub 11 (overlay). El área 12 dibuja solo con el banco 15; sus páginas muestran pasto, árboles y una compuerta mecánica, así que no parece ser una calle de la ciudad. Resultado: área 5, 2903 grupos en 3 terrenos, todos resueltos, 901 ventanas; área 6, 4192 grupos en 5 terrenos, 4045 resueltos, 1301 ventanas; área 12, 924 grupos, 277 ventanas. Los 147 grupos del área 6 que nunca se resuelven usan la página `(0x280,0x100,0x20,0x80)` y las CLUT `(0,0x1F6)` / `(0,0x1F2)`, que solo suben subs de otras áreas: el juego los dibujaría con el estado anterior, así que probablemente no se ven.

**Grupos 0x8000.** `func_8007CC94` dibuja quads sueltos: la palabra 1 del grupo es el primer vértice y la 2 la cantidad de quads (4 vértices de 16 bytes del archivo 0x12); `func_8007C108` toma la caja de las UV (`min`/`max` de s y t `>> 6`, par, al menos 2) y se emite `SETTILE` t7 CI 8b + `LOADTILE` + `SETTILE` t0 CI 4b + `SETTILESIZE`. El pipeline arma ese mismo DL sintético por quad: 174 ventanas en las áreas 2, 3, 6, 8, 19, 23, 24, 27 y 30 (todos los grupos 0x8000 quedan cubiertos).

**Páginas animadas por overlays.** Todas son desplazamientos verticales (agua y cascadas) hechos con `func_8002FDF4`, un "MoveImage" que copia un rectángulo de VRAM sobre los datos de la página (si el destino es una página de 0x20×0x80, todas las coordenadas se dividen por 2). Hay un objeto "cascada" compartido por ovl4/16/19/22 (áreas 9, 14, 17, 20: tabla de 5 entradas `{x0, y0, x1, y1, paso, bandera}`, bloque A de 0x10×0x40 desplazado `s` filas, bloque B de 8×0x20 desplazado `s/2`, `s` avanza de a 2) y cuatro bloques fijos en ovl3 (área 11). Se generan los 32 cuadros (aplicando primero el cuadro anterior, porque en páginas de media resolución un desplazamiento impar deja una fila del cuadro previo) solo para los bloques cuyo destino ya contiene en la ROM un cuadro de la tira en al menos la mitad de las filas: área 9 (4A, 4B), 11 (los cuatro), 14 (0A, 0B), 17 (3A, 3B) y 20 (1A, 2A). Son 1179 ventanas. En ovl3 se supone que los contadores arrancan en 0 (fase par entre los bloques grandes y chicos).

**CLUT animadas (no hechas).** Los overlays de 16 áreas tienen un objeto que mezcla dos CLUT en una tercera (por ejemplo `func_8024D31C_ovl10` en el área 5): interpola cada canal de 5 bits con `paso/pasos`. Las CLUT de origen y destino y los pasos vienen de los datos de cada objeto colocado, que no se interpretaron; las ventanas que usan una CLUT destino muestran la textura original durante y después de la mezcla.

**Inestables.** Las ventanas que leen la fila 128 de páginas de 128 filas (bytes que siguen al archivo TIM en el heap) no se pueden predecir y quedan fuera; la mayor parte está en el área 26 (812 dibujos en sus 8 terrenos).

**Escalado.** Con la GPU compartida con el juego en prueba, waifu2x devolvió en silencio imágenes negras en dos lotes (246 páginas) y otras 5 páginas oscuras pasaban un control ingenuo. Ahora cada página escalada se compara con su original (reducción por caja, con tolerancia relativa para páginas oscuras), se rehace con lotes y tiles más chicos si falla (xBRZ por CPU solo como último recurso, informado en `summary.json`), y un manifiesto en el directorio de DDS rehace las ventanas de toda página regenerada.

**Resultado (2026-10-02):** 19 811 ventanas únicas, 19 811/19 811 hashes idénticos con el hasher C++ de RT64 y 19 811/19 811 TMEM reconstruidas por su cargador C++; 18 341 exactas contra el recorte de la página, 1415 con el defecto de un texel de la DL original (el recorte lo corrige) y 55 que leen pasado el borde.

| área | ventanas | grupos sin resolver / motivo |
|---:|---:|---|
| 0 | 930 | 24 dibujos inestables |
| 1 | 0 | 9 de 9: página y CLUT solo de otras áreas |
| 2 | 354 | — |
| 3 | 1391 | 24 dibujos inestables |
| 4 | 1774 | 16 dibujos inestables |
| 5 | 901 | 98 dibujos inestables |
| 6 | 1301 | 147 grupos (página/CLUT de otras áreas), 58 dibujos inestables |
| 7 | 1352 | 72 grupos del terreno del banco 6 (E5): páginas `(0x280/0x240,0x100)` de otras áreas |
| 8 | 256 | — |
| 9 | 1003 | 8 dibujos inestables (incluye 274 cuadros de cascada) |
| 10 | 1037 | — |
| 11 | 1194 | — (529 cuadros de ovl3) |
| 12 | 277 | — |
| 13 | 960 | 6 dibujos inestables |
| 14 | 504 | — (62 cuadros) |
| 15 | 649 | — |
| 16 | 545 | 6 dibujos inestables |
| 17 | 405 | — (188 cuadros) |
| 18 | 316 | 6 dibujos inestables |
| 19 | 1201 | 14 dibujos inestables |
| 20 | 496 | — (126 cuadros) |
| 21 | 147 | — |
| 22 | 509 | — |
| 23 | 154 | — |
| 24 | 260 | — |
| 25 | 494 | 41 dibujos inestables |
| 26 | 903 | 812 dibujos inestables (8 terrenos) |
| 27 | 1033 | 12 dibujos inestables |
| 28 | 212 | — |
| 29 | 1982 | — |
| 30 | 216 | — |

Las sumas superan el total porque 1542 ventanas aparecen en más de un área. Los detalles quedan en `out\all\coverage.csv`, `sequences.csv`, `windows.csv`, `problems.csv` y `summary.json`, y las comparaciones original/HD de cada área en `out\all\previews\areaNN.png`.

**Packs:** `out\all\packs\mm64_hd_terrain_all_w2x\mm64_hd_terrain_all_w2x.rtz` (176 MB, 19 811 DDS BC7 con mips, mod id `mm64_hd_terrain_all_w2x`) y un pack por área en `out\all\packs\mm64_hd_terrain_areaNN_w2x\` (el del área 4 reemplaza al de la sección 9 con el mismo id y pasa de 946 a 1774 ventanas). La caché de mips bajos del pack combinado ocupa 202 MB sin comprimir y RT64 la sube entera a la GPU al cargar el pack (unos 100 MB de texturas más el búfer de subida de 202 MB): sin problema en la PC, pero a tener en cuenta para el port de Quest.

**Uso:**

```
cd C:\Users\Usuario\Devel\tools\upscale\terrain
python build_all_terrain.py                     # todo (unos 10 min la primera vez; incremental después)
python build_all_terrain.py --skip-upscale      # replay, verificaciones C++ e informes (unos 2 min)
python build_all_terrain.py --areas 4,5,12      # subconjunto
powershell -ExecutionPolicy Bypass -File install_all.ps1 [-Uninstall] [-RemoveAreaPacks] [-DryRun]
```

`install_all.ps1` no hace nada si el juego está abierto, avisa si hay packs por área instalados (duplican texturas del combinado) y no toca `mods.json`: un mod nuevo se activa solo al siguiente arranque.

**Riesgos:** (1) las mezclas de CLUT de los overlays; (2) las secuencias dependientes de la historia generan ventanas que una partida concreta nunca ve (más tamaño, no errores); (3) la regla de las secuencias derivadas descarta variantes de grupos que ya resuelve una carga fresca: si el juego mostrara justo esa variante, se vería la textura original; (4) la fase supuesta de ovl3 y el criterio de "bloque verificado" de las cascadas; (5) los nombres de casi todas las áreas siguen sin confirmar en el juego.

**Prueba en el juego del pack combinado (2026-10-02):** instalado solo durante la prueba (y quitado después, con `mods.json` restaurado), con capturas sin y con el pack de la misma build: `area_scan.ps1 -Areas @("save","4,0","5,0","14,1","20,0","26,0","15,0","16,0") -Full -Suffix "_sd"` (y `"_hd"` con el pack), y la hoja `shots\hdcmp_sheet.png` más recortes `hdcmp_crop_<área>.png` (`python C:/Users/Usuario/Devel/tools/hd_compare.py`, que compara los pares `scan_<área>_sd/_hd.png`). Carga sin errores y sin costo de GPU apreciable (1,4–1,9 ms igual que sin pack). Se ven más nítidos los relieves de la ruina rosada (26,0), las piedras del pasillo con raíces (14,1), las marcas de la calle de la ciudad (5,0) y las paredes de metal azul (20,0); en el bosque (área 3) casi no cambia (texturas de baja frecuencia y niebla). **Defecto:** en texturas con ruido o tramado (el metal azul de 20,0) waifu2x convierte el ruido en manchas suaves con aspecto de acuarela; para esas páginas convendría el nivel de ruido 0 o un modelo que conserve el grano (por ejemplo los de `models\ncnn\`), elegido por página según su varianza local. La validación por volcado (`validate_terrain_dump.py`) sigue pendiente.

### 9.2 Ruido y tramado: grano conservado por página (waifu2x + hqx con máscara local)

Continuación de 9.1 (2026-10-02) a partir del defecto visto en la prueba en el juego: en las páginas con ruido o tramado waifu2x convierte el grano en manchas suaves con aspecto de acuarela (paredes de metal azul de 20,0, paredes de roca de 14), mientras que las páginas de formas limpias (relieves de 26, marcas de la calle de 5) se ven bien y no deben cambiar. Todo vive en `C:\Users\Usuario\Devel\tools\upscale\terrain\`: `page_noise.py` (medida y tratamiento), `noise_eval.py` (distribución y hojas de comparación) y el paso 3b de `build_all_terrain.py`. No se ejecutó el juego, no se instaló nada en `mods` y no se descargó nada: todos los candidatos son las herramientas y modelos de la sección 3.

**Medida** (`page_noise.py`, sobre la página original 1x, solo texeles opacos):

- Dos texeles vecinos son *distintos* si algún canal difiere en más de 12 (dos pasos de 5 bits; el moteado tenue, como las vetas del mármol, no cuenta).
- Un texel es *activo* si al menos 1 de sus 4 vecinos es distinto, y es una *mota* si al menos 3 de los 4 son distintos y no es *lineal*: no forma una racha de más de 2 texeles parecidos en una dirección (horizontal, vertical o una de las diagonales) sin tenerla también en la perpendicular. Así no cuentan los bordes, las líneas de 1 texel, los contornos diagonales, los anillos ni los extremos de línea, y sí el ruido aleatorio y el tramado (el damero incluido, porque tiene rachas en las dos diagonales).
- **Medida por página: proporción de motas = motas / texeles activos**, es decir, cuánto del detalle de alta frecuencia es grano y no bordes. La primera versión (fracción de texeles con 3 o 4 vecinos distintos, con umbral 6 y sin el criterio de líneas) confundía relieves, rejas y ventanas con ruido: daba 0,32 a los relieves de 26 (página `85c6c258`) y 0,41 al metal azul; con la proporción de motas quedan en 0,12 y 0,21–0,33.
- **Máscara local** (solo en las páginas que se tratan): densidad de motas medida con umbral 6 (un paso de 5 bits), sumando los texeles con 3 o 4 vecinos distintos de bajo contraste (ninguna diferencia mayor que 56: las rayas cortas del metal cepillado de 20, que el criterio de líneas descarta), suavizada con una gaussiana de σ = 2 texeles normalizada por la cobertura opaca y llevada a 0..1 con un smoothstep entre 0,12 y 0,28.

Distribución sobre las 1084 imágenes de página del build (`out\all\pages.csv`; histograma y muestras ordenadas con la máscara superpuesta en `out\noise_eval\page_noise_hist.png` y `sorted_by_ratio.png`): percentiles 5/25/50/75/95 de la proporción de motas = 0,033 / 0,074 / 0,136 / 0,218 / 0,515. Hay un grueso de páginas limpias (edificios, letreros, máquinas: menos de 0,15), una cola larga muy ruidosa (roca, tierra, pasto, agua: 0,3–0,6) y una franja dudosa entre 0,14 y 0,20 (pasto fino, estanterías, grava de la calle).

**Candidatos** (`noise_eval.py compare`): 18 recortes de 64×64 texeles de 16 páginas, 11 en páginas que se tratan (20 metal y piso, 14 roca y bloques, 9 tierra, 11 cascada, 26 granito con tótem y casas) y 7 en páginas limpias que no deben cambiar (relieves y máquina de 26, calle, edificios y estantería de 5). Cada hoja `cmp_<recorte>.png` muestra original nearest x4 | actual | 41 candidatos; `short_<recorte>.png` es una selección de 9 y `zoom_<recorte>.png` un acercamiento 8x de 8 recortes. Métricas medias sobre los 11 recortes de páginas tratadas (`metrics.csv`): *fidelidad* = error medio de la reducción por caja contra el original (0–255); *centro* = error del centro de cada texel HD contra su color original (bajo = cada mota del grano sigue legible); *subtexel* = estructura inventada dentro de cada texel (alto = manchas o detalle nuevo); *limpias* = diferencia máxima con el resultado actual en los recortes de páginas limpias.

| Candidato | Fidelidad | Centro | Subtexel | Limpias |
|---|---:|---:|---:|---:|
| actual: waifu2x cunet n0 | 2,70 | 6,04 | 9,85 | 0 |
| waifu2x cunet n−1 (sin reducción de ruido) | 1,52 | 6,12 | 10,17 | 168 |
| waifu2x cunet n−1 x2 + Lanczos | 1,74 | 5,22 | 10,13 | 153 |
| waifu2x anime n−1 (photo: igual) | 1,86 | 5,82 | 10,74 | 207 |
| Real-ESRGAN x4plus | 11,42 | 13,27 | 12,25 | 240 |
| NMKD Siax / LSDIR Compact (ncnn) | 5,61 / 7,77 | 14,10 / 16,74 | 20,89 / 22,39 | 204 / 212 |
| HFA2k (ncnn) | 11,69 | 10,23 | 6,39 | 185 |
| PBRify SPANV4 / HDCube (`.pth`, CPU) | 9,26 / 9,92 | 9,30 / 14,47 | 11,57 / 19,43 | 214 / 254 |
| xBRZ | 3,63 | 0,46 | 4,88 | 218 |
| hqx | 4,02 | 2,01 | 3,84 | 207 |
| nearest suavizado (gaussiana de 0,7 px HD) | 4,11 | 0,30 | 4,32 | 150 |
| actual + alta frecuencia del original (nearest) | 11,94 | 16,24 | 8,16 | 136 |
| waifu2x sobre base bilateral + grano del original | 3,15 | 5,11 | 8,81 | 116 |
| 50 % actual + 50 % nearest suavizado | 2,86 | 2,92 | 6,67 | 75 |
| filtro por página + máscara + nearest suavizado | 4,00 | 0,46 | 4,47 | 0 |
| **elegido: filtro por página + máscara + hqx** | 4,00 | 2,09 | 4,05 | 0 |

Lo que se ve en las hojas:

- **waifu2x sin reducción de ruido (−1) no lo arregla**: la acuarela no la produce el reductor de ruido sino la reconstrucción (el modelo dibuja formas a partir del grano). Mejora la fidelidad por caja, pero el centro y la estructura subtexel quedan igual que con n0; photo y anime hacen lo mismo.
- Los modelos extra de la sección 3 inventan material (Siax, LSDIR, HDCube, PBRify) o aplanan el grano (x4plus, HFA2k), con poca fidelidad.
- xBRZ conserva los colores pero dibuja diagonales duras (aspecto posterizado); el nearest suavizado es lo más fiel pero se ve como píxeles agrandados; **hqx deja cada mota como un cuadrado suave y suaviza las líneas**: es el grano más legible sin perder las formas.
- Los híbridos sobre waifu2x (sumarle la alta frecuencia del original, correr waifu2x sobre una base sin grano y devolverle el grano, mezclas al 50 %) dejan las manchas debajo o duplican el ruido.
- Tratar páginas enteras no sirve: las páginas son atlas (letreros junto a grava, relieves dentro de granito, casas junto a arbustos). Por eso el tratamiento se mezcla por texel con la máscara, y el filtro por página garantiza que las páginas limpias no cambien.

**Decisión:** si la proporción de motas de la página es ≥ 0,18, HD = (1 − m)·waifu2x + m·hqx, con m = la máscara local subida a 4x con interpolación bilineal (las ventanas vecinas siguen continuas); si es menor, waifu2x tal cual. Para el informe, una página tratada con máscara media ≥ 0,5 se llama *ruidosa* y el resto *mixta*. Justificación: en los paneles de 20,0 (el defecto visto en el juego) el grano queda como bloques de los colores originales en lugar de manchas, en la roca de 14 se lee el moteado y las raíces siguen siendo líneas, los relieves dentro del granito de 26 siguen legibles y las páginas limpias (los relieves de 26 en `85c6c258`, la calle de 5 en `972f7778`, edificios y máquinas) dan exactamente el resultado anterior. El nearest suavizado fue el segundo (más fiel, pero más "pixelado"); se elige con `GRAIN = "soft"` en `page_noise.py`. Resumen visual: `out\noise_eval\decision_sheet.png` (original | actual | elegido | máscara; en gris las páginas limpias, donde la máscara no se aplica).

**Páginas animadas** (las cascadas de 9.1): cada cuadro usa la medida y la máscara de su página de la ROM, con 0 en los texeles que cambia algún cuadro (más uno de margen). Todos los cuadros reciben así el mismo tratamiento fuera de la tira y la tira animada queda con waifu2x, como antes.

**Integración** (paso 3b de `build_all_terrain.py`): solo CPU (8 procesos) y determinista. Cada página tratada se guarda como `pages_4x\w2x-cunet_x4_n0_grain\<página>_<id de tratamiento>.png`, donde el id es un hash de los parámetros y de la máscara, y el manifiesto del directorio de DDS guarda `página:tratamiento`: cambiar un parámetro solo rehace las páginas y ventanas afectadas, y una segunda corrida no rehace nada. Cada página tratada pasa el mismo control que las salidas de la GPU (`upscale_ok`; si fallara se usaría la de waifu2x y se informaría). Nuevo en el paso 4: cada DDS recién convertido se decodifica y se compara con su recorte (error medio ≤ 8 sobre 255; BC7 da 1–2,5) y los que fallen se rehacen con texconv en CPU, porque la GPU es compartida con el juego en prueba. `out\all\pages.csv` lista cada página con su medida, clase y tratamiento; `summary.json` agrega el bloque `grain` y `dds_verify`. `--grain off` vuelve a los packs de 9.1. Los packs conservan ids, nombres de archivo y estructura (`mod.json`, `rt64.json`, caché de mips bajos y 19 811 DDS); solo cambian la descripción y la versión (0.2.0), así que `install_all.ps1` funciona igual.

**Números (build completo del 2026-10-02):**

| Clase | Páginas | Ventanas | Dibujos |
|---|---:|---:|---:|
| limpia (waifu2x sin cambios) | 692 | 13 314 | 231 488 |
| mixta (tratada, máscara media < 0,5) | 90 | 1056 | 28 879 |
| ruidosa (tratada, máscara media ≥ 0,5) | 302 | 5441 | 111 857 |

- Tratadas 392 páginas (6497 ventanas, 33 %; 190 de los 222 cuadros animados), ninguna rechazada. Por área: todas las ventanas de 20 (496), casi todas las de 9 (999 de 1003), 17 (341 de 405), 14 (383 de 504), 10 (845 de 1037), 11 (827 de 1194) y 29 (1208 de 1982); 253 de 903 en 26, 60 de 901 en 5, 1 de 960 en 13 y ninguna en 4, 7, 15, 21, 23 y 25.
- Tiempo: build completo con el tratamiento calculado desde cero 511 s (antes, con todo en caché, 436 s): tratamiento 24 s, recorte + 6497 DDS + verificación 32 s; el resto son el replay, las verificaciones C++ y los 31 packs. Una segunda corrida tarda 328 s y no recalcula ninguna página ni DDS (el paso 3b solo vuelve a medir las 1084 páginas, 10 s).
- Tamaño: pack combinado 175,7 → 163,5 MB (−7 %, probablemente porque el BC7 de las zonas con hqx, más planas dentro de cada texel, se comprime mejor con zstd); los 30 packs por área suman 198,6 → 183,0 MB (área 20: 7,3 → 6,1; 14: 6,8 → 5,6; 26: 9,1 → 8,8; 4 y 5 sin cambio).
- Verificaciones: 19 811/19 811 hashes idénticos con el hasher C++ de RT64 y 19 811/19 811 TMEM reconstruidas por su cargador (estados sin cambio: 18 341 exactas, 1415 con el defecto de un texel, 55 fuera del borde); 6497 DDS nuevos decodificados (peor error medio 2,49, ninguno rehecho); 0 páginas con el respaldo xBRZ. Las 13 314 ventanas de páginas limpias tienen DDS idénticos byte a byte (CRC) a los del pack de 9.1; de las 6497 de páginas tratadas cambian 5345 (el resto cae en zonas con máscara 0). Recalcular una página tratada da los mismos bytes, y dos packs armados con los mismos DDS tienen el mismo CRC en cada entrada (los `.rtz` solo difieren en las fechas que texture_packer pone a las entradas). Con `--grain off`, en un build parcial de 14, 20 y 26, 1879 de 1903 DDS son idénticos a los de 9.1; los 24 restantes son ventanas compartidas con el área 5, que en un build parcial se recortan de otra página (comportamiento previo de los subconjuntos). Copia del pack de 9.1 para comparar: `out\noise_eval\before\mm64_hd_terrain_all_w2x_before_9.2.rtz` (mismo mod id: instalar uno u otro).

**Cómo volver a correrlo:**

```
cd C:\Users\Usuario\Devel\tools\upscale\terrain
python noise_eval.py measure              # distribución de la medida (unos 20 s)
python noise_eval.py compare              # hojas de comparación en out\noise_eval\ (GPU solo la primera vez: un lote chico por modelo)
python build_all_terrain.py               # packs con el tratamiento (incremental)
python build_all_terrain.py --grain off   # packs como en 9.1
```

Los parámetros están al principio de `page_noise.py` (`PAGE_RATIO`, `LO`/`HI`, `TAU_MASK`, `LOWC`, `GRAIN`); como el id de tratamiento los incluye, el siguiente build rehace solo lo afectado.

**Prueba en el juego (2026-10-02):** pack instalado solo durante la prueba y quitado después (`mods.json`
restaurado); capturas con `area_scan.ps1 -Areas @("20,0","14,1","26,0","5,0") -Full -Suffix "_hd2"`, comparadas con
las de 9.1 (`_hd`) y sin pack (`_sd`) en `shots\hd2_crop_<área>.png`. El metal azul de 20,0 conserva el grano del
original en lugar de las manchas de acuarela; las paredes de roca de 14,1 también; el piso de 14,1 queda con
bloques de texel visibles (hqx respeta cada texel: más fiel al original, menos "liso"); la ruina de 26,0 y la calle de
5,0 no cambian. Sin costo de GPU apreciable.

**Pendiente:** la franja 0,14–0,18 queda sin tratar (por ejemplo la mitad de grava de la página de la calle de 5, `972f7778`, sigue con waifu2x para no tocar las marcas); las tiras animadas siguen con waifu2x; hqx es Python puro (unos 0,1 s por página de 128², suficiente para unas 400 páginas).
