# Roadmap: revamp gráfico de Mega Man 64 (y kit reutilizable)

## Prompt refinado (2026-10-02)

> Construir en RT64 un modo de **iluminación raster mejorada** que dé el mismo aspecto que el path tracer
> (sombras del sol, luz direccional, oclusión ambiental, cielo bonito, atmósfera, bloom) usando técnicas clásicas
> y baratas, sin hardware de ray tracing, para que funcione en GPUs modestas (gráficas integradas, Steam Deck,
> Android/Quest) y en D3D12, Vulkan y Metal. Debe ser **agnóstico al juego**: un módulo genérico de RT64 que solo
> recibe pistas del host (cámara/mundo, escena, interiores), reutilizable en otros decomps de N64, con shaders y
> algoritmos documentados para portarlos a ports de PS1.
>
> Requisitos:
> 1. **Sombras del sol** con shadow maps estabilizados (texel snapping en espacio de mundo, cascadas o mapa único),
>    PCF suave, bias por pendiente y normal offset, **alpha test** para que las hojas de los árboles proyecten
>    sombras recortadas. Sombras de contacto en espacio de pantalla para detalle fino.
> 2. **Oclusión ambiental** (GTAO/horizon AO a media resolución + upsample bilateral), luz de cielo hemisférica,
>    relighting N·L con normales suaves, normalizado para no lavar la iluminación horneada del N64.
> 3. **Interiores/dungeons bonitos**: sin sol, luz de linterna/puntual con sombra suave, ambiente coloreado,
>    niebla de distancia, emisivos que brillen.
> 4. **Cielo**: cielo procedural (atmósfera + nubes 2D/2.5D baratas) que reemplace el cielo 2D en exteriores,
>    god rays por radial blur, perspectiva aérea.
> 5. **Post**: bloom (dual filter), tonemap/grading LDR (vibrance, contraste, curva), CAS, viñeta sutil,
>    aplicado a la escena 3D y no al HUD.
> 6. **Árboles**: eliminar el vaivén de UV y el ruido de hojas del path tracer; bordes recortados con antialiasing,
>    sombras de hojas, translucidez a contraluz, normales "volumétricas"; experimento de árboles 3D procedurales.
> 7. **Calidad por presets** (Bajo/Medio/Alto/Ultra) desde el menú Gráficos, con costo medido
>    (`RT64_PRINT_FRAME_TIME`) y objetivos: Bajo ≤ 0,5 ms, Medio ≤ 1,5 ms, Alto ≤ 3 ms a 1080p en GPU media.
> 8. **Robustez**: MSAA, widescreen/expand, resoluciones altas, VR estéreo (dos viewports), HUD y 2D intactos,
>    transparencias sin oscurecer, sin parpadeos al girar la cámara (texel snapping), sin artefactos en el cielo.
> 9. **Remake experimental**: texturas HD (dump + reescalado con licencia libre + texture pack de RT64), estudio
>    de reemplazo de modelos (formato del juego vía decomp, hooks del renderer), mapas de normales derivados.
> 10. **Proceso**: investigar lo existente (ReShade, XeGTAO, Bend SSS, FidelityFX CAS, etc., solo licencias
>     compatibles con MIT para código), commits por funcionalidad, capturas comparativas (raster original /
>     mejorado / path tracing) en exterior, ciudad, árboles e interior, mediciones de rendimiento, y documentarlo
>     todo en `docs/rendering/` (handbook incluido) para no repetir investigación.

## Estado

| Línea | Estado | Notas |
|---|---|---|
| Handbook + memoria | hecho (inicial) | `HANDBOOK.md`, memoria del agente |
| Investigación de técnicas | en curso | `techniques-research.md` |
| Investigación de remake (texturas/modelos) | en curso | `remake-research.md` |
| Iluminación raster: infraestructura | hecho | `enhanced-lighting.md`; opacos → iluminación → translúcidos → post |
| Shadow map + alpha test | hecho | mapa estable alineado al mundo, PCF por preset |
| G-buffer de normales (replay) | hecho | normales suaves, *sphere impostor* en follaje |
| AO | hecho | GTAO a media resolución + blur bilateral |
| Composición (sol, cielo, AO, interiores) | hecho | linterna en interiores; ajustar dungeons |
| Cielo procedural raster | en curso (agente) | `LightingSky`, `procedural-sky.md` |
| God rays, bloom, grading, CAS | en curso (agente) | `PostEffects`, `post-effects.md` |
| Menú y presets | hecho | Enhanced Lighting On/Off (F3), Lighting Quality Low–Ultra |
| Árboles | en curso | volumen + sombras de hojas + translucidez hechos; PT sin vaivén/ruido; faltan alpha-to-coverage y viento por vértice |
| Texture pack HD | prototipo | pack de modelos (2577 texturas, waifu2x) sin probar en juego; ver `remake-research.md` |
| Reemplazo de modelos (experimento) | investigado | hook en `func_800805F8`; árbol procedural de 618 triángulos |
| Rendimiento | en curso | fusionar casters con alpha test, normales suaves O(n²) |
