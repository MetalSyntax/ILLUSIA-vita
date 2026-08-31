# Plan de Port — ILLUSIA (PS Vita)

> Generado por psvita-port-toolkit el 2026-08-18. Punto de partida con lo detectado automáticamente --
> confirmar todo con objdump/Ghidra/jadx a mano antes de asumirlo como cierto.

## 0. Contexto

- **Juego:** ILLUSIA
- **Paquete Java:** com.gamevil.illusia
- **APK original:** `illusia-102.apk`
- **TITLEID asignado:** `ILLUSIA01`

**¿Motor conocido?** Confirmado (2026-08-20): **Gamevil Nexus2** (`com.gamevil.nexus2`), envuelto por
`com.gamevil.illusia.IllusiaLauncher extends NexusGLActivity`. Si hay un port hermano bajo la misma
BASE_DIR con el mismo paquete `com.gamevil.nexus2` en su APK, comparte ciclo de vida nativo y
convención JNI -- confirmar símbolos antes de reusar código a ciegas (el subsistema de fuentes GFA/
NexusFont en particular puede variar entre títulos).

## 1. Detección automática

- **ABI(s):** armeabi
- **ABI elegida:** armeabi
- **Nota de arquitectura:** Solo armeabi (ARMv6, soft-float) -- Vita lo ejecuta igual (ARMv7 es superset), sin NEON de v7a.
- **Versión de GLES:** AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES1 (pipeline fijo: glVertexPointer/glClearColorx/glTexParameterx))

## 2. .so encontrados (ABI armeabi)

- `illusia_extract/lib/armeabi/._libgameDSO.so` (4 KB)
- `illusia_extract/lib/armeabi/libgameDSO.so` (1043 KB)


## 3. Exports JNI (convención `Java_*`)

Confirmado con `nm -D illusia_extract/lib/armeabi/libgameDSO.so`: **13 exports** por convención de
nombre, **sin `RegisterNatives`** (grep -c en el pseudo-C de Ghidra = 0). `JNI_OnLoad` solo guarda el
`JavaVM*` global y devuelve `JNI_VERSION_1_6`.

- `Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef`
- `Java_com_gamevil_nexus2_Natives_NativeAsyncTimerCallBack`
- `Java_com_gamevil_nexus2_Natives_NativeDestroyClet`
- `Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo`
- `Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize`
- `Java_com_gamevil_nexus2_Natives_NativeIsNexusOne`
- `Java_com_gamevil_nexus2_Natives_NativeNetTimeOut`
- `Java_com_gamevil_nexus2_Natives_NativePauseClet`
- `Java_com_gamevil_nexus2_Natives_NativeRender`
- `Java_com_gamevil_nexus2_Natives_NativeResize`
- `Java_com_gamevil_nexus2_Natives_NativeResponseIAP`
- `Java_com_gamevil_nexus2_Natives_NativeResumeClet`
- `Java_com_gamevil_nexus2_Natives_handleCletEvent`

Ciclo de vida real (`NexusGLActivity`/`IllusiaLauncher`, ver `decompiled/apk_jadx/sources/com/gamevil/`):

| Android                          | Nativo (JNI)                          |
|-----------------------------------|----------------------------------------|
| `onCreate` (estático, load lib)   | `System.loadLibrary("gameDSO")` + `JNI_OnLoad` |
| `onResume`                        | `Natives.InitializeJNIGlobalRef()`     |
| Frame de `GLSurfaceView`          | `Natives.NativeRender()`               |
| Input táctil/teclado              | `Natives.handleCletEvent(x,y,type,?)`  |
| `onDestroy`                       | `Natives.NativeDestroyClet()`          |
| Pausa/resume de juego             | `Natives.NativePauseClet()` / `NativeResumeClet()` |

**Nota — subsistema de fuentes (riesgo alto):** `InitializeJNIGlobalRef` también hace
`FindClass("com/gamevil/nexus2/NexusFont")`. Las ~20 funciones `GFA_*` del .so (dibujo/medida de texto)
llaman de vuelta a esa clase Java, que usa `android.graphics.{Canvas,Paint,Bitmap,Typeface}` para
rasterizar y devolver píxeles crudos (`GFA_GetPixels16/32`). **No hay Android VM en Vita** -- este
puente debe reimplementarse nativamente (stb_truetype/freetype + fuente bundleada) antes de que
aparezca cualquier texto en pantalla. Ver detalle en `port_progress.md` Fase 3. No es parte del
bootstrap del loader; es un ítem propio en la fase de Gráficos/UI (sección 4).

**Otros hallazgos de imports (nm -D, 70 símbolos totales):**
- Sin `pthread_*` -- single-threaded, `NativeRender()` es el único punto de entrada del frame.
- GLES1.1 fixed-pipeline confirmado (`glVertexPointer/glClearColorx/glTexParameterx/glOrthof/glActiveTexture`).
- Red vía BSD sockets nativos (`socket/connect/send/recv/select`), no vía `java.net`.
- I/O de archivos vía libc real (`fopen/fread/...`), **no** `AAssetManager` -- confirmar en Fase 6 cómo
  el .so localiza los assets (`illusia-102/assets/`) en disco.


## 4. Checklist

- [x] Repo creado desde soloader-boilerplate, git init, .gitignore anti-DMCA.
- [x] APK decompilado (jadx) y .so decompilado(s) (Ghidra) -- ver sección 2/3.
- [x] Análisis del motor real (ciclo de vida nativo, reuso de otro port o boilerplate genérico) -- ver sección 3.
- [x] Bootstrap del loader: so_file_load/so_relocate/so_resolve -- ya en el scaffold genérico
  (`source/utils/init.c`), solo hizo falta corregir `SO_PATH` y sumar `inet_addr` a `dynlib.c`.
- [x] Tabla JNI (FalsoJNI): registrar exports + callbacks hacia "Java" -- `source/java.c` completo
  (63 métodos, ver `port_progress.md` Fase 4 para el detalle de qué es real vs. stub).
- [ ] Primer arranque (Vita3K primero, consola real después) -- **código listo, sin compilar/probar**
  en esta sesión (sin VITASDK instalado acá). Ver "Sin probar todavía" en `port_progress.md` Fase 4.
- [ ] Gráficos (wrappers GL según versión detectada).
- [ ] Input, Audio, Assets, LiveArea/VPK.
- [ ] Pruebas en hardware real.

## 5. Herramientas

Este port se gestiona con **psvita-port-toolkit** (standalone, fuera de este repo). Desde el
toolkit: `Continuar con un port existente` → elegí esta carpeta (ya tiene `.psvita-toolkit.json`).


## 6. Próximos Features Pendientes
- [x] **Audio / Sonido**: Implement `NexusSound` stubs (`OnSoundPlay`, `OnStopSound`) using a dedicated audio thread to play the WAV files directly via `sceAudioOutOutput`.
- [x] **I/O de Guardado (Save States)**: Redirect I/O from `assets` to `saves` so save files load properly. (Done: redirected `m_getAbsolueFilePath` to `saves` and created the directory at boot).
- [x] **FPS Cap**: Limit render loop to 30 FPS using `sceKernelGetProcessTimeLow` to prevent the game logic from running twice as fast.
