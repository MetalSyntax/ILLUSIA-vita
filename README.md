# ILLUSIA — Port a PS Vita

Port de **ILLUSIA** (Android, `com.gamevil.illusia`, motor **Gamevil Nexus2**) a PS Vita vía
soloader (SoLoader + FalsoJNI). Generado con **psvita-port-toolkit**.

## Estado actual

- **Fase 1-2 (setup + decompilación):** completas.
- **Fase 3 (análisis del motor real):** completa. Ver `port_progress.md` para el detalle.
  - Convención JNI estándar `Java_com_gamevil_nexus2_Natives_*` (13 exports), sin `RegisterNatives`.
  - GLES1.1 fixed-pipeline confirmado.
  - Single-threaded, red por BSD sockets, I/O de archivos por libc real (no `AAssetManager`).
  - ⚠️ Riesgo alto identificado: el renderizado de texto (`GFA_*`) delega en `android.graphics.*` vía
    callback JNI a la clase Java `NexusFont` -- requiere reimplementación nativa dedicada.
- **Fase 4 (bootstrap del loader):** código completo, **sin compilar/probar todavía** (este entorno no
  tiene VITASDK instalado). `source/java.c` tiene la tabla FalsoJNI completa (63 métodos), `source/main.c`
  tiene la secuencia real de arranque (`InitializeJNIGlobalRef` → `NativeInitWithBufferSize` →
  `NativeInitDeviceInfo`/`NativeResize` → loop de `NativeRender`). Ver `port_progress.md` Fase 4 para el
  detalle completo y los pasos manuales pendientes antes del primer build (copiar `.so`/assets al
  destino de deploy).
- **Fase 5 (input):** pendiente -- `handleCletEvent` no está mapeado a sceTouch/sceCtrl todavía.
- **Fase 6 en adelante:** pendiente (texto real con stb_truetype, audio, LiveArea/VPK final).

Ver `PORTING_PLAN.md` para el plan completo y `port_progress.md` para la bitácora de progreso.

## Estructura

- `illusia-102/`, `illusia_extract/` — APK original y extraído (gitignored).
- `decompiled/` — Java (jadx) y pseudo-C (Ghidra) del `.so` (gitignored, regenerable).
- `source/`, `lib/so_util`, `lib/falso_jni` — scaffold del boilerplate (SoLoader + FalsoJNI).
- `PORTING_PLAN.md` — plan vivo, actualizado a medida que se confirman cosas del motor real.
- `port_progress.md` — bitácora, un bug confirmado a la vez.
- `.psvita-toolkit.json` — config para el toolkit standalone (build/deploy/logs/LiveArea/crash dumps).

Este port **no** tiene una copia local de `porting_tools/` -- todo el build/deploy/debug se maneja
desde **psvita-port-toolkit**, la herramienta standalone (fuera de este repo). Abrí el toolkit y
elegí "Continuar con un port existente" apuntando a esta carpeta.

## Flujo de trabajo esperado

1. Análisis de símbolos antes de tocar loader/source -- skill `psvita-port-init` cubrió la Fase 0-2.
2. Bootstrap del loader guiado por la skill `psvita-porting`.
3. Build/deploy con el toolkit standalone → probar en Vita3K → probar en consola real.
4. Un bug a la vez, guiado por el log real -- skill `so-crash-triage`.
5. Actualizar `port_progress.md` con cada bug confirmado.
