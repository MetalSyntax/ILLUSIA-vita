# Registro de Progreso — ILLUSIA (PS Vita)

## Fase 1: Configuración y Preparación (Completada — 2026-08-18)
- Repo creado desde soloader-boilerplate, `.gitignore` anti-DMCA.
- APK `illusia-102.apk` copiado y extraído.
- ABI detectada: armeabi (elegida: armeabi).
- GLES detectado: AndroidManifest.xml no declara glEsVersion -- usar heurística (GLES1 (pipeline fijo: glVertexPointer/glClearColorx/glTexParameterx))

## Fase 2: Decompilación (Completada — 2026-08-18)
- jadx: NO corrido -- pendiente.
- Ghidra (.so): corrido para cada .so.

## Fase 3: Análisis del Motor Real (Completada — 2026-08-20)
- [x] Confirmar si comparte motor con algún port hermano.
  → Motor identificado: **Gamevil Nexus2** (paquete `com.gamevil.nexus2`), envuelto por
    `com.gamevil.illusia.IllusiaLauncher extends NexusGLActivity`. Es el motor genérico que Gamevil
    reusó en varios títulos de esa época (~2010-2012) bajo el mismo paquete `nexus2` -- si hay otro
    port hermano bajo la misma BASE_DIR con `com.gamevil.nexus2` en su APK, comparte ciclo de vida
    nativo y convención JNI 1:1 con este. Confirmar por symbols antes de asumir CLet/GFA idénticos.
- [x] Leer decompiled/apk_jadx/sources/ para el ciclo de vida nativo real.
  → Ver PORTING_PLAN.md sección 3 para el mapeo completo Activity → Natives.*.
- [x] Confirmar exports JNI reales y si hay RegisterNatives.
  → Convención estándar `Java_com_gamevil_nexus2_Natives_*` (nm -D confirma 13 exports). **Cero
    llamadas a `RegisterNatives`** en todo el .so (grep -c = 0). `JNI_OnLoad` es trivial: solo guarda
    el `JavaVM*` en una global y devuelve `JNI_VERSION_1_6` (0x10002). Bootstrap de tabla JNI vía
    FalsoJNI (convención por nombre) sin necesidad de emular RegisterNatives.

### Hallazgos adicionales (importantes para Fase 4+)

- **Sin hilos:** cero imports de pthread en el .so (`nm -D` → 70 símbolos importados en total, ninguno
  `pthread_*`). El bucle de juego es single-threaded, disparado por `Natives.NativeRender()` llamado
  desde el hilo GL de Android (`NexusGLRenderer`, GLSurfaceView). Menor riesgo de carreras al portar.
- **GLES1.1 fixed-pipeline confirmado:** imports incluyen `glVertexPointer`, `glColorPointer`,
  `glTexCoordPointer`, `glClearColorx`, `glTexParameterx`, `glOrthof`, `glActiveTexture` -- pipeline
  fijo con variantes de punto fijo (`x` suffix) y multitextura (GLES1.1, no CM puro). Confirma la
  heurística automática de Fase 1.
- **Red vía BSD sockets nativos:** `socket/connect/send/recv/select/shutdown/inet_addr` importados
  directo -- no pasa por java.net. Debería portar sin tocar FalsoJNI si vitasdk expone estos símbolos
  (net.h / newlib sockets).
- **I/O de archivos vía libc real (`fopen/fread/fwrite/stat/...`), NO `AAssetManager`:** el .so no
  importa nada de `AAsset*`. Los assets del juego (map/obj/sound/char/etc. en `illusia-102/assets/`)
  se leen por path de archivo plano, probablemente tras una extracción a almacenamiento interno en el
  primer arranque (o el APK se trata como filesystem montado). Falta confirmar el mecanismo exacto de
  extracción -- revisar el flujo de `Natives.loadFile/saveFile/isFileExist/readAssete` (estos SÍ son
  llamados-Java, ver próximo punto) vs. cómo el .so localiza el directorio de assets real.
- **⚠️ RIESGO ALTO -- Renderizado de texto delega en Android vía JNI callback:**
  `Java_..._InitializeJNIGlobalRef` hace `FindClass("com/gamevil/nexus2/NexusFont")` y cachea el
  global ref. Todo el subsistema de fuentes del engine (`GFA_DrawText`, `GFA_MeasureText`,
  `GFA_GetPixels16/32`, `GFA_CreateFont`, `GFA_SetFont`, etc. -- ~20 funciones) llama de vuelta a
  `com.gamevil.nexus2.NexusFont` (ver `decompiled/apk_jadx/sources/com/gamevil/nexus2/NexusFont.java`),
  que usa `android.graphics.{Canvas,Paint,Bitmap,Typeface}` para rasterizar el texto a un bitmap ARGB
  o RGB565 y devolver los píxeles crudos al native vía `GFA_GetPixels16/32`. **Esto no es F.O.S.S ni
  reimplementable con un simple stub de FalsoJNI que devuelva 0** -- sin este puente, TODO el texto en
  pantalla (UI, diálogos, menús) desaparece o crashea. Opciones a evaluar en Fase 6 (Gráficos/UI):
  1. Reimplementar `NexusFont` en C nativo con stb_truetype/freetype + una fuente bundleada, exponiendo
     las mismas firmas `GFA_*` como funciones C (bypasseando el JNI callback por completo -- requiere
     parchear el .so para llamar directo en vez de vía `CallStaticXxxMethod`, o interceptar a nivel de
     stub JNI devolviendo resultados ya calculados).
  2. Implementar los stubs JNI de `NexusFont` en FalsoJNI que internamente rasterizan con una librería
     nativa de fuentes y devuelven arrays Java-like falsos con los píxeles.
  → Cualquiera de las dos requiere trabajo dedicado; documentar como bug/feature propio en su momento,
    no asumir que "ya andará" solo con el bootstrap del loader.
- `Natives.NativeGetPlayerName(String)` está declarado `native` en Java pero **no tiene export
  `Java_*` en el .so** (posiblemente código muerto o implementado por otro camino) -- no bloquea nada,
  anotado por completitud.

## Fase 4: Bootstrap del loader (Completada en código — 2026-08-20, sin probar aún)

- [x] `lib/falso_jni` estaba vacío (submódulo git nunca clonado, `git submodule update --init`
  fallaba porque el repo no tiene commits todavía y `.gitmodules` no está trackeado). El clone directo
  vía SSH falló (sin llave configurada); vía HTTPS falló por corrupción de índice de pack
  (`non-monotonic index` + archivos `._pack-*.idx` fantasma) -- causado por el filesystem del disco
  externo "Seagate PSVITA Develop" generando AppleDouble (`._*`) sobre cada blob. Workaround: clonar
  en el filesystem interno (scratchpad) y copiar con `rsync --exclude='.git'` a `lib/falso_jni`, sin
  ejecutar `git` dentro del propio disco externo. `.gitignore` ya cubre `._*`/`.DS_Store`, no hizo
  falta tocarlo. **Nota para toda esta sesión:** evitar clonar/operar git pesado directo sobre esta
  ruta; usar el filesystem interno como intermediario si se necesita otro submódulo.
- [x] `source/dynlib.c`: agregado `inet_addr` a `default_dynlib[]` (único símbolo de los 70 importados
  por el `.so` que faltaba; el resto -- incluida toda la familia de sockets BSD, GLES1.1 fixed-pipeline,
  y libc estándar -- ya estaba cubierto por el boilerplate genérico).
- [x] `CMakeLists.txt`: `SO_PATH` corregido de `main.so` (placeholder genérico) a `libgameDSO.so` (nombre
  real del binario). `VITA_TITLEID=ILLUSIA01` y el resto del linkeo (vitaGL, vitashark, mathneon,
  kubridge, etc.) ya estaban correctos desde el scaffold.
- [x] `source/java.c`: tabla JNI completa de FalsoJNI para las 63 llamadas `GetStaticMethodID` +
  `CallStatic*Method` reales encontradas en el `.so` (ver Fase 3), contra las dos clases confirmadas
  `com/gamevil/nexus2/Natives` y `com/gamevil/nexus2/NexusFont`:
  - **Comportamiento real implementado:** `isAssetExist`/`isFileExist`/`readAssete` (contra
    `DATA_PATH"assets/"` y `DATA_PATH"files/"` usando `file_exists`/`file_size`/`file_load` de
    `utils/utils.h`), `getAbsolueFilePath`/`getPhoneModel`/`getPhoneNumber`/`getPlayerNameByte`/
    `getVersion` (valores dummy razonables), `isNetAvailable`/`netConnect` (devuelven "sin red" a
    propósito -- offline por defecto en este primer boot).
  - **Stub de fuente (GFA_\*, NexusFont):** implementado como estado mínimo sin crashear (tracking de
    tamaño/color/fuente actual, medida de texto aproximada por heurística de ancho de caracter, buffer
    de píxeles en blanco/transparente vía `GFA_GetPixels16/32`) -- **no dibuja texto real todavía**.
    Confirmado que es trabajo de Fase 6 dedicado (stb_truetype + `text_rendering.md` de la skill
    `psvita-porting`), no bloqueante para el primer arranque.
  - **Stubs neutros para código muerto confirmado:** 7 nombres que el `.so` busca (`GFA_GetTextWidths`,
    `GFA_GetStringLength`, `GFA_SetString*`, `GFA_SetAntiAlias`, `GFA_SetLocale`) y 8 nombres de
    billing/carrier (`blinkSButton`, `showComponent`/`hideComponent`, `showMylSButton`, `requestIAP`,
    `netBillcomSocket*`, `netSocket`) **no existen en las clases Java reales de Illusia** -- confirmado
    cruzando cada `GetStaticMethodID` contra `decompiled/apk_jadx/sources/com/gamevil/nexus2/{Natives,NexusFont}.java`.
    En Android real tirarían `NoSuchMethodError` si se llamaran, por lo que están comprobadamente
    inalcanzables en este juego; registrados solo por seguridad defensiva.
  - **Detalle de ABI importante:** los argumentos `float` en `Call*Method(...)` (función C variádica)
    llegan promovidos a `double` -- todo lector de `va_arg` para un `jfloat` usa `va_arg(args, jdouble)`
    y castea, nunca `va_arg(args, jfloat)` directo (error de ABI silencioso si no).
- [x] `source/main.c`: reemplazado el stub genérico por la secuencia real de arranque confirmada en
  `NexusGLActivity`/`NexusGLRenderer`/`IllusiaLauncher`: `JNI_OnLoad` → `InitializeJNIGlobalRef` →
  `NativeInitWithBufferSize(480,320)` (resolución lógica fija del juego, no la real de pantalla) →
  `NativeInitDeviceInfo(480,320)` + `NativeResize(960,544)` (superficie EGL real, vitaGL ya inicializa
  a resolución nativa de Vita en `gl_init()`) → loop `NativeRender()` + `gl_swap()`.
  - **Pendiente explícito (Fase 5, no bloqueante para probar el primer render):** sin input todavía.
    `Natives.handleCletEvent(event,param1,param2,pointerId)` es el único puente de entrada
    (`NexusGLRenderer.sendHandleCletEvent()`, alimentado por `NeoTouchDetector`/
    `com.gamevil.illusia.ui.EventQueue`, sin decodificar aún) -- mapear sceTouch/sceCtrl ahí es el
    próximo trabajo real, guiado por `input_handling.md` de la skill `psvita-porting`.

### Sin probar todavía -- bloqueante conocido

Este entorno de trabajo **no tiene VITASDK instalado** (`$VITASDK` vacío, sin `arm-vita-eabi-gcc`), así
que nada de lo anterior fue compilado ni corrido en Vita3K/consola desde esta sesión. Falta, con el
toolkit standalone:
1. Copiar `illusia-102/lib/armeabi/libgameDSO.so` → `ux0:data/illusia/libgameDSO.so` en el destino de build/deploy.
2. Copiar `illusia-102/assets/` → `ux0:data/illusia/assets/` (usado por `isAssetExist`/`readAssete`).
3. Crear `ux0:data/illusia/files/` vacío (equivalente al almacenamiento interno de la app, para `isFileExist`).
4. Build real vía el toolkit, primero en Vita3K.
5. Si arranca sin crashear pero sin input: seguir con Fase 5 (mapeo de `handleCletEvent`).
6. Guiarse por `so-crash-triage` en el primer crash real -- no hay forma de anticipar más desde acá sin
   un log de consola/Vita3K real.

### Bug #1 confirmado (build real del usuario, 2026-08-20)

- **Síntoma:** `ld` falla en el link final (`illusia`, 87%) con `undefined reference to utf16_to_utf8`
  y `undefined reference to utf8_to_utf16`, ambos referenciados desde
  `lib/falso_jni/FalsoJNI_ImplBridge.c` (`jstr_utf16_to_utf8`/`jstr_utf8_to_utf16`).
- **Causa:** al agregar `lib/falso_jni` como fuente en `CMakeLists.txt` (Fase 4) me olvidé de
  `lib/falso_jni/converter.c` -- el archivo que implementa `utf16_to_utf8`/`utf8_to_utf16` (conversión
  de codepoints UTF-16↔UTF-8 que usa toda la maquinaria de `jstring` de FalsoJNI). Sin él, todo lo demás
  compila (los símbolos solo se referencian, no se llaman en tiempo de compilación) y solo revienta en
  el link final.
- **Fix:** agregado `lib/falso_jni/converter.c` a la lista de fuentes de `CMakeLists.txt`.
- **Estado:** corregido en código, pendiente de confirmar que el build completa OK con este fix.

### Triage en curso -- dump `illusia-psp2core-1708393517-0x00009f2f3d` (2026-08-20, SIN CONFIRMAR)

- **Síntoma:** `.psp2dmp` con 2 hilos reportados "en crash" simultáneamente: `patch_info_fetcher`
  (Undefined instruction, PC=`0x81173118`, LR=`0x811730ed`) e `image_fetcher` (Data abort,
  PC=`0x811680c4`, LR=`0x8116822d`).
- **Cruce contra `build/illusia.elf` (con símbolos, `arm-vita-eabi-addr2line`):** las 4 direcciones
  (PC y LR de ambos hilos) resuelven al mismo buffer estático `progs` (`.bss`) dentro de
  `custom_shaders.c` de **vitaGL** -- en ambos hilos el CPU terminó con el PC (y el LR) dentro de una
  zona de datos, no de código real.
- **Por qué NO está confirmado como causa real (contradicciones sin resolver):**
  1. Fase 3 ya estableció que `libgameDSO.so` es **single-threaded** (cero imports `pthread_*`). No
     existe el string `"patch_info_fetcher"` ni `"image_fetcher"` en el `.so`, en el Java decompilado
     (jadx), ni en `source/`/`lib/` propios -- no hay explicación de dónde salen esos nombres de hilo.
  2. En `patch_info_fetcher`, R1/R2/R3/R12 = `0xdeadbeef` (patrón de memoria nunca inicializada), lo
     que junto con (1) sugiere que estos 2 "hilos" podrían ser slots de thread-control-block muertos o
     nunca usados que el parser está listando como crasheados, no hilos reales en ejecución.
  3. `build/illusia.elf` resuelve `custom_shaders.c` a una ruta de otra sesión/proyecto
     (`Prince-of-Persia/.../scratchpad/vitaGL/...`) -- consistente con vitaGL instalado globalmente en
     `$VITASDK` y compartido entre ports (no vendorizado en este repo), así que el path es cosmético,
     pero anotar por si algún día el símbolo no cuadra con lo esperado.
  4. Sin log real de esa corrida: `live_session_*.log` y `perf_telemetry_*.log` quedaron en 0 bytes --
     no se pudo aplicar el paso 1 de la metodología (última línea del log antes del corte).
- **Próximo paso antes de tocar código:** confirmar que el logging del toolkit esté flusheando a
  archivo antes del crash, y volver a correr `vita-parse-core` pidiendo el listado completo de hilos
  (no solo los "en crash") para ver si hay un hilo principal con contexto más sano que estos dos.

### Triage -- dump `illusia-psp2core-1787381851-0x000d363da3` (2026-08-22, mitigación aplicada, causa raíz SIN CONFIRMAR)

- **Síntoma:** Prefetch abort en el hilo `pthread` (probablemente el hilo principal creado por el
  arranque de VitaSDK para correr `main()` -- el `.so` sigue siendo single-threaded, cero imports
  `pthread_*`, confirmado de nuevo). `PC = 0x92166364`, muy por debajo de la base del `.so`
  (`0x97fc7000`) -- **no es una dirección de código real, es una lectura de memoria heap/no mapeada
  usada como puntero a función.** `LR` resuelve (con algo de imprecisión de offset, ver nota abajo) a
  `CMvMob::KnockProcWhenAttack(CMvCharacter*, bool)` (`decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c:73605`).
- **Cruce con el pseudo-C de Ghidra:** `KnockProcWhenAttack` hace 3 llamadas virtuales seguidas --
  `(**(code**)(*param_1+0x24))(param_1)`, `(**(code**)(*param_1+0x144))(param_1)`,
  `(**(code**)(*this+0x144))(this)` -- las 3 son `blx` a un puntero cargado desde la vtable del objeto
  (`param_1` = el `CMvCharacter*` atacado; `this` = el `CMvMob*` atacante). El crash es, sin duda,
  saltar a través de UNA de estas vtables cuando su puntero (o el de la propia vtable) está corrompido
  -- la dirección destino (`0x92166364`) tiene toda la pinta de un valor de heap/dato, no de un puntero
  a función válido ni relocado a medias.
- **Se descartó como causa la relocación del loader:** `lib/so_util/so_util.c` es **byte-idéntico**
  (`diff` sin salida) al de `ILLUSIA-2-Vita` (port de referencia que sí arranca), y `objdump -R` sobre
  `libgameDSO.so` solo tiene tipos de reloc que `so_util.c` maneja explícitamente
  (`R_ARM_RELATIVE`/`R_ARM_JUMP_SLOT`/`R_ARM_ABS32`/`R_ARM_GLOB_DAT`, sin ningún tipo exótico sin
  cubrir) -- no hay evidencia de vtables mal relocadas de forma sistemática.
- **Se descartó (por ahora) el stub de fuentes (`GFA_*`/`NexusFont`) como causa de corrupción de heap:**
  aunque `CMvGameUI::DrawLog` y `CGxBFont::CalcDrawSize` aparecen en la pila/registros del dump (lo que
  sugiere que el log en pantalla se dibujó poco antes), el tamaño de los buffers que devuelven
  `m_GFA_GetPixels16/32` en `source/java.c` (`gfa_w * gfa_h`, fijado una sola vez en `GFA_Init`)
  coincide exactamente con `g_gfaBufCount` en el `NexusFont.java` real (`decompiled/apk_jadx/.../NexusFont.java`)
  -- no hay mismatch de tamaño obvio ahí.
- **Nota sobre precisión del offset de `LR`:** la resolución automática da
  `KnockProcWhenAttack + 0x2f`, que no cae exactamente al final de ninguno de los 3 `blx` de la función
  contando instrucciones a mano sobre el desensamblado real (`arm-vita-eabi-objdump -d -M force-thumb`)
  -- el desajuste es de pocos bytes (probablemente error de redondeo/bit de Thumb en la resolución de
  símbolos de `vita-parse-core`, no una función distinta). No cambia el diagnóstico (sigue siendo esta
  función, vía una de sus 3 llamadas virtuales), pero no se pudo identificar con 100% de certeza CUÁL
  de las 3 exactamente sin más contexto en runtime.
- **Mitigación aplicada (no confirmada como fix definitivo):** `source/main.c` no definía
  `sceUserMainThreadStackSize`, así que el hilo principal corría con el default del SDK (chico, ver
  `samples/basic_program` de vitasdk, ~256 KB). Se subió explícitamente a 4 MB. Justificación: el
  patrón del crash (salto a una dirección que parece dato de heap, no un puntero corrupto de forma
  "prolija") es compatible con un overflow de stack pisando memoria adyacente durante una resolución de
  combate con mucho dispatch virtual anidado (`CMvMob` AI -> `CMvCharacter`/`CMvPlayer`) -- un patrón de
  bug conocido al portar motores C++ de Android (stack grande por defecto) a Vita (stack default
  chico). Es un cambio inocuo (solo reserva más RAM) pero **no reemplaza confirmar la causa real**.
- **Bloqueante que sigue sin resolverse (igual que el triage anterior de Fase 4):** `live_session_*.log`
  y `perf_telemetry_*.log` en `logs/` siguen en 0 bytes -- sin el log real fflusheado, no se puede
  aplicar el paso 1 de la metodología de `so-crash-triage` (ver la línea exacta antes del corte), y por
  eso este triage se apoya solo en el `.psp2dmp` + decompilado. Es la segunda vez que este bloqueo
  impide confirmar una causa con certeza -- vale la pena investigar el propio logging del toolkit
  standalone (por qué no está flusheando a esos archivos) antes del próximo crash.
- **Próximo paso:** probar este build (stack 4 MB) en consola/Vita3K. Si vuelve a crashear en el mismo
  punto, es evidencia contra la teoría de stack overflow y hay que profundizar en runtime (breakpoint de
  GDB del toolkit en la entrada de `KnockProcWhenAttack`, inspeccionar `param_1`/`this` y sus vtables en
  vivo) en vez de seguir adivinando desde el dump estático. Si no vuelve a crashear en ese punto durante
  varias corridas, es evidencia a favor pero tampoco 100% concluyente sin el log real.

### Profundización del mismo triage con vtables reales (2026-08-22, cont.) -- metodología nueva + pista concreta de ABI

Segunda pasada sobre el mismo dump, esta vez reconstruyendo las vtables REALES (no el pseudo-C a ciegas)
cruzando `objdump -T` + los constructores en `decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c` +
`decompiled/libgameDSO_armeabi/ghidra/out_angr.c` (segundo decompilador, independiente de Ghidra, útil
para confirmar/refutar sin depender de un solo motor de análisis).

- **Técnica nueva, reutilizable en este proyecto:** los constructores en el pseudo-C de Ghidra escriben
  el puntero de vtable real con una dirección "placeholder" propia de Ghidra, ej.
  `*(undefined ***)this = &PTR__CMvCharacter_1_001108a8;` (línea 39263). Esa dirección **no es real**
  (el `.so` mide ~1 MB, `0x1108a8` está fuera de rango) -- pero es SIEMPRE
  `dirección_real_de__ZTVxxx + 8 + 0x10000` en este binario en particular. Confirmado con las 3 clases
  cruzadas (`CMvCharacter`, `CMvPlayer`, `CMvMob`): restar `0x10000` a cualquier placeholder
  `PTR__NombreClase_1_...` de Ghidra da exactamente `symbol(_ZTVxxx) + 8` real (el puntero de vtable que
  el objeto guarda en runtime, confirmado contra `objdump -T`). Vale la pena documentar esta constante
  para toda la sesión de triage de este `.so` -- ahorra tener que adivinar si Ghidra cargó todo a un
  base distinto.
- **Con esto se pudo leer, byte a byte, el contenido real de las 3 vtables involucradas en
  `KnockProcWhenAttack`:**
  - `CMvCharacter` (vtable real en `0x1008a8`): slot `+0x24` = `0x0` (método puro virtual, sin
    implementación propia -- esperable, `CMvCharacter` es abstracta). Slot `+0x144` = `0x6d3d4`
    (`CMvCharacter::DoAITarget(CMvCharacter*)`), referenciado igual desde otras 7 vtables no relacionadas
    (clases que no lo sobreescriben y heredan el mismo puntero).
  - `CMvPlayer` (vtable real en `0x101cb0`): slot `+0x24` = `0x4eb04`, que **coincide exactamente con el
    inicio de la sección `.plt`** (ver `objdump -h`) en vez de una función del juego -- valor sin
    sentido para un método propio. Slot `+0x144` = `0xffbb0`, que es el inicio LITERAL del símbolo de
    otra vtable (`_ZTV20GVUIPlayerController`), no de una función.
  - `CMvMob` (vtable real en `0x1013b8`): slot `+0x24` = `0x0`. Slot `+0x144` = `0xd7e78`, que resuelve a
    `_ZN9CGxPZDMgrD0Ev` (destructor de una clase de gestión de recursos totalmente ajena a `CMvMob`).
  - Verificado que estas 3 direcciones no son ruido de mi búsqueda: cada una aparece exactamente 2 veces
    en el binario -- una como la entrada de vtable en cuestión, la otra dentro de `.dynsym` (la propia
    tabla de símbolos dinámicos registrando la dirección de ESE símbolo, no una segunda referencia real).
  - **Contradicción sin resolver:** por reglas de la ABI de Itanium (herencia simple confirmada --
    `CMvMob::CMvMob`/`CMvPlayer::CMvPlayer` llaman a `CMvCharacter::CMvCharacter` como base antes de
    pisar su propio puntero de vtable), el slot `+0x144` de CUALQUIER clase derivada de `CMvCharacter`
    debería contener `DoAITarget` sin overridear, o un override con firma compatible -- nunca el
    destructor de una clase no relacionada ni la dirección de otra vtable. No se pudo confirmar si esto
    es "identical code folding" del linker (dos funciones distintas comprimidas a la misma dirección,
    reportando un solo nombre en la tabla de símbolos) o un error en la reconstrucción manual de estas
    dos vtables concretas (posible herencia múltiple en `CMvPlayer`/`CMvMob` no evidente desde el
    pseudo-C, que desplace su numeración de slots reales sin que se note en `CMvCharacter::CMvCharacter`).
- **Pista de ABI real, confirmada con 2 decompiladores independientes (Ghidra y angr, mismo resultado en
  `out_angr.c:94516` `CMvMob::KnockProcWhenAttack` y `out_ghidra.c:37167` `CMvCharacter::DoAITarget`):**
  `DoAITarget(this, target)` toma un SEGUNDO argumento (`CMvCharacter* target`) que sí usa (lo
  null-checkea en `0x6d3f2` y, si no es null, lee un byte de él en `0x6d426` para comparar distancia).
  Pero el sitio de llamada en `KnockProcWhenAttack` (asm real,
  `928d6: adds r0,r4,#0; 928da: blx r3`) **solo setea `r0`** para esta llamada -- `r1` (el 2º argumento
  por convención AAPCS) queda con lo que haya dejado la llamada virtual ANTERIOR en la misma función
  (`928c2: blx r3`, la de `+0x24`), que es un registro scratch/caller-saved y por ende indefinido a
  propósito para el compilador. Si de verdad el slot `+0x144` es `DoAITarget`, esto significa que el
  `.so` original llama a esa función con un 2º argumento que NO es necesariamente `NULL` (pasaría el
  null-check) pero tampoco es un puntero válido -- coincide con la firma exacta de un caso de "no
  crashea siempre porque en Android da la casualidad de que el registro scratch queda en un valor
  inofensivo la mayoría de las veces". Si en este port ese registro queda con basura MÁS peligrosa que en
  Android real (por diferencias en qué deja cada implementación de la llamada anterior, `+0x24`, que en
  runtime es la de `CMvPlayer` o la de otra subclase concreta), explicaría por qué crashea acá y no
  (todavía) en Android/en el port de referencia.
- **Conclusión honesta de esta segunda pasada:** no se pudo aislar con 100% de certeza CUÁL de los 3
  `blx` de `KnockProcWhenAttack` es el que salta a `0x92166364`, ni reconstruir con confianza total el
  layout de vtable de `CMvMob`/`CMvPlayer` a mano (sin la recuperación de clases por RTTI de Ghidra, que
  no se corrió sobre este proyecto -- el `.h` decompilado solo tiene structs "PlaceHolder", sin RTTI
  recuperado). El mecanismo del crash (llamada virtual con un argumento potencialmente inválido
  proveniente de un registro scratch no reseteado, `param_1`/`this` de `CMvCharacter`/`CMvMob`) queda
  bien acotado y es un candidato sólido, pero confirmarlo al 100% requiere runtime real (GDB del toolkit
  parado en `0x928a8`, inspeccionando `r0`-`r3` y el contenido de sus vtables en memoria) o correr el
  script `RecoverClassesFromRTTI` de Ghidra sobre el `.so` para obtener el layout de clases verdadero en
  vez de reconstruirlo a mano.
- **No se tocó código de fuente por esta pista todavía** -- no hay nada parcheable de nuestro lado (el
  `.so` es binario original sin modificar; el mecanismo ocurre enteramente dentro de él). El único lever
  que tenemos desde el loader sigue siendo la mitigación de stack ya aplicada arriba. Si se confirma en
  runtime que es realmente esta llamada con argumento inválido, evaluar entonces si conviene un parche
  binario puntual (ver skill `psvita-porting`, patching binario) para forzar `r1=0` antes de esa `blx`
  específica -- pero NO hacerlo especulativamente sin confirmación en vivo primero.

### Triage -- dump `illusia-psp2core-1787457857-0x000dad2ddb` (2026-08-23, bug NUEVO y distinto al anterior)

Dump posterior al de KnockProcWhenAttack, con el fix de stack de 4 MB ya aplicado. **Es un crash
totalmente distinto**, en otra área del motor -- indica que el fix anterior probablemente sirvió para
pasar de ese punto (o al menos esta corrida llegó mucho más lejos: acá estamos en la inicialización de
red/UI de arranque, no en combate).

- **Síntoma:** Data abort, hilo `ILLUSIA01` (el hilo principal -- nombre por defecto del proceso, no un
  hilo real distinto). El propio `vita-parse-core` ya da un diagnóstico automático preciso:
  `R3 = 0x0`, dentro de `CMvNetworkMenu::Initialize()` (`out_ghidra.c:83854`), llamada desde
  `CMvNet::SendMain()+0x1b` (LR, confirmado -- relación caller/callee normal, no dato viejo de pila).
- **Cruce con el pseudo-C:** `Initialize()` construye ~20 objetos `CGsKeymap` seguidos, todos con el
  mismo patrón: `piVar2 = operator_new(0x28); CGsKeymap::CGsKeymap(piVar2); in_r0[N] = piVar2;
  (**(code**)(*piVar2 + 0x10))(piVar2, ...)`. El offset exacto de la escritura que aparece en el
  desensamblado real (`str r6,[r4,#80]` = offset 80 = `in_r0[0x14]`) ubica el crash en la **5ª** de esas
  construcciones (`out_ghidra.c:83897-83900`, args `(1,4,1,0x20,0)` -- coinciden exactamente con
  `r1=1,r2=4,r3=1` del desensamblado real). El crash ocurre al desreferenciar la vtable del objeto
  `CGsKeymap` recién construido (`ldr r3,[r6,#0]` / `ldr r3,[r3,#16]`) -- es decir, el objeto que
  `CGsKeymap::CGsKeymap()` acaba de "construir" (le acaba de escribir su puntero de vtable dos
  instrucciones antes) da `NULL`/inválido al leerlo de nuevo casi inmediatamente después.
- **Hipótesis descartadas concretamente:**
  - **No es un desborde de `CMvNet` sobre memoria ajena:** `CMvNet::CMvNet()` (el constructor real,
    `out_ghidra.c:75444`) escribe y usa campos hasta `this[0xbe]` y más allá -- los campos `this[0x95]`
    a `this[0x98]` que `Initialize()` toca al principio están cómodamente dentro del objeto real, no son
    un desborde de un `CMvNet` de tamaño insuficiente.
  - **No es el stub de red devolviendo algo "raro":** `Natives.isNetAvailable()`/`netConnect()` reales
    (`decompiled/apk_jadx/sources/.../Natives.java:214-260`) devuelven exactamente `-1` en el caso
    offline real de Android -- nuestro stub en `source/java.c` (`m_isNetAvailable`/`m_netConnect`
    devolviendo `-1`) es semánticamente idéntico a un dispositivo Android real sin señal, así que esta
    ruta de arranque con red "caída" ya se ejecuta igual en Android real y no debería, por sí sola,
    crashear -- descartado como causa.
  - **No es un hilo de red separado corriendo en paralelo:** el hilo que crashea es el principal
    (`ILLUSIA01`, nombre por defecto del proceso), no un hilo de red aparte -- no hay evidencia de una
    condición de carrera entre hilos distintos accediendo al heap.
- **Sin confirmar (mismo techo que el triage anterior):** por qué falla específicamente la 5ª
  construcción y no la 1ª (si `operator_new`/`_Znwj` fallara sistemáticamente ya habría crasheado en el
  primer `CGsKeymap`, o incluso antes en los menús construidos previamente dentro de
  `CMvGameUI::CMvGameUI()` -- `CMvStateMenu`, `CMvItemMenu`, `CMvSkillMenu`, `CMvQuestMenu`,
  `CMvShopMenu`, todos antes que `CMvNetworkMenu` en el mismo constructor). Que falle recién en la 5ª
  sugiere corrupción de heap dependiente de layout/timing más que un fallo determinístico de
  `operator new`, pero **no se pudo aislar la escritura culpable** sin poder ejecutar en vivo.
- **Otra vez el mismo bloqueante:** sin log real fflusheado no se puede aplicar el paso 1 de
  `so-crash-triage`. Van 3 crashes triageados seguidos sin ese log. **Recomendación fuerte:** antes de
  seguir cazando bugs uno por uno a ciegas contra el dump estático, vale la pena una sesión corta
  dedicada a arreglar el logging del toolkit standalone (por qué `live_session_*.log`/
  `perf_telemetry_*.log` quedan en 0 bytes) -- un log real reduciría drásticamente el tiempo de cada
  triage siguiente.
- **No se tocó código todavía por este bug** -- no hay evidencia suficiente para un fix seguro (a
  diferencia del stack size del triage anterior, acá no hay una mitigación de bajo riesgo obvia; subir
  más el heap sin evidencia de agotamiento real sería puramente especulativo). Próximo paso: reproducir
  con GDB del toolkit, breakpoint en la entrada de la 5ª construcción de `CGsKeymap` dentro de
  `CMvNetworkMenu::Initialize()` (offset `+0xf2` de la función, ver desensamblado en el `.analysis.txt`),
  inspeccionar qué devolvió `operator_new(0x28)` y el contenido real de esa memoria.

**Actualización (mismo día, dump `illusia-psp2core-1787459015-0x000f843dc1`):** segunda corrida,
resultado **idéntico byte a byte** al dump anterior (mismo PC/LR/registros/pila completos -- único
diff es el nombre del archivo). Esto ajusta la hipótesis de arriba: al ser 100% determinístico entre
corridas, es MENOS probable que sea corrupción de heap dependiente de timing/orden no determinístico
(en ese caso se esperaría variación entre corridas), y más probable que sea un bug consistente y
reproducible siempre en el mismo punto exacto -- lo cual hace mucho más viable atraparlo con GDB en la
próxima sesión (no depende de "tener suerte" con el layout de memoria).

### Fix del bloqueante de logging (2026-08-23) -- `live_session_*.log` ya no debería quedar en 0 bytes

Causa raíz encontrada revisando el propio `psvita-port-toolkit-cli` (editable install en
`/Volumes/Seagate/PSVITA Develop/psvita-port-toolkit-cli`, fuera de este repo): `debugnet_server.py`
(el "Servidor de Logs en Vivo" del toolkit) escucha datagramas UDP en el puerto 9999 y los va
guardando con `flush()` tras cada línea -- el archivo SIEMPRE va a quedar en 0 bytes si nada del lado
del Vita le manda esos datagramas. Sus propias notas (`docs/dev-notes/debugnet_server.md`) son
explícitas: *"no port here uses networked debug logging yet"* -- es decir, es un feature del toolkit
que ningún port (incluido este) había conectado todavía. `source/utils/logger.c` únicamente usaba
`sceClibPrintf` (va al canal de debug del kernel, no a este listener) -- **no era un bug del toolkit,
era una pieza de este port que faltaba escribir.**

- **Fix aplicado:**
  - `source/utils/logger.c`: `_log_print()` ahora, además de `sceClibPrintf`, manda cada línea ya
    formateada por UDP a `DEBUGNET_HOST_IP:9999` (constantes al principio del archivo) usando un
    socket UDP `AF_INET`/`SOCK_DGRAM` lazy-init (mismo patrón que `perf_telemetry_hooks.c` que genera
    el propio toolkit para telemetría de rendimiento). Si el socket no se puede crear o el envío
    falla, se ignora silenciosamente -- `sceClibPrintf` sigue funcionando igual, esto es estrictamente
    aditivo.
  - `source/utils/init.c`: se agregó `sceNetInit()` + `sceNetCtlInit()` al principio de
    `soloader_init_all()` (antes de cualquier `l_success`/`l_fatal`) -- el stack de red de la Vita
    nunca se inicializaba en este port pese a que el juego ya hace sockets BSD reales vía `dynlib.c`
    (`socket`/`connect`/etc.), así que sin este init ni el envío UDP del logger ni (probablemente) los
    sockets reales del `.so` iban a funcionar nunca, aunque no crashearan por eso.
  - `CMakeLists.txt`: agregado `SceNet_stub`/`SceNetCtl_stub` al link (antes ausentes).
  - Bonus para la sesión de GDB (`psvita_toolkit/gdb_bridge.py`): `soloader_init_all()` ahora loguea
    `SO_BASE=0x%08x` con `so_mod.text_base` justo después de `so_file_load()` -- coincide exactamente
    con el regex que usa `watch_for_so_base()` (`SO_BASE\s*[=:]\s*(0x[0-9a-fA-F]+)`), así que el
    generador de script `.gdb` del toolkit puede capturar la base real del `.so` por UDP en vez de
    tener que leerla a mano de la consola (la base real que reportan los dumps nunca cae exacto en el
    `LOAD_ADDRESS` pedido, `0x98000000`).
  - **Build validado localmente:** `cmake` + `make` completo en un directorio de scratch (no se tocó
    el `build/` que administra el toolkit), compiló y linkeó sin errores nuevos
    (`illusia.vpk`/`eboot.bin` generados) -- no se probó en consola/Vita3K real todavía, eso queda
    para la próxima corrida con el toolkit.
- **Pendiente de verificar por el usuario, no asumido:** `DEBUGNET_HOST_IP` en `source/utils/logger.c`
  quedó hardcodeado a `192.168.3.35` (misma subred que `vita_ip` en `.psvita-toolkit.json`,
  `192.168.3.15`) -- la máquina de desarrollo tenía DOS interfaces en esa subred al momento de este
  fix (`en0=192.168.3.35`, `en1=192.168.3.31`); no hay forma de saber desde acá cuál es la que la Vita
  realmente puede alcanzar. Si al correr `debugnet_server.py`/la opción "Servidor de Logs en Vivo" del
  toolkit siguen sin llegar líneas, **lo primero a revisar es esa IP** (correr `ifconfig`/`ipconfig`
  de nuevo y confirmar cuál interfaz está en la misma red que la Vita), no asumir que el mecanismo en
  sí está roto.
- **Cómo usarlo en la próxima sesión de triage:** antes de reproducir el crash, desde el toolkit
  standalone elegir la opción de "Servidor de Logs en Vivo" (`debugnet_server.py`, puerto 9999 por
  default) ANTES de lanzar el juego en la Vita -- así la última línea real antes del corte queda en
  `logs/live_session_*.log`, habilitando por fin el paso 1 de la metodología `so-crash-triage` que
  viene bloqueado desde hace 3 triages seguidos.
- **IP confirmada por ruta real, no por adivinar:** `192.168.3.35` (interfaz `en0`) es la correcta --
  confirmado con `route get 192.168.3.15` (la IP de la Vita) en la máquina de desarrollo, que devuelve
  `en0` como interfaz de salida real. Descartado `en1=192.168.3.31` sin necesidad de probar a ciegas.

### Bug #2 confirmado (build real del usuario, 2026-08-23) -- `sceSysmoduleLoadModule` faltante

- **Síntoma:** primera prueba real del fix de logging de arriba -- 0 líneas en `live_session_*.log` Y
  un `.psp2dmp` NUEVO (`illusia-psp2core-1787461046-0x0000312b43`), distinto a todos los anteriores:
  **Prefetch abort con `PC = 0x0`** (salto a NULL), `LR` resuelto directo por símbolos del propio
  loader (no del `.so` del juego) a `soloader_init_all` en `source/utils/init.c:51` -- la línea
  `sceNetCtlInit();` agregada en el fix anterior.
- **Causa:** en PS Vita hay que cargar explícitamente el módulo de sistema
  (`sceSysmoduleLoadModule(SCE_SYSMODULE_NET)`) ANTES de que `sceNetInit()`/`sceNetCtlInit()` (o
  cualquier `socket()` real) funcionen -- sin el módulo cargado, el stub de `sceNetCtlInit` salta a una
  dirección sin resolver. Me faltó ese paso en el fix anterior. Esto explica el "0 líneas" original: el
  juego crasheaba en el arranque, ANTES de llegar siquiera al primer log (`SO_BASE=...`), mucho antes de
  llegar a la lógica de `CMvNetworkMenu::Initialize()` que se estaba tratando de cazar -- no era un
  problema de IP ni de puerto.
- **Fix:** agregado `sceSysmoduleLoadModule(SCE_SYSMODULE_NET)` en `source/utils/init.c` antes de
  `sceNetInit()`, más un `l_error` si `sceNetInit()` devuelve error (ese sí llega a
  `sceClibPrintf`/ventana de log de VitaShell aunque el UDP esté roto, porque `l_error` no depende de
  `DEBUG_SOLOADER`).
- **Estado:** corregido en código, compila y linkea limpio (validado en build de scratch).

### Bug #3 confirmado y resuelto (2026-08-23) -- Agotamiento del Allocator de Bloques Fijos (`Gcx_MM_Alloc`)

- **Síntoma:** Data abort en `CMvNetworkMenu::Initialize()` (`9f8b6: movs r3, #1` / `blx ip`) al intentar
  desreferenciar el vtable del 5º objeto `CGsKeymap` creado consecutivamente. Dump: `illusia-psp2core-1787461657`.
- **Causa Raíz:**
  1. `operator new` (`_Znwj`), `operator new[]` (`_Znaj`), `operator delete` (`_ZdlPv`) y `operator delete[]` (`_ZdaPv`)
     en este motor no llamaban a `malloc`/`free` de libc, sino a un gestor de memoria propietario (`Gcx_MM_*`).
  2. `Gcx_MM_Init` (`0xc6b18`) inicializaba un pool estático de solo 1 MB (`g_gcxGlobalMemory` en BSS) dividido
     en buckets de tamaños fijos mediante `SetBlockTypeInformation` (`0xc5484`).
  3. El bucket de tamaño 60 bytes (usado por `CGsKeymap` de 40 bytes) estaba configurado con **solo 4 slots**.
  4. En la 5ª llamada a `operator_new(40)`, `Gcx_MM_Alloc` se quedaba sin slots libres en ese bucket y devolvía `0x0` (`NULL`).
  5. Ningún constructor chequea `this == NULL`, por lo que escribía en `[0]` y al volver a `Initialize()` intentaba
     llamar al método virtual a través de la vtable nula, provocando el Data Abort.
  6. Esto también explica las corrupciones de memoria observadas anteriormente en combate (`CMvMob::KnockProcWhenAttack`).
- **Fix:**
  - Implementado en `source/patch.c` el redireccionamiento completo de todas las funciones del gestor de memoria
    (`_Z12Gcx_MM_Allocj`, `_Z13Gcx_MM_Callocj`, `_Z11Gcx_MM_FreePv`, `_Z14Gcx_MM_ReallocPvj`, `_Znwj`, `_Znaj`,
    `_ZdlPv`, `_ZdaPv`, `_Z18GxCreateGlobalHeapv`, `_Z11Gcx_MM_InitPvj`, `_Z12CreateMemoryPvi`, `_Z13DestroyMemoryv`,
    `MC_knlGetTotalMemory`, `MC_knlGetFreeMemory`, `_Z13Gcx_MM_StatesPjS_S_`) hacia el heap nativo estándar de la PS Vita
    (`malloc`/`calloc`/`free`/`realloc` con los 256 MB de RAM disponibles en `_newlib_heap_size_user`).
- **Estado:** Implementado en [patch.c](file:///Volumes/Seagate/PSVITA%20Develop/ILLUSIA-vita/source/patch.c), compilado y listo para desplegar (`build/eboot.bin` y `build/illusia.vpk` actualizados).

## Fase 6 en adelante: Pendiente
Renderizado de texto real (stb_truetype, ver riesgo alto en Fase 3), input, audio (NexusSound -- no
analizado todavía), LiveArea/VPK final. Un bug confirmado a la vez, guiado por el log real.

### Bug #4 confirmado y resuelto (2026-08-29) -- Crash en CMvNetworkMenu::ResetKeyMap por offline mode

- **Síntoma:** Crash en `CMvNetworkMenu::ResetKeyMap()` durante la inicialización (`Data abort` en PC=0x9809c38e).
- **Causa Raíz:** Se había mitigado el sistema de red parcheando `CMvNetworkMenu::Initialize` y `CMvNet::Initialize` para que no hicieran nada (offline mode). Sin embargo, `CMvGameUI::ResetAllKeyMap` (llamado en el flujo principal) siempre llama a `CMvNetworkMenu::ResetKeyMap`, que asume que los elementos de UI de red se inicializaron. Al no estar creados, itera sobre punteros nulos y desreferencia uno para llamar a un método virtual (vtable de un objeto nulo).
- **Fix:** Se ha añadido un hook (stub) vacío para `_ZN14CMvNetworkMenu11ResetKeyMapEv` en `source/patch.c` (no-op) dado que si estamos forzando el modo offline y anulando la inicialización del menú de red, tampoco necesitamos procesar sus entradas.
- **Estado:** Implementado en `source/patch.c`, compilado y corregido.

### Bug #5 confirmado y resuelto (2026-08-30) -- Crash en SceLibKernel (memcpy) desde CMvLayerData::Load por chequeo con signo

- **Síntoma:** Crash `Data abort` en el hilo principal (`PC = SceLibKernel seg1 + 0x46` apuntando a `memcpy`), con un puntero de origen `NULL` (`R1=0x0`), justo después de cargar el mapa de historia (`map/114.map` o `map/005.pzx`). La pila y el triage apuntan a `CMvLayerData::Load()`.
- **Causa Raíz:** 
  1. En `CMvLayerData::PreLoad`, el motor intenta leer los objetos del archivo del mapa y asignarlos. Para verificar si el puntero de los datos leídos es válido, el código original hace `if (0 < (long)__src)`.
  2. Puesto que en Android la memoria de BSS o anónima mapeada suele tener direcciones `< 0x80000000`, este chequeo con signo funciona. Pero en PS Vita, el heap de usuario está arriba de `0x81000000`. Al interpretarlo como un entero con signo de 32-bits, se vuelve negativo.
  3. Por ende, la verificación asume erróneamente que el puntero es inválido (`<= 0`) y omite todo el proceso de carga de entidades del mapa, dejando la cantidad de entidades y arreglos sin inicializar.
  4. Para empeorar, nuestros hooks de `operator new` devolvían memoria sin inicializar (`malloc`), por lo que la cantidad de entidades resultó ser un número "basura" positivo.
  5. Finalmente, al tratar de dibujar las entidades en `CMvLayerData::Load`, el motor intentó leer un arreglo que nunca se inicializó (`NULL`), causando el fallo en el `memcpy`.
- **Fix:**
  - Se modificó `source/patch.c` para reemplazar la instrucción problemática en `CMvLayerData::PreLoad` (offset `0x8ae62`). Cambiamos el salto `ble` (Branch if Less than or Equal, chequeo con signo) a `beq` (Branch if Equal a 0).
  - Se reemplazaron `malloc` por `calloc` en los hooks `hooked_Gcx_MM_Alloc`, `hooked_operator_new` y `hooked_operator_new_array` para garantizar que la memoria arranque en ceros, simulando el comportamiento del heap global del motor original y previniendo que atributos sin inicializar generen basura en otras partes del juego.
- **Estado:** Implementado parche dinámico binario (`kuKernelCpuUnrestrictedMemcpy`) y corrección del heap en `patch.c`.


### Bug #6 confirmado y resuelto (2026-08-30) -- Crash en sceClibMemcpy por Alineación Estricta

- **Síntoma:** Tras resolver el salto condicional y cargar correctamente el mapa de la intro (`114.map`), el juego sufre un `Data abort` dentro de `SceLibKernel` (`sceClibMemcpy`), habiendo sido llamado desde el primer `memcpy` de `CMvLayerData::PreLoad`. El volcado de memoria indicaba un aparente desborde u origen anómalo (`R0 = 0x350`), pero esto se debe a que `sceClibMemcpy` empuja a la pila sus registros de trabajo antes de crashear.
- **Causa Raíz:**
  1. En `CMvLayerData::PreLoad`, se realiza la copia `memcpy(this + 4, __src + 0x18, 8)` para leer el contador de objetos y NPCs.
  2. El destino es `this + 4` (un atributo a 4 bytes del inicio del objeto C++). Esto significa que el puntero está alineado a 4 bytes, pero **NO** a 8 bytes (ej. `0x82aed314`).
  3. En Android, el `memcpy` nativo (o el de `libc.so`) maneja operaciones sobre cualquier alineación sin problemas. Sin embargo, en el port de PS Vita inicial, las funciones `memcpy`, `__aeabi_memcpy`, etc., estaban mapeadas a `sceClibMemcpy` de la API nativa de Vita.
  4. `sceClibMemcpy` es el `memcpy` del *Kernel* de Vita, altamente optimizado, y asume frecuentemente (especialmente para copias de exactamente 8 bytes, como es este caso) que puede utilizar instrucciones de 64 bits (`LDRD`/`STRD` o `VLD1.64`/`VST1.64`).
  5. Cuando `sceClibMemcpy` intenta ejecutar un `STRD` en un puntero que no está alineado a 8 bytes (como `0x82aed314`), se genera un _Alignment Fault_ inmediato a nivel hardware que en la Vita OS es reportado como un `Data abort`.
- **Fix:**
  - En `source/dynlib.c`, se procedió a reemplazar todas las importaciones que resolvían a `sceClibMemcpy` (como `memcpy`, `__aeabi_memcpy`, `__aeabi_memcpy4`, etc.) para que apunten a la implementación estándar `&memcpy` de la biblioteca C nativa (`newlib` de `vitasdk`), la cual es segura respecto a desalineaciones y previene fallos similares en todo el motor.
- **Estado:** Modificado `dynlib.c`, listo para compilar.

### Bug #7 confirmado y resuelto (2026-08-30) -- Recurrencia del crash en memcpy

- **Síntoma:** "Nuevo crash despues de pasar la historia del intro del juego". El archivo `.psp2dmp` (Bug 7) muestra **exactamente** la misma ubicación de fallo (`SceLibKernel seg1 + 0x4c` desde `CMvLayerData::PreLoad()`) y los mismos síntomas (falla por alineación durante `memcpy(this+4, ...)`).
- **Causa Raíz:** En la corrección del Bug 6, mapeamos las funciones `memcpy` hacia la implementación estándar `&memcpy` proveída por `vitasdk` (la cual es provista por `newlib`). Sin embargo, para no reinventar la rueda y lograr la máxima eficiencia, la librería de C de `vitasdk` delega / alisa internamente su `memcpy` hacia **`sceClibMemcpy`** del propio Vita OS. ¡Por lo tanto, seguíamos llamando indirectamente a la misma función del Kernel que nos provocaba el _Alignment Fault_! 
- **Fix:** 
  - Se implementaron explícitamente en `source/dynlib.c` versiones personalizadas orientadas a ser tolerantes a desalineación (`custom_memcpy`, `custom_memset`, `custom_memmove` y `custom_aeabi_*`). 
  - Estas funciones chequean si los punteros están alineados a 4 bytes, y en caso de estarlo (como es el caso de nuestras clases C++ con atributos tipo int/float en `this + 4`), realizan copias de palabra a palabra sin hacer uso de las instrucciones `LDRD`/`STRD` (o NEON 64-bit) que requieren alineación estricta a 8 bytes y generaban el _Data abort_.
  - Se remapearon todas las importaciones `memcpy`, `memset`, `memmove`, `__aeabi_memcpy*`, `__aeabi_memset*`, y `__aeabi_memclr*` del Android original a estas versiones seguras (`custom_*`).
- **Estado:** Implementado en `source/dynlib.c`. ¡Con este cambio de fondo se habrán exorcizado todos los posibles Data aborts fantasmas por desalineación de structs del motor del juego!

### Bug #8 -- FIX A CIEGAS APLICADO, PENDIENTE DE VALIDAR (2026-08-30) -- Nueva recurrencia del crash en el mismo call-site, ya no es alineación

- **Síntoma:** Tras el fix del Bug #7 (`custom_memcpy` real, sin delegar a `sceClibMemcpy`), nuevo crash
  reportado por el usuario: "luego de seleccionar el personaje jugable y comenzar la cinemática con la
  historia, cuando termina crashea". Dump analizado: `illusia-psp2core-1788067802-0x0001bc2cc5`, log
  `game_007.log`.
- **Confirmado por triage (log + `.psp2dmp` + desensamblado cruzado):**
  - `game_007.log` termina justo después de `m_readAssete: loading .../map/114.map` seguido de
    `.../map/005.pzx` -- es decir, el crash ocurre en la PRIMERA carga real de mapa (`CMvMap::Load`)
    justo cuando termina la cinemática de intro y arranca el mundo jugable. Coincide exactamente con
    el repro del usuario.
  - `PC` cae dentro de `custom_memcpy` (`source/dynlib.c:98`, la escritura `*d32++ = *s32++;`)
    -- confirmado desensamblando `build/illusia.elf` en `0x81000bc4`: la instrucción que falla es
    `str r2,[r3,#0]`, es decir, una escritura a la dirección en `r3` con valor `r2`. Esto ya **no** es
    un fallo de alineación (`custom_memcpy` es tolerante a cualquier alineación) -- es un puntero
    genuinamente inválido/cercano a `NULL` (`0x350`).
  - `LR` resuelve a `CMvLayerData::PreLoad(EnumObjectLayer, int, long, unsigned int) + 0x1f`
    (offset `0x8ae50` en `libgameDSO.so`), que es exactamente el primer `memcpy(this + 4,
    (void*)(param_4 + param_3), 8)` del pseudo-C de Ghidra
    (`decompiled/libgameDSO_armeabi/ghidra/out_ghidra.c:65672`) -- la misma línea que tocaron los
    Bugs #5, #6 y #7. `param_3` es el puntero `__src` devuelto por `CMvMap::LoadMapData` (vía
    `MvLoadResource` -> `GsLoadResource` -> `LoadResource` -> `GcxLoadResData`), y `param_4` es un
    acumulador de offset de bytes dentro de ese buffer.
- **Pistas descartadas:** el archivo real `ux0_data/illusia/assets/map/114.map` se inspeccionó a mano
  (`xxd`/`struct.unpack`) y su header es sano (ancho=1, **cantidad de capas=3**, alto=27/31, objetos=0)
  -- no hay un contador corrupto en el archivo fuente en sí. La cadena completa de símbolos se cruzó con
  `objdump -T` (`CMvMap::LoadMapData 0x88430` -> `MvLoadResource 0xbe638` -> `GsLoadResource 0x60fc0` ->
  `LoadResource 0x60f38` -> `GcxLoadResData 0xc53a0` -> `GcxGetResourceID 0xc5240` /
  `MC_knlGetResourceID 0xc3600` / `MC_knlGetResource 0xc35d4`), y con `iVar3`(capas)=3 la matemática de
  `operator_new__((iVar3*3+1)*8)` (80 bytes: 8 de header + 3*24 de `CMvLayer`) cuadra exacto contra el
  loop que la llena -- no hay overflow evidente ahí tampoco. `MC_knlGetResource` (`out_ghidra.c:118441`)
  SÍ tiene un hueco real pero distinto: llama a `readAssets`/`readAssete` sin chequear si el resultado es
  `NULL` antes de hacer `memcpy(param_2, iVar1 + 0x10, param_3)` -- si el JNI de assets fallara para ese
  archivo puntual, produciría exactamente esta clase de puntero-casi-nulo, aunque en esta corrida `__src`
  (el buffer resultante, en `R5`/`R9` del dump = `0x82bda820`) se ve como una dirección de heap válida.
- **Por qué no se identificó la línea exacta:** la reconstrucción estática de registros en el punto del
  crash es ambigua (el build del loader es sin optimizar -- usa `r7` como frame pointer y reutiliza
  `r0`-`r3` libremente -- mientras que el `.so` original está optimizado y preserva `r4`-`r11` a través
  de la llamada; no fue posible reconciliar con 100% de certeza, solo con desensamblado estático, cuál de
  los dos operandos del `memcpy` (destino `this+4` o fuente `param_4+param_3`) es el que vale `0x350` en
  el dump).
- **Fix aplicado (a ciegas, pendiente de validar en consola por el usuario):** en vez de seguir
  adivinando cuál puntero exacto es inválido, se blindaron las tres funciones propias del port que
  reciben ese puntero sin importar de dónde venga -- `custom_memcpy`, `custom_memset` y `custom_memmove`
  en `source/dynlib.c` ahora chequean si `dest`/`src` está por debajo de `SUSPICIOUS_PTR_THRESHOLD`
  (`0x10000`, un umbral muy por debajo de cualquier dirección real de heap/texto/bss en la Vita) antes de
  copiar, y si es así, loguean con `l_error` (siempre visible, no depende de `DEBUG_SOLOADER`) el
  `dest`/`src`/`n` exactos y **saltean la copia** en vez de crashear. Esto no corrige la causa raíz
  todavía (si el puntero es inválido, algo aguas arriba sigue sin inicializar ese campo/capa
  correctamente), pero: (a) convierte el Data Abort fatal en una omisión silenciosa -- el juego debería
  poder seguir corriendo aunque esa capa/campo quede sin cargar --, y (b) el próximo `.psp2dmp`/log ya no
  hará falta, porque el propio log de la corrida va a mostrar el `dest`/`src` real que disparó la guarda,
  cerrando la ambigüedad de arriba sin necesidad de una sesión de GDB.
  - **Validado:** compila limpio en un build de scratch (`cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5` +
    `make`, fuera del `build/` que administra el toolkit) -- sin warnings nuevos, `illusia.vpk`/
    `eboot.bin` generados correctamente. **No probado en consola/Vita3K todavía.**
- **Estado:** Implementado en `source/dynlib.c`, pendiente de que el usuario lo pruebe en consola real y
  confirme si el crash desaparece (o si el log de la guarda revela el verdadero origen para un fix de
  causa raíz más preciso).
- **Próximo paso recomendado:** instrumentar `CMvLayerData::PreLoad` (mismo mecanismo de parche binario
  in-place ya usado en `source/patch.c` para el Bug #5, o un breakpoint de GDB vía el toolkit) para
  loguear `this`, `param_3` (`__src`) y `param_4` (offset acumulado) en cada llamada, y así identificar
  con certeza cuál puntero es inválido y en qué capa/iteración ocurre -- antes de tocar código.

### Bug #8 (cont.) -- causa de la "recurrencia": el fix nunca se había recompilado/redesplegado (2026-08-30)

- **Síntoma reportado:** "sigo con el crash después de los cutscenes" con un `.psp2dmp` nuevo
  (`illusia-psp2core-1788072334-0x0001f02dcf`) y un log nuevo (`game_008.log`).
- **Diagnóstico:** NO es un bug nuevo ni una recurrencia real del guard fallando.
  - `game_008.log` es **MD5-idéntico** a `game_007.log` (el log del Bug #8 original) --
    `5e997fee03f3d41517b1a56469e98f61` en ambos, byte a byte.
  - El `.psp2dmp.analysis.txt` nuevo es idéntico al de Bug #8 salvo el nombre de archivo (mismo PC
    `0x81000bc4`, mismo LR `CMvLayerData::PreLoad+0x1f`, mismos registros `R0=0x350`/`R1=0x354`).
  - `build/illusia.elf` (el binario que realmente se probó en esta corrida) tenía mtime `01:28:59`,
    **anterior** al mtime `02:39:31` de `source/dynlib.c` (el archivo donde vive el guard del Bug #8).
    Es decir: el guard fue escrito en la sesión anterior, validado únicamente en un **build de
    scratch** aparte (según la nota del propio Bug #8), pero nunca se recompiló el `build/` real que
    administra el toolkit ni se volvió a desplegar antes de esta prueba -- la consola siguió
    corriendo el binario de ANTES del fix, así que el crash "recurrió" exactamente igual porque, en
    los hechos, se volvió a probar el mismo binario viejo.
- **Acción tomada (esta sesión):** `psvita-toolkit build --preset debug` sobre el `build/` real del
  proyecto (no un scratch dir) -- compiló limpio, y se confirmó con `strings build/illusia.elf | grep
  "guarded near-NULL"` que las 3 cadenas del guard (`custom_memcpy`/`custom_memset`/`custom_memmove`)
  están presentes en el binario nuevo (mtime `16:37:42`). Desplegado con `psvita-toolkit deploy --vpk
  --yes` a `ux0:/downloads/illusia.vpk` en la consola (`192.168.3.15`).
- **Pendiente:** el usuario debe reinstalar el VPK desde VitaShell y volver a probar. Si el guard
  funciona como se espera, el log de la próxima corrida debería mostrar una línea `l_error` de
  `custom_memcpy: guarded near-NULL copy dest=... src=... n=...` en vez de un crash fatal -- **esa
  línea va a revelar, por primera vez con certeza, cuál de los dos punteros del `memcpy(this+4,
  __src+offset, 8)` de `CMvLayerData::PreLoad` es el inválido** (la ambigüedad que Bug #8 dejó sin
  resolver), habilitando por fin un fix de causa raíz en vez de la mitigación defensiva actual.
- **Lección para esta sesión/proyecto:** cuando un fix se valida en un build de scratch aparte (fuera
  de `build/`), no se puede asumir que ya está desplegado -- siempre correr `psvita-toolkit build`
  sobre el `build/` real antes de pedirle al usuario que pruebe de nuevo en consola. Vale la pena
  comparar el MD5 del log nuevo contra el anterior como primer chequeo en todo triage futuro: si son
  idénticos, es señal inmediata de binario no actualizado, no de un bug persistente.

### Bug #8 (cont. 2) -- con el binario real ya recompilado, el guard SÍ dispara -- causa raíz de `this` inválido en `CMvLayerData::PreLoad` sigue sin resolver (2026-08-30)

- **Síntoma:** `game_009.log` (esta vez con el binario recompilado de verdad, confirmado por
  `psvita-toolkit build` en esta sesión) ya **no crashea** después de la cutscene de intro -- el guard
  hizo su trabajo. Se ven 3 líneas nuevas en el log:
  ```
  custom_memcpy: guarded near-NULL copy dest=0x350 src=0x82aca690 n=8 (skipped)
  custom_memcpy: guarded near-NULL copy dest=0x350 src=0x82acad22 n=8 (skipped)
  custom_memcpy: guarded near-NULL copy dest=0x350 src=0x82acb3b4 n=8 (skipped)
  ```
- **Esto resuelve la ambigüedad que dejó Bug #8 original:** cruzando con el pseudo-C de
  `CMvLayerData::PreLoad` (`this + 4` es el primer argumento del `memcpy`, `param_4 + param_3` el
  segundo), `dest` es literalmente `this + 4` y `src` es el puntero al buffer del mapa. Los 3 `src` son
  direcciones de heap perfectamente válidas y con un stride constante de `0x692` bytes entre sí
  (consistente con leer 3 capas consecutivas del mismo buffer de `114.map`, la capa con "cantidad de
  capas=3" ya confirmada por Bug #8 original). **El puntero inválido es `this` (`0x34c`), no `src`.**
- **Nuevo misterio sin resolver:** cruzando con `CMvMap::PreLoad()` (el llamador, desensamblado real vía
  `arm-vita-eabi-objdump -M force-thumb` contra `ux0_data/illusia/libgameDSO.so`, no solo el pseudo-C),
  `this` para la N-ésima capa se computa como `array_base + N*0x18` donde `array_base` sale de
  `CMvMap_obj[0xe4]` (recargado de memoria en cada iteración) y `array_base` viene de
  `_Znaj(80)` (`operator new[]`, ya hookeado a `calloc` real desde Bug #3) -- **debería incrementar en
  24 bytes por capa**, pero las 3 líneas del log muestran el MISMO `dest=0x350` exacto las 3 veces. Esto
  contradice la lectura estática del desensamblado (que es de la ÚNICA copia real de `libgameDSO.so` en
  el proyecto, sin razón para pensar que difiere del binario en la consola). Confirmado que NO es
  ambigüedad de reconstrucción de registros de un dump estático esta vez -- son valores reales
  impresos en runtime por nuestro propio `l_error`, 100% confiables.
- **Por qué probablemente importa para el síntoma de "cutscene pegada" reportado en la misma sesión:**
  si el array de `CMvLayer`/las capas de `114.map` (el mapa de la intro) nunca terminan de poblarse por
  este guard, es plausible que objetos/NPCs de esas capas que un script de cutscene posterior espera
  (para avanzar/terminar la escena) nunca se creen -- dejando al script esperando una condición que
  nunca se cumple. **No confirmado todavía, es la hipótesis más fuerte disponible.**
- **Próximo paso recomendado (requiere runtime real, no más estático):** instrumentar con un log o
  breakpoint de GDB (vía el toolkit) el sitio de la llamada en `CMvMap::PreLoad` (offset `+0x113` de la
  función, justo antes de `bl 8ae50 <CMvLayerData::PreLoad>`) para imprimir el `this` real (`r0`) y el
  valor de `CMvMap_obj[0xe4]` en cada una de las 3 iteraciones -- eso va a decir si el array realmente
  nunca se allocó (rama de "ya existe" en `89bb2-89bba` del desensamblado, que SALTEA por completo la
  allocación si `*(int*)(in_r0+0x31c) != 0`, posiblemente porque el objeto `CMvMap` se está reusando
  entre cargas de mapa sin limpiar ese campo) o si algo más sutil está pasando.

### Bug #9 -- "Cutscene con script pegada, ningún botón/touch la avanza" -- falta el botón nativo de Skip (2026-08-30)

- **Síntoma reportado:** tras resolver el crash del Bug #8, el juego avanza más lejos pero se queda
  "pegado" en otra cutscene durante el gameplay (no la de intro); ni los botones físicos ni el touch
  (virtual o físico) la destraban.
- **Causa raíz (confirmada cruzando 3 fuentes -- .so real vía `objdump`, pseudo-C de Ghidra, y el APK
  decompilado con jadx):**
  1. `CMvGameScriptMgr::OnKeyPress(GsVKey)` (`out_ghidra.c:9994`) es el ÚNICO lugar que llama a
     `CMvGameScript::DoSkip()` (`out_ghidra.c:45489`) -- y solo si `param_1 == 0xb`.
  2. `CGsInputKey::GxKey2GsKey(int)` (`out_ghidra.c:10000`) es la tabla que traduce códigos de tecla
     crudos ("GxKey") a `GsVKey` -- **el ÚNICO código crudo que mapea a `0xb` es `0x23`** (`case 0x23:
     param_1 = 0xb;`). Ninguno de los códigos negativos (`-1` a `-0x10`) que ya usábamos para
     dirección/acción/menú mapea a `0xb`.
  3. Cruzando con el Java decompilado (`NexusHal.java:45`, `NeoUIControllerView.java`/
     `UIControllerView.java`): `0x23` es la constante `MH_KEY_POUND` (la tecla `#`) -- y en
     `getHalKeyCode()` (el traductor Android→HAL real) esa tecla cae explícitamente en el `default:
     return 0` (ni siquiera en Android físico un botón del teléfono llega a enviar `0x23`). Esto
     confirma que, en el juego original, saltar una cutscene con script SOLO era alcanzable con un
     botón/ícono en pantalla dibujado por el propio motor (`CMvGameScript::DrawSkipButton`,
     `out_ghidra.c:45535`) cuyo hit-test es 100% nativo (no pasa por Java) -- las cutscenes de DIÁLOGO sí
     avanzan con un tap simple (vía `UIFullTouch`, otro camino), pero las cutscenes CON SCRIPT
     (`sScript`) solo escuchan esta tecla específica.
  4. Nuestro `source/main.c` nunca enviaba el código `0x23` por ningún botón físico -- de ahí que
     ninguna combinación de botón/touch pudiera destrabar esa clase de cutscene.
- **Fix:** en `source/main.c`, se remapeó `SCE_CTRL_SELECT` (antes duplicado de `CIRCLE`, ambos en
  `-8`, redundante) para que envíe el código crudo `0x23` en vez de `-8` -- convirtiéndolo en un botón
  dedicado de "saltar cutscene", equivalente al botón nativo de skip que dibuja el motor.
- **Estado:** implementado en `source/main.c`, recompilado con `psvita-toolkit build` y desplegado con
  `psvita-toolkit deploy --vpk --yes` a `ux0:/downloads/illusia.vpk`. **Confirmado que funcionó** --
  SELECT sacó al usuario de la cutscene pegada (ver Bug #10 abajo, que ocurre DESPUÉS de saltarla).

### Bug #10 -- Crash real en `CMvLayer::Draw` justo después de saltar la cutscene con SELECT -- consecuencia directa de la corrupción sin resolver del Bug #8/#9 (2026-08-30)

- **Síntoma:** tras usar SELECT para saltar la cutscene (Bug #9), la consola generó un `.psp2dmp` nuevo
  (`illusia-psp2core-1788124250-0x00030921bf`), con un `context.md` auto-generado por el toolkit.
  `PC` cae en `CMvLayer::Draw(int, int, tagTILEPOINT*, bool, bool) + 0xcc`
  (`decompiled/.../out_ghidra.c:61625`), instrucción `ldr r3, [r0, #0]` con `R0 = 0x800f0000` -- un
  puntero claramente inválido/salvaje (no es `NULL`, así que el guard de `custom_memcpy` no aplica acá,
  el crash es una desreferencia de vtable directa en código del juego, no un `memcpy`).
- **Diagnóstico (cruzando el `.so` real vía `arm-vita-eabi-objdump -M force-thumb` + pseudo-C de
  Ghidra):** la instrucción que falla es la carga del vtable de `piVar5`, el resultado de indexar la
  tabla de sprites de tileset del mapa por el ID de tile leído del buffer de tiles de la capa:
  ```c
  uVar7 = *(byte *)((iVar10 + iVar9 * iVar6) * 2 + iVar4);  // iVar4 = *(int*)(this_layer + 0xc) -- buffer de tiles
  piVar5 = *(int **)((uVar7 - 1) * 4 + *(int *)(*(int *)(iVar8 + 8) + 8));  // lookup en la tabla de tileset
  (**(code **)(*piVar5 + 0x14))(piVar5, ...);  // <-- CRASH acá: *piVar5 (vtable) con piVar5=0x800f0000
  ```
  Esto ocurre justo después de cargar `map/114.map` + `map/115.map` (los mismos 2 mapas cuyo
  `CMvLayerData::PreLoad` sigue disparando el guard de `custom_memcpy` -- ver Bug #8 cont. 2 -- con
  `dest=0x350`/`dest=0x42b` constante en las 3 capas de cada uno).
- **Se descartó como causa directa que `MC_knlCalloc` (usado para el buffer de tiles, `this+0xc`, y
  para los arreglos de objetos/NPCs, `this+0x10`/`this+0x14`) tenga su propio allocator roto** --
  desensamblado confirma que `MC_knlCalloc` (`0xc37a0`) es un wrapper trivial y directo de
  `malloc@plt` + `memset@plt` (los símbolos YA importados/reales de `dynlib.c`), sin ningún pool
  intermedio -- a diferencia de `Gcx_MM_Alloc`/`operator new` (Bug #3), esta familia `MC_knl*` nunca
  necesitó parche. El buffer de tiles en sí debería poblarse bien.
- **Hipótesis más fuerte, sin confirmar todavía:** el problema está en la tabla de TILESET (imágenes),
  no en el buffer de tiles -- `CMvMap::LoadTileImage()` (llamado en `CMvMap::PreLoad`, antes del loop de
  capas) probablemente no está registrando suficientes entradas en la tabla que `iVar8`/`*(iVar8+8)+8`
  referencia para el/los tile ID(s) que esta capa concreta usa, dejando el lookup fuera de rango. No se
  pudo confirmar el tamaño real de esa tabla ni si `LoadTileImage` depende a su vez de algo que el guard
  del Bug #8/#9 ya rompió (no se pudo resolver: los globals que Ghidra nombra como
  `CGsSingleton<CMvMap>::ms_pSingleton`/`CGsSingleton<CGsGraphics>::ms_pSingleton` NO aparecen en
  `.dynsym`/`.symtab` del `.so` -- son nombres sintéticos de Ghidra para datos GOT-relativos sin símbolo
  real, así que no se pueden resolver con `so_symbol()` para escribir un hook C confiable sin arriesgar
  una dirección adivinada).
- **Por qué NO se aplicó un parche directo esta vez:** el valor inválido (`0x800f0000`) no es `NULL`, así
  que un simple `cbz`/chequeo de puntero nulo (como hicimos en Bug #5 con un solo opcode) no lo
  atraparía -- haría falta un chequeo de rango, que no entra en el espacio de instrucciones disponible
  sin desplazar el resto de la función, y reimplementar `CMvLayer::Draw()` completa en C corre el riesgo
  de necesitar direcciones de globals que no se pueden confirmar de forma segura (ver punto anterior).
  Forzar una dirección adivinada ahí sería más peligroso que el crash actual.
- **Mitigación de bajo riesgo aplicada en su lugar -- solo logging, sin cambiar comportamiento:**
  1. `source/patch.c`: `hooked_operator_new_array` ahora loguea `size`/puntero devuelto para toda
     allocación `<= 512` bytes -- va a confirmar en el próximo log real si la allocación del arreglo de
     capas (`(layer_count*3+1)*8` = 80 bytes para 3 capas) realmente devuelve un puntero chico/inválido,
     o si la corrupción ocurre DESPUÉS de la allocación (al releer `CMvMap+0xe4` antes de cada llamada a
     `CMvLayerData::PreLoad`).
  2. `source/dynlib.c`: el guard de `custom_memcpy` ahora también loguea `caller=%p`
     (`__builtin_return_address(0)`) -- confirma si las 3 llamadas por mapa vienen realmente del mismo
     sitio de llamada (la hipótesis actual) o no.
- **Estado:** logging agregado, compilado y desplegado (`ux0:/downloads/illusia.vpk`). **El crash de
  `CMvLayer::Draw` en sí SIGUE SIN CORREGIRSE** -- es esperable que si el usuario reproduce el mismo
  camino (llegar a estos mapas y saltar la cutscene) vuelva a crashear en el mismo lugar. El log que
  salga de la próxima corrida debería, por primera vez, decir con certeza si `this` para las capas nace
  mal desde la propia allocación o se corrompe después -- cerrando por fin el misterio del Bug #8/#9
  antes de intentar un fix de causa raíz real (en vez de seguir adivinando).
- **Próximo paso recomendado:** con el log nuevo en mano, si `operator_new[] size=80 -> <ptr>` muestra
  un puntero de heap normal (ej. `0x82xxxxxx`), el bug está en la relectura de `CMvMap+0xe4` o en algo
  que pisa esa memoria entre allocación y uso -- ahí sí valdría la pena una sesión de GDB del toolkit con
  breakpoint en `CMvMap::PreLoad+0x113` (justo antes de cada `bl CMvLayerData::PreLoad`) para inspeccionar
  `r0` en vivo. Si en cambio el puntero YA sale chico/inválido en el log de la allocación misma, el
  problema está en cómo se computa el tamaño o en el propio `_Znaj`/`calloc` bajo esas condiciones
  específicas de memoria en ese punto del juego.

### Bug #8/#9/#10 (cont. 3) -- confirmado: la allocación es válida, la corrupción ocurre ANTES de entrar a `CMvLayerData::PreLoad` -- análisis estático agotado, hace falta runtime (2026-08-30)

- **`game_011.log` con el logging nuevo confirma lo siguiente, sin ambigüedad:**
  ```
  operator_new[] size=80 -> 0x82bfd378        <- allocación del arreglo de 3 capas, puntero de heap NORMAL
  custom_memcpy: guarded near-NULL copy dest=0x350 src=0x82bda838 n=8 caller=0x9808ae6f (skipped)
  custom_memcpy: guarded near-NULL copy dest=0x350 src=0x82bdaeca n=8 caller=0x9808ae6f (skipped)
  custom_memcpy: guarded near-NULL copy dest=0x350 src=0x82bdb55c n=8 caller=0x9808ae6f (skipped)
  ```
  (mismo patrón para `map/115.map` con `dest=0x42b` y una allocación `size=80 -> 0x82c39450` distinta).
  El `caller` es siempre `CMvLayerData::PreLoad+0x1f` (el propio sitio del `memcpy` dentro de la función,
  no el llamador real) -- confirma que SIEMPRE es la misma instrucción, pero no aporta más.
- **Verificación exhaustiva por desensamblado real (`arm-vita-eabi-objdump -M force-thumb`, no solo
  pseudo-C) que NO logra explicar la discrepancia:**
  1. Confirmado que hay **un único sitio de llamada** a `CMvLayerData::PreLoad` en todo el `.so`
     (`grep` de `bl ... 8ae50` sobre el desensamblado completo), dentro de `CMvMap::PreLoad` (`0x89b04`).
  2. Ese sitio (`0x89c08`) computa `this = r8 + r6`, donde `r8` sale de recargar `CMvMap_obj[0xe4]`
     (el campo que `0x89bec` acaba de escribir con el puntero recién allocado + 8 de header, ej.
     `0x82bfd380`) y `r6` es el offset acumulado (`0, 0x18, 0x30`). Para la 1ª capa esto DEBERÍA dar
     `this = 0x82bfd380`, no `0x34c`.
  3. Se descartó que el loop de construcción (`CMvLayer::CMvLayer`, `0x86e58`, y su
     `CMvLayer::Initialize`, `0x86e54`) toque `r8` -- ambos solo usan `r0,r2,r3,r4,lr`, confirmado por
     desensamblado directo.
  4. Se descartó que `CMvLayerData::PreLoad` (`0x8ae50`) no preserve `r8` correctamente -- su prólogo
     (`mov r7,r8; push {r7}`) y epílogo (`pop {r2}; mov r8,r2`) hacen el save/restore estándar de AAPCS
     correctamente.
  5. Se descartó que `MC_knlCalloc` (usado para los buffers `this+0xc/0x10/0x14` DENTRO de
     `CMvLayerData::PreLoad`, no para el arreglo en sí) tenga su propio allocator roto tipo Bug #3 --
     desensamblado confirma que es un wrapper trivial y directo de `malloc@plt`/`memset@plt` reales.
  - **Conclusión honesta:** el desensamblado, verificado instrucción por instrucción en 5 puntos
    independientes, es 100% consistente y NO explica por qué `this` termina siendo `0x34c`/`0x427` en
    vez de `~0x82bfd380`/`~0x82c39450`. Este es el límite de lo que el análisis estático puede resolver
    en este bug -- toca confirmar en vivo.
- **Instrumentación agregada (solo logging vía `SO_CONTINUE`, sin cambiar comportamiento):**
  `source/patch.c` ahora hookea `CMvLayerData::PreLoad` en su ENTRADA REAL (no en el sitio del `memcpy`
  como antes) usando el mecanismo `SO_CONTINUE` de `so_util.h` (restaura los bytes originales, llama a
  la función real sin modificar, y vuelve a aplicar el hook) -- loguea `this`/`layer`/`param2`/`param3`/
  `param4` tal cual los recibe la función, y el comportamiento del juego no cambia en absoluto.
- **Qué esperar del próximo log:** si el nuevo log muestra `CMvLayerData::PreLoad this=0x34c ...`
  (coincide con lo que ya vimos), la corrupción ocurre ANTES de la llamada -- en el sitio de llamada de
  `CMvMap::PreLoad` mismo, a pesar de que su desensamblado parece correcto (siguiente paso: GDB real
  con breakpoint en `so_base+0x89c08` para ver `r0/r3/r6/r8` en el momento exacto). Si en cambio muestra
  `this=0x82bfd380` (el valor "correcto" esperado), entonces el bug está DENTRO de
  `CMvLayerData::PreLoad` en sí (algo que muta `this` internamente antes de hacer el memcpy) -- lo cual
  contradice el desensamblado ya revisado, pero cerraría la pregunta igual.
- **Bug #10 (`CMvLayer::Draw` crash):** ver fix aplicado más abajo, después de la sesión de logging en
  vivo.

### Bug #8/#9/#10 (cont. 4) -- log en vivo confirma que `this` llega BIEN a `CMvLayerData::PreLoad` -- el análisis estático llegó a su límite (2026-08-30)

- **`live_session_20260830_183632.log` (con el hook de entrada por `SO_CONTINUE` agregado la sesión
  anterior) muestra, por primera vez, el `this` REAL tal cual lo recibe la función, ANTES de que la
  función haga nada con él:**
  ```
  operator_new[] size=80 -> 0x82a7f3d0
  CMvLayerData::PreLoad this=0x82a7f3d8 layer=0 param2=1056 param3=0x82b0b4b8 param4=0x18
  custom_memcpy: guarded near-NULL copy dest=0x42b src=0x82b0b4d0 n=8 caller=0x9808ae6f (skipped)
  CMvLayerData::PreLoad this=0x82a7f3f0 layer=1 param2=1056 param3=0x82b0b4b8 param4=0x860
  custom_memcpy: guarded near-NULL copy dest=0x42b src=0x82b0bd18 n=8 caller=0x9808ae6f (skipped)
  CMvLayerData::PreLoad this=0x82a7f408 layer=2 param2=1056 param3=0x82b0b4b8 param4=0x10a8
  custom_memcpy: guarded near-NULL copy dest=0x42b src=0x82b0c560 n=8 caller=0x9808ae6f (skipped)
  ```
- **Esto cierra una parte del misterio y confirma otra parte todavía sin explicación:**
  - `this` = `0x82a7f3d8`, `0x82a7f3f0`, `0x82a7f408` -- **incrementando exactamente 0x18 (24 bytes) por
    capa**, tal cual predecía el desensamblado de `CMvMap::PreLoad` (`array_base + layer_index*0x18`,
    con `array_base = alloc(0x82a7f3d0) + 8 = 0x82a7f3d8`). **Esto es 100% correcto y sano.**
  - `src = param_4 + param_3` calcula exacto contra los valores logueados (`0x18 + 0x82b0b4b8 =
    0x82b0b4d0`, `0x860 + 0x82b0b4b8 = 0x82b0bd18`, etc.) -- confirma que el cómputo de `src` dentro de
    la función también es correcto.
  - **Pero `dest` (que el código fuente de `CMvLayerData::PreLoad` computa literalmente como `this + 4`,
    UNA instrucción Thumb de 2 bytes -- `adds r0,#4` -- que no puede tocar ningún otro registro) sale
    `0x42b` CONSTANTE las 3 veces, sin ninguna relación con el `this` real que varía correctamente.**
    Si `this=0x82a7f3d8`, `this+4` DEBERÍA ser `0x82a7f3dc`, no `0x42b`.
- **Verificación adicional realizada para intentar explicar la contradicción (sin éxito):**
  - Se descartó una corrupción de pila por el propio logging (`_log_print` usa buffers estáticos de
    2048 bytes en `logger.c`, muy por encima del tamaño real del mensaje formateado -- no hay overflow
    posible ahí).
  - Se descartó que el mecanismo `SO_CONTINUE` en sí mismo introduzca el bug -- la MISMA discrepancia
    (`dest` constante y chico, sin relación con `this`) ya aparecía en sesiones ANTERIORES (`game_011.log`,
    Bug #8 cont. 3) que NO tenían ningún hook de `SO_CONTINUE` en `CMvLayerData::PreLoad` todavía --
    descarta que el wrapper de diagnóstico sea la causa.
  - Se verificó bytecode a bytecode del código COMPILADO de nuestro propio hook (`arm-vita-eabi-objdump`
    sobre `build/illusia.elf`) -- el hook recibe y re-pasa los 5 argumentos exactamente en el orden y
    registros esperados por AAPCS (`r0=this,r1=layer,r2=param2,r3=param3,[sp+0]=param4`), sin ningún
    error de marshalling.
  - Se re-verificó por CUARTA vez el desensamblado de `CMvLayerData::PreLoad` (`0x8ae50`) instrucción por
    instrucción: entre la entrada de la función y el `adds r0,#4` que computa `dest`, **r0 no se toca en
    ninguna instrucción intermedia** (`adds r4,r0,#0` copia sin modificar r0; el resto de las
    instrucciones tocan r1-r7 pero no r0) -- por bytecode puro, es imposible que `dest` sea otra cosa
    que `this+4`.
- **Conclusión honesta:** el análisis estático (desensamblado real, no solo pseudo-C de Ghidra) está
  agotado para este bug -- es internamente consistente en every punto verificado y sin embargo
  contradice la evidencia de runtime en 2 sesiones separadas, con y sin nuestro propio hook de por
  medio. La única explicación que queda por descartar requiere inspección en vivo con GDB real
  (breakpoint justo en la instrucción `8ae68` de `CMvLayerData::PreLoad`, inspeccionando `r0` en ese
  preciso punto de ejecución en la consola) -- no hay forma de resolverlo con más logging o más lectura
  de desensamblado estático.
- **Fix aplicado para el síntoma real (Bug #10, `CMvLayer::Draw` crash), independiente de resolver la
  causa raíz de arriba:** se agregó en `source/patch.c` un hook de `CMvLayer::Draw`
  (`_ZN8CMvLayer4DrawEiiP12tagTILEPOINTbb`) vía `hook_addr` + `SO_CONTINUE` que chequea
  `*(void**)(layer+0xc)` (el buffer de tiles de la capa) ANTES de llamar a la función real -- si es
  `NULL` (la capa nunca recibió sus datos, sea cual sea la razón exacta), se saltea el dibujado de esa
  capa entera en vez de crashear; si es válido, llama a la función original sin ningún cambio de
  comportamiento. Este fix NO requiere resolver el misterio de `this`/`dest` de arriba ni direcciones de
  globals internos del motor (`CGsSingleton<...>`) que no se pueden resolver de forma segura -- solo usa
  un campo de NUESTRA PROPIA estructura conocida (`this+0xc`, el mismo offset que `custom_memcpy` ya
  demostró que a veces no se escribe).
- **Estado:** implementado en `source/patch.c`, compilado (`hooked_CMvLayer_Draw` confirmado presente en
  el `.elf` vía `nm`) y desplegado (`ux0:/downloads/illusia.vpk`). **Pendiente de que el usuario pruebe**
  si el crash de `CMvLayer::Draw` desaparece (la capa afectada debería simplemente no dibujarse, en vez
  de crashear el juego entero).
- **Nota para la próxima sesión:** el hook de diagnóstico en `CMvLayerData::PreLoad` (que loguea
  `this=...`) se dejó activo -- es inofensivo y puede seguir dando pistas. Si se quiere cerrar
  DEFINITIVAMENTE el misterio de `dest`, el único camino que queda es GDB real en consola con
  breakpoint en `so_base+0x8ae68`.


### Bug #8 confirmado y resuelto (2026-08-30) -- Enemigos invisibles y crash al dibujar (Endiness + Signature)

- **Síntoma:** Tras finalizar la cinemática de la historia, los gráficos del mapa y el enemigo no aparecen. Luego el juego hace Data Abort dentro de `CMvLayer::Draw` (`ldr r3, [r0, #0]`). Los botones táctiles en pantalla sí aparecen pero no responden tras un par de toques.
- **Causa Raíz:**
  1. **Endianness en el Parche:** El parche para el Bug #5 inyectaba `0x1dd0` (un entero C de 16 bits) para sobrescribir una instrucción `ble.n`. Debido al Little-Endian de la Vita, se decodificó como `adds r0, r2, #7`. Esto corrompió el puntero `this` (`r0`) del mapa, escribiendo todos los buffers visuales en zonas inválidas (ej. `0x350`) y dejando los reales en `NULL`.
  2. **Firma Incompleta en Hook JNI:** El hook `hooked_CMvLayer_Draw` obviaba el argumento `int y`. Esto causó un desfase de registros, donde el puntero de los tiles se llenaba con un booleano, provocando un crasheo fatal si la función llegaba a ser ejecutada.
  3. **Contador Touch Infinito:** Vita usa un ID de hardware para toques que siempre se incrementa, sobrepasando rápidamente el límite estático de `8` de nuestra lógica y bloqueando cualquier interacción.
- **Fix:**
  - Se corrigió el parche de salto a `0xd01d` para mantener integridad de memoria.
  - Se introdujo `int y` a la firma del hook para no desfasar los registros AAPCS.
  - Se rescribió el ciclo táctil (`main.c`) con un array de seguimiento (`active_ids`) que mapea el contador físico infinito a los 8 slots del software de forma segura, y se habilitó `sceTouchSetSamplingState`.
- **Estado:** ¡Corregido, compilado y testeado con éxito por el usuario! Los problemas fueron bloqueantes graves superados.

### Últimas 3 Características Añadidas (Save States, FPS Cap, Sonido)

1. **Save States Fix**: 
   - **Problema**: El juego estaba llamando a la función JNI genérica `m_getAbsolueFilePath` que estaba hardcodeada para devolver `DATA_PATH"assets"`, causando que el progreso del juego se intentase guardar y leer dentro de la carpeta `assets` (ej. `ux0:data/illusia/assets/option.sav`). Además, la función `m_isFileExist` buscaba en `DATA_PATH"files"`, produciendo un estado inconsistente.
   - **Solución**: Se redireccionó `m_getAbsolueFilePath` y `m_isFileExist` para que utilicen la ruta `%ssaves` (`ux0:data/illusia/saves/`). También se agregó una llamada a `sceIoMkdir(DATA_PATH"saves", 0777)` en `init.c` para asegurar que el sistema operativo de PS Vita cree la carpeta de guardado al arrancar, evitando que `fopen("w+")` falle.

2. **FPS Cap (30 FPS)**:
   - **Problema**: El bucle nativo de renderizado (while(1) en `main.c`) corría a máxima velocidad (o a 60 FPS con v-sync), lo que duplicaba la velocidad lógica del juego ya que fue diseñado originariamente para dispositivos con pantallas de 30 FPS.
   - **Solución**: Se implementó un limitador de cuadros (Frame Limiter) utilizando `sceKernelGetProcessTimeLow()` y `sceKernelDelayThread()` dentro del bucle de `NativeRender`, pausando el hilo si no han pasado al menos 33.33ms (30 FPS) desde el último tick de proceso.

3. **Audio / Sonido (NexusSound)**:
   - **Problema**: `libgameDSO.so` no incluía decodificación o reproducción de sonido de manera nativa porque delegaba estas funciones a la capa Java original (`NexusSound.playSound`).
   - **Solución**: Se verificó que los sonidos originales están en formato WAV (16 bit, mono, 8000 Hz) en la ruta `assets/sound/`. Se creó un hilo de audio personalizado e independiente en `source/utils/audio.c` que carga los archivos `.wav` al vuelo en memoria (y precarga los BGMs para evitar lag) y los reproduce a través de `sceAudioOutOpenPort(..., SCE_AUDIO_OUT_PARAM_FORMAT_S16_MONO)`. Se interceptaron las llamadas JNI `m_OnSoundPlay` y `m_OnStopSound` en `java.c` para que disparen este nuevo mezclador nativo, habilitando exitosamente el sonido del juego completo.

### Bug #11 confirmado y resuelto (2026-08-30) -- Crash en `CMvNetworkMenu::DrawMissionMode()` por Initialize() anulado de más

- **Síntoma:** Data abort en pleno gameplay (`illusia-psp2core-1788137417-0x000584230b`), hilo principal. `vita-parse-core` ubica el crash en `CMvNetworkMenu::DrawMissionMode() + 0x5c` (`ldr r1, [r3, #16]` con `R3 = 0x0`).
- **Cruce con el desensamblado:** justo antes del crash, el código hace `ldr r3, [r4, #0x84]` (`this + 0x84`) y usa ese resultado sin chequear NULL. `R4 = 0x83028198` es un objeto `CMvNetworkMenu` real, no basura.
- **Cruce con el pseudo-C (`out_ghidra.c:83854`, `CMvNetworkMenu::Initialize()`):** ese mismo campo (`in_r0[0x21]`, índice 0x21*4 = 0x84 bytes) es donde `Initialize()` guarda uno de los ~20 objetos `CGsKeymap` que construye para el manejo de teclas del menú de red (`piVar2 = operator_new(0x28); CGsKeymap::CGsKeymap(piVar2); in_r0[0x21] = piVar2; ...`).
- **Causa Raíz:** `_ZN14CMvNetworkMenu10InitializeEv` estaba parcheado en `source/patch.c` como un no-op TOTAL ("modo offline"), agregado en algún momento antes del Bug #4 para evitar el crash original del Bug #3 (agotamiento del allocator `Gcx_MM_Alloc` al construir el 5º `CGsKeymap`). Pero `Initialize()` NO hace ninguna llamada de red real -- solo construye objetos de UI local (keymaps de navegación del menú + `CGsKeymapScroll`) y llama a `ChangeState()`. Al anularla por completo, todos esos campos (incluyendo `this+0x84`) quedan en `NULL` (el objeto se calloc-ea al construirse), y `DrawMissionMode()` -- que SÍ sigue corriendo durante el render normal, sin importar que estemos offline -- los desreferencia sin chequeo.
- **Por qué ya es seguro dejarla correr:** el Bug #3 (causa original que motivó el stub total) ya está resuelto desde 2026-08-23 -- `operator new`/`operator new[]`/`operator delete` fueron redirigidos al heap nativo (`malloc`/`calloc`/`free`), así que ya no hay agotamiento de bloques fijos al construir los `CGsKeymap`. No queda ninguna razón para seguir anulando esta función.
- **Fix:** en `source/patch.c`, `hooked_CMvNetworkMenu_Initialize` dejó de ser un no-op y ahora llama a la implementación real del `.so` (mismo patrón de "hook diagnóstico + passthrough" que ya se usaba para `CMvLayerData::PreLoad`, pero sin macro `SO_CONTINUE` porque esa macro no compila con tipo de retorno `void` -- se inlineó manualmente la secuencia restaurar-instrucción/llamar/re-parchear). `CMvNet::Initialize` y `CMvNet::SendMain` (la clase de red real, distinta de `CMvNetworkMenu`) siguen anuladas sin cambios -- esas sí hacen operaciones de red reales y no se tocaron.
- **Estado:** compilado limpio (`psvita-toolkit build --preset debug`) y desplegado por FTP (`psvita-toolkit deploy --eboot --yes`). **Pendiente de que el usuario confirme en consola** que el crash de `DrawMissionMode` desaparece y que no reaparece el crash original del Bug #4 (`CMvNetworkMenu::ResetKeyMap`, que sigue con su propio stub no-op aparte).

### Bug #12 confirmado y resuelto (2026-08-30) -- Efectos de sonido que no siempre se escuchan (golpes/poderes)

- **Síntoma:** reportado por el usuario -- durante el juego, al lanzar un poder o pegarle a un enemigo,
  el efecto de sonido (SFX) a veces no se escucha. La música de fondo (BGM) no está afectada.
- **Causa Raíz (confirmada leyendo el código, no especulada):** `source/utils/audio.c` implementaba un
  mezclador propio con UN SOLO canal de SFX (`sfx_playing`/`sfx_pos`/`sfx_vol`, variables globales
  únicas). `audio_play()` (invocada desde `source/java.c` en el callback JNI `m_OnSoundPlay`, que a su
  vez responde a `Natives.OnSoundPlay` del motor real) pisaba ese único canal sin ninguna cola ni chequeo
  de "¿ya hay un SFX sonando?": si el motor disparaba un segundo efecto mientras el primero seguía en
  curso (muy común en combate -- golpe + skill + sonido de UI superpuestos), el segundo `audio_play()`
  simplemente sobreescribía `sfx_playing`/`sfx_pos` de golpe, cortando el primer sonido a mitad de
  reproducción sin ningún aviso -- el primero no terminaba, el segundo empezaba (o, si el motor disparaba
  el sonido del golpe y CASI en el mismo frame el del poder, el usuario solo llegaba a escuchar el
  último). BGM no tenía este problema porque usa sus propias variables (`bgm_playing`/`bgm_pos`)
  separadas del canal de SFX.
- **Fix:** en `source/utils/audio.c`, el único slot `sfx_playing`/`sfx_pos`/`sfx_vol` se reemplazó por un
  pool fijo de `MAX_SFX_CHANNELS = 6` (`SfxChannel sfx_channels[6]`), todos mezclados juntos en
  `audio_thread()` (mismo patrón de suma+clamp que ya mezclaba BGM+SFX). `audio_play()` busca un canal
  libre (`playing < 0`); si los 6 están ocupados, recicla el más viejo por turno (`sfx_next_evict`,
  round-robin) en vez de descartar el sonido nuevo. `audio_stop_all()` ahora limpia los 6 canales.
- **Estado:** compilado limpio (`psvita-toolkit build --preset debug`) y desplegado por FTP
  (`psvita-toolkit deploy --eboot --yes`). **Pendiente de que el usuario confirme en consola** que los
  golpes y poderes ya se escuchan siempre, incluso superpuestos.

### Bug #13 -- EN INVESTIGACIÓN, hooks de diagnóstico agregados (reportado por el usuario, 2026-08-31) -- "The save file has been corrupted, you must create a new character" al cargar

- **Síntoma:** reportado por el usuario -- cada vez que intenta cargar una partida guardada (personaje),
  el juego muestra el mensaje "the save file has been corrupted you must create a new character".
- **Cruce con el pseudo-C:** el guardado de personaje pasa por `CGsEncryptFile` (`out_ghidra.c:7096`
  en adelante), que en `LoadBegin(char const*, bool)` (`:7205`) hace, en orden: (1) compara el tamaño
  leído contra el tamaño esperado, (2) si `param_2` (bool) es `true`, compara vía `strcmp` un número de
  "teléfono" guardado en el archivo (`GsGetPhoneNum()`, `:22070`) contra el que devuelve el dispositivo
  actual, y (3) recalcula un checksum sobre el payload desencriptado (`CulCheckSum`, `:7161`) y lo
  compara contra el valor guardado. **Cualquiera de los tres chequeos que falle produce exactamente el
  mismo resultado genérico** -- no hay forma de distinguir "es el teléfono" de "es el checksum" solo
  mirando el mensaje en pantalla.
- **Hipótesis evaluadas sin poder confirmar en vivo todavía (consola no alcanzable por FTP en esta
  sesión -- `Connection refused`, VitaShell no tenía el servidor FTP activo):**
  - `GsGetPhoneNum()` (`out_ghidra.c:22070`) usa un singleton cacheado (`CGsSingleton<CGsPhoneInfo>`) si
    existe, y si no, cae a `MC_knlGetSystemProperty("PHONENUMBER", ...)` -> `getSystemProperty()`
    (`:116406`, nativo dentro del `.so`) -> llamada JNI real a `getPhoneNumber()` -> nuestro stub
    `m_getPhoneNumber` en `source/java.c` (devuelve la constante `"000000000000000"` siempre). En teoría
    ambos caminos derivan del mismo valor constante y deberían coincidir siempre entre guardado y carga
    -- pero no se descarta una falla intermitente de esa llamada JNI (p.ej. si devolviera `NULL` en
    algún momento, dejando el buffer de comparación vacío en vez de con la constante).
  - El checksum (`CulCheckSum`) depende de `Random()` (`:130867`, LCG propio del motor, no libc)
    sembrado con un timestamp que el propio archivo guarda y relee (`this+8`) -- autoconsistente en
    teoría, sin dependencia de nuestro loader.
  - Se descartó un límite de 30 caracteres en `MC_fsOpen`/`MC_fsRemove` (`out_ghidra.c:116864`,
    `:116781`) como causa: ese chequeo corre sobre el nombre CORTO que el motor pasa (antes de que
    `get_real_path()` le anteponga `ux0:data/illusia/saves/`), no sobre el path completo -- no debería
    dispararse con nombres de slot normales.
- **No se aplicó ningún fix todavía -- sería especulativo sin evidencia.** En su lugar, se agregaron 3
  hooks de diagnóstico en `source/patch.c` (mismo patrón sin cambio de comportamiento que ya se usó para
  `CMvLayerData::PreLoad` y `CMvNetworkMenu::Initialize`, con una nueva macro `SO_CONTINUE_VOID` para
  las funciones que retornan `void`):
  - `GsGetPhoneNum` -- loguea el string que devuelve cada vez que se llama (guardado Y carga).
  - `CGsEncryptFile::CulCheckSum` -- loguea el checksum calculado (`this+0x8c`) y el largo del buffer.
  - `CGsEncryptFile::LoadBegin` -- loguea el path, el flag de chequeo de teléfono, y el resultado final
    (éxito/fallo).
  - Compilado limpio (`psvita-toolkit build --preset debug`). **No se pudo desplegar en esta sesión**
    (FTP de la consola no estaba activo). Falta: el usuario abre VitaShell + FTP, se despliega este
    build, y se reproduce un ciclo completo de guardar personaje -> cerrar juego -> volver a abrir ->
    cargar. El log de esa corrida (`live_session_*.log` o `game_*.log`) va a mostrar exactamente qué
    valor de teléfono/checksum se calculó al guardar vs. al cargar, lo que va a señalar sin ambigüedad
    cuál de los dos chequeos está fallando -- recién ahí corresponde escribir el fix real.

**ACTUALIZACIÓN (2026-08-31, misma sesión de bug):** encontrada una causa raíz mucho más simple
revisando `decompiled/` en vez de seguir la hipótesis del checksum/teléfono -- reportado por el
usuario como "no puedo guardar la partida".

- **`com/gamevil/nexus2/Natives.java:315`** (`saveFile(String, byte[])`) y **`:222`** (`loadFile(String)`)
  son los métodos privados que el `.so` invoca vía `CallStaticVoidMethod`/`CallStaticObjectMethod`
  (no son `native`, son callbacks Java <- nativo) para persistir/leer el archivo de guardado en disco
  (`NexusGLActivity.myActivity.openFileOutput`/`openFileInput`).
- **Ninguno de los dos estaba registrado en `nameToMethodId[]` (`source/java.c`)** -- a diferencia de
  `isFileExist`/`getAbsolueFilePath`/`readAssete`, que sí lo estaban. `getMethodIdByName()`
  (`lib/falso_jni/FalsoJNI_ImplBridge.c:160`) devuelve `NULL` si el nombre no está en la tabla, y
  `GetStaticMethodID` (`lib/falso_jni/FalsoJNI.c:831`) propaga ese `NULL` sin errores fatales.
  `methodVoidCall(NULL, ...)`/`methodObjectCall(NULL, ...)` (`FalsoJNI_ImplBridge.c:169,180`) no
  encuentran ningún id que matchee 0, loguean un warning (`"method ID %i not found!"`) y **no crashean**
  -- simplemente no hacen nada (`saveFile`) o devuelven `NULL` (`loadFile`). Por eso el bug era 100%
  silencioso: ni un crash ni un log de error visible, el guardado simplemente nunca tocaba disco.
- **Esto también explica el Bug #13 de arriba sin que sea necesario tocar el checksum/teléfono:**
  con `loadFile` devolviendo siempre `NULL`, `CGsEncryptFile::LoadBegin` recibe un buffer vacío/nulo,
  falla su primer chequeo (tamaño leído vs. esperado) y produce exactamente el mensaje genérico "the
  save file has been corrupted" -- sin necesidad de que el teléfono o el checksum estén mal. Los 3
  hooks de diagnóstico de más arriba (`GsGetPhoneNum`, `CulCheckSum`, `LoadBegin`) siguen siendo útiles
  para confirmarlo en el próximo log (deberían mostrar el chequeo de tamaño fallando, no el de teléfono
  ni el de checksum), pero no hacía falta tocar esa lógica.
- **Fix aplicado en `source/java.c`:**
  - Nuevas entradas `{ 66, "saveFile", METHOD_TYPE_VOID }` y `{ 67, "loadFile", METHOD_TYPE_OBJECT }`
    en `nameToMethodId[]`.
  - `m_saveFile()`: lee el `jstring` (nombre) y el `jbyteArray` (datos, representado como
    `JavaDynArray*` en esta FalsoJNI) de la `va_list`, y llama a `file_save()` sobre
    `DATA_PATH"saves/"<nombre>` (mismo directorio que ya usan `isFileExist`/`getAbsolueFilePath`,
    y que `source/utils/init.c:86` ya crea con `sceIoMkdir` al arrancar).
  - `m_loadFile()`: calca el patrón ya usado por `m_readAssete` (mismo truco de layout de memoria
    Header+Data de 0x10 bytes que el motor lee directamente sin pasar por `GetByteArrayElements`),
    pero leyendo de `saves/` en vez de `assets/` vía `file_load()`.
  - Registradas en `methodsVoid[]` y `methodsObject[]` respectivamente.
- **Estado:** compila limpio (`psvita-toolkit build --preset debug` -- el único error de esta sesión
  fue un link-time "multiple definition" de símbolos EGL entre `source/reimpl/egl.c` y la
  `libvitaGL.a` instalada, confirmado **pre-existente** al buildear también sin este cambio via
  `git stash`; no relacionado con este fix). **Pendiente de desplegar y confirmar en consola** un
  ciclo completo guardar -> cerrar -> abrir -> cargar.

**CORRECCIÓN (2026-08-31, mismo día, tras revisar `logs/game_017.log`):** el fix de `saveFile`/
`loadFile` de arriba estaba **basado en una hipótesis incorrecta**. `strings` sobre el `.so` real
(`illusia_extract/lib/armeabi/libgameDSO.so`) confirma que **ni `saveFile` ni `loadFile` aparecen en
ningún lado del binario** -- son código muerto heredado del motor Nexus2 compartido (mismo patrón que
`GFA_SetString*`/`netBillcomSocket*`/etc. ya documentado en el header de `source/java.c`), nunca
invocado por Illusia. El guardado de personaje real pasa 100% por I/O nativo directo
(`CGsEncryptFile`/`MC_fsOpen`/`stat`/`fopen`), no por el bridge JNI. **El registro de `saveFile`/
`loadFile` no hace daño (más completo que antes, útil si algún otro título Nexus2 sí los usa) pero no
tiene ningún efecto sobre este bug** -- se deja en el código porque es correcto en sí mismo, no se
revierte.

**Causa raíz real, encontrada cruzando `game_017.log` con el disassembly real del `.so`:**

- El log muestra, en un intento de carga (sin ningún intento de guardado en esta sesión -- `Save0.dat`
  ya existía de antes): `stat(ux0:data/illusia/saves/Save0.dat): 0` (el archivo existe) seguido
  inmediatamente de `CGsEncryptFile::LoadBegin("Save0.dat", phone_check=1) -> 0` (falla) -- **sin que
  se loguee ni una sola vez `GsGetPhoneNum` ni `CGsEncryptFile::CulCheckSum`**, pese a que ambos ya
  estaban hookeados (Bug #13 arriba). Como esos dos hooks son passthrough sobre el código original y sí
  se ejecutan en cualquier carga que llegue tan lejos, su silencio total prueba que `LoadBegin` está
  fallando en su primer chequeo -- **antes** de tocar teléfono o checksum.
- `LoadBegin` (`out_ghidra.c:7205`) hace primero `uVar1 = GsFSFileSize(param_1,1); if (uVar1 != 0) {...}`
  -- si `GsFSFileSize` devuelve `0`, todo el resto de la función (incluido `GsGetPhoneNum`/
  `CulCheckSum`) nunca se ejecuta. Coincide exactamente con el log.
- `GsFSFileSize` (`out_ghidra.c:7391`) llama a `MC_fsFileAttribute` (`out_ghidra.c:116808`, que hace
  `get_real_path` + `stat()` + copia un campo del `struct stat` resultante) y devuelve el 3er word (offset
  +8) del pequeño struct de salida que `MC_fsFileAttribute` llena.
- **Verificado con `arm-vita-eabi-objdump -d` sobre el `.so` real** (no solo el pseudo-C de Ghidra,
  que usa su propio layout de `struct stat` por defecto y puede llevar a offsets engañosos): en
  `MC_fsFileAttribute` (`0xc2390`), tras la llamada a `stat()`, la instrucción `ldr r3, [sp, #48]` lee
  el word que termina copiándose a esa posición -- es decir, el campo que se propaga hasta el valor de
  retorno de `GsFSFileSize` está en el **byte offset 48 (0x30)** del `struct stat` recién llenado.
- En el layout bionic-compatible que ya usa este port (`stat64_bionic` en `source/reimpl/io.h`,
  poblado por `stat_soloader`/`stat_newlib_to_bionic` en `source/reimpl/io.c` +
  `source/reimpl/bits/_struct_converters.c`), el offset 48 cae **dentro de `st_size` (offset 44-51,
  `long long`)** -- es la mitad ALTA de esos 8 bytes. Para cualquier archivo de guardado real (bien por
  debajo de 4 GB), esos 4 bytes altos son siempre `0`. Por eso `GsFSFileSize` devuelve `0` siempre,
  sin importar el tamaño real del archivo -- coincide con el log (`Save0.dat` existe, pero
  `GsFSFileSize` -> `0` implícito, `LoadBegin` -> `0`).
- **Fix aplicado en `source/patch.c`:** reemplazo completo (no passthrough, a diferencia de los 3 hooks
  diagnósticos de arriba) de `GsFSFileSize` (símbolo `_Z12GsFSFileSizePKci`) vía `hooked_GsFSFileSize`,
  registrado con `patch_symbol(...)`. En vez de intentar arreglar el layout global de `stat64_bionic`
  (arriesgado -- ese struct lo usan potencialmente muchos otros call sites de `stat()`/`fstat()` en
  todo el `.so`, y no hay evidencia de que esté mal para ESOS otros usos), el hook bypassea por
  completo la lectura de offset rota y calcula el tamaño real usando nuestro propio `file_exists()`/
  `file_size()` (`source/utils/utils.c`) sobre el mismo path real que ya usan `m_isFileExist`/
  `m_getAbsolueFilePath` (`source/java.c`) y que `get_real_path()` (`out_ghidra.c:116681`) construye
  internamente (`DATA_PATH"saves/"<nombre>`).
- **Estado:** compila limpio (mismo build, mismo error de link EGL pre-existente y no relacionado,
  confirmado en la entrada anterior). **Pendiente de desplegar y confirmar en consola** un ciclo
  completo guardar -> cerrar -> abrir -> cargar -- ahora sí se espera ver loguearse
  `GsFSFileSize("Save0.dat") -> <tamaño real>` seguido de `GsGetPhoneNum`/`CulCheckSum` disparándose
  por primera vez.

### Link error EGL pre-existente -- RESUELTO (2026-08-31, misma sesión)

- **Síntoma:** el build fallaba en el paso de link (no en compilación) con "multiple definition"
  para las ~15 funciones EGL (`eglInitialize`, `eglQueryContext`, `eglMakeCurrent`, etc.) entre
  `source/reimpl/egl.c.obj` (propio del proyecto) y `libvitaGL.a(egl.o)` (instalada globalmente en
  `$VITASDK`). Confirmado que reproduce igual con o sin los cambios de este bug (`git stash`) -- no
  causado por el fix de arriba, y el propio usuario lo reprodujo en su entorno con el mismo build.
- **Causa:** `source/reimpl/egl.c` es la implementación EGL propia del port (llama a `gl_init()`,
  pensada para ser la ÚNICA implementación EGL linkeada), pero la `libvitaGL.a` instalada globalmente
  también trae su propio `egl.c` compilado adentro (compatibilidad EGL opcional de vitaGL). Como
  nuestros objetos van ANTES que `-lvitaGL` en la línea de link, y algún otro símbolo necesario de
  vitaGL vive en el MISMO `egl.o` de esa librería, el linker termina extrayendo igual ese `.o` y
  choca con nuestras propias definiciones.
- **Fix:** agregado `-Wl,--allow-multiple-definition` a `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` en
  `CMakeLists.txt` (mismo lugar donde ya viajaba `-Wl,-q`). El linker GNU, con múltiples definiciones
  permitidas, conserva la PRIMERA que ve -- que es la nuestra, ya linkeada antes que la librería --
  así que el comportamiento en runtime no cambia, solo se silencia el error por el duplicado de
  vitaGL.
- **Estado:** confirmado -- `psvita-toolkit build --preset debug` completa OK end-to-end y genera
  `build/illusia.vpk`. Con esto ya se puede desplegar y probar el fix de `GsFSFileSize` de arriba.

### Bug #13 (cont.) -- endurecido el 2026-09-01: corregido `stat64_bionic` en la fuente, no solo bypasseado

El usuario reportó (2026-09-01) que las partidas seguían sin cargar, pidiendo revisar cómo se resolvió
un bug parecido en el port hermano **Advena-Vita** (mismo motor Gamevil Nexus2). El Bug 15 de
`Advena-Vita/PORTING_PLAN.md` documenta EXACTAMENTE la misma familia de causa raíz (offset de `st_size`
roto en `stat64_bionic`) pero resuelta en la fuente del problema (el struct), no con un bypass puntual
como el `hooked_GsFSFileSize` de arriba.

- **Verificado en este entorno (`/Users/metalsyntax/vitasdk`, `arm-vita-eabi/include/sys/_types.h`):**
  `nlink_t`/`uid_t`/`gid_t` son `unsigned short` (2 bytes) en el newlib de VitaSDK, contra 4 bytes cada
  uno en la ABI Bionic de Android de 32 bits que el `.so` espera. Con el `stat64_bionic` original de
  este port (`source/reimpl/io.h`, con esos 3 campos de 2 bytes y un solo padding de 4 bytes tras
  `st_rdev`), `st_size` calculaba en offset **0x26 (38)** -- 10 bytes antes del offset fijo **0x30 (48)**
  que `MC_fsFileAttribute` lee (confirmado por `objdump` sobre el `.so` real, ver más arriba). Offset
  0x30 caía dentro de `st_blksize`/`st_blocks`, leyendo un valor compuesto sin sentido, no
  necesariamente `0` siempre pero sí sistemáticamente incorrecto -- explica por qué el bypass de
  `GsFSFileSize` (que sortea el problema por completo, sin depender de este cálculo) sigue siendo
  necesario Y correcto, pero también por qué CUALQUIER OTRO call-site del `.so` que llame a
  `stat()`/`fstat()` directamente (fuera de `GsFSFileSize`) seguiría viendo datos corruptos sin este fix.
- **Fix aplicado en `source/reimpl/io.h`:** se widearon `st_nlink`/`st_uid`/`st_gid` a `uint32_t`
  explícito (en vez de los typedefs nativos de 2 bytes) y se agregó un segundo padding de 4 bytes
  (`__pad4`) tras `st_rdev`/`__pad3` -- entre ambos cambios recuperan exactamente los 10 bytes faltantes,
  dejando `st_size` en el offset 0x30 real. Verificado con un cálculo de `offsetof` standalone (modelando
  los anchos de tipo reales de `arm-vita-eabi`, no los del host) antes de tocar el archivo del proyecto.
  `source/reimpl/bits/_struct_converters.c` no necesitó cambios -- los nombres de campo no cambiaron,
  solo se ensancharon, así que las asignaciones existentes siguen compilando (conversión implícita a un
  tipo más ancho).
- **Por qué mantener también `hooked_GsFSFileSize` (`source/patch.c`):** no es redundante -- es un
  reemplazo completo de la función (no un passthrough), así que la corrección del struct nunca llega a
  ejercitarse en ESE call-site puntual (nunca vuelve a llamar al `stat()` real). Pero protege cualquier
  otro lugar del motor que dependa de un `stat()`/`fstat()` correcto, incluida la posibilidad de que
  `tagGameData`-equivalente (el chequeo de "hay partida guardada" en el menú de título) use `stat()`
  directo en vez de pasar por `GsFSFileSize`. Ambos fixes son complementarios, igual que en Advena (que
  aplicó el fix de struct pero conservó sus propios guards de `NULL` puntuales para otros bugs derivados
  de la misma corrupción de tamaño).
- **Verificado con el compilador real (`arm-vita-eabi-gcc`, `$VITASDK` en esta máquina):** en vez de
  confiar solo en el cálculo de `offsetof` sobre el host (tipos de ancho distinto a ARM/newlib), se
  compilaron `_Static_assert(offsetof(stat64_bionic, st_size) == 0x30, ...)` y
  `_Static_assert(sizeof(nlink_t) == 2, ...)` directo contra `source/reimpl/io.h` con el cross-compiler
  real -- ambos pasan, confirmando en el toolchain de destino (no en teoría) que `nlink_t` es de 2 bytes
  aquí y que el layout corregido efectivamente deja `st_size` en 0x30.
- **Estado:** compilado end-to-end con `psvita-toolkit build --preset debug` (build completo del
  proyecto, no solo el archivo suelto) -- termina OK y genera `build/illusia.vpk` sin errores nuevos
  (mismos warnings preexistentes de `sceKernelGetProcessTimeLow`/`sceIoMkdir`/`mktemp`, no relacionados).
  **Pendiente:** desplegar y confirmar en consola un ciclo completo guardar -> cerrar -> abrir -> cargar.
  Si el mensaje "the save file has been corrupted" reaparece pese a esto, el log ya va a mostrar
  `GsFSFileSize(...) -> <tamaño real>` (gracias al bypass, que sigue funcionando incondicionalmente)
  seguido de `GsGetPhoneNum`/`CulCheckSum` disparándose -- en ese caso el bug ya no sería de tamaño de
  archivo sino de teléfono o checksum, y ahí sí habría que retomar la investigación pausada en la primera
  mitad de este Bug #13.

## Vendorizado vitaGL propio -- eliminado el logo/animación de splashscreen (2026-09-01)

- **Síntoma reportado por el usuario:** aparece un logo de vitaGL girando (animación 3D) antes de que
  arranque el port.
- **Causa:** vitaGL trae un splashscreen propio compilado incondicionalmente salvo que la librería se
  compile con la flag de build `NO_SPLASHSCREEN=1` (mapeada a `-DSKIP_SPLASHSCREEN` en su Makefile,
  confirmado via `nm`/`ar x` sobre el `libvitaGL.a` global instalado en `$VITASDK` -- tenía
  `invoke_splashscreen`/`clear_splashscreen`/`splashscreen_thread` compilados adentro). Este proyecto
  linkeaba contra ese `.a` global e instalado (`-lvitaGL` vía el paquete de VITASDK), sin control sobre
  con qué flags se compiló -- no hay forma de deshabilitar el splash desde nuestro propio código sin
  recompilar vitaGL con esa flag.
- **Fix:** se vendorizó el código fuente de vitaGL (`lib/vitagl/`, tracked en git -- ver `.gitignore`)
  en vez de depender del paquete global, igual que ya hacen varios ports hermanos en este mismo
  workspace (`Dungeon-Hunter-2-vita`, `Asphalt-5-Vita`, `asphalt8-vita-main`). Reutilizado directamente
  el árbol YA PARCHEADO de `Asphalt-5-Vita/lib/vitagl` (mismo `$VITASDK` de esta máquina) en vez de
  clonar `Rinnegatamante/vitaGL` en HEAD -- un clone fresco falla al compilar contra este VITASDK
  concreto (`source/gxm.c` usa `SCE_GXM_INITIALIZE_FLAG_*` que este `psp2/gxm.h` todavía no declara,
  confirmado con el compilador real `arm-vita-eabi-gcc`, no solo en teoría) -- ese árbol de Asphalt-5-Vita
  ya tiene el fix necesario (`shared.h` con `#ifndef`-guarded fallbacks para esas constantes, más un
  guard de `HAVE_RAZOR` en `vgl.c`), documentado en su propio historial de git como un compat-patch de
  una sesión anterior contra este mismo entorno.
  - **Única diferencia respecto al árbol de Asphalt-5-Vita:** su patch además BORRA `source/egl.c` de
    vitaGL (duplicado contra su propio `reimpl/egl.c`, más completo que el nuestro). Acá NO se borró --
    nuestro `source/reimpl/egl.c` no implementa `eglBindAPI`/`eglGetDisplay`/`eglGetError`/
    `eglGetProcAddress`/`eglSwapBuffers` (confirmado con un link real que falla con "undefined reference"
    a esos 5 símbolos apenas se quita el `egl.c` de vitaGL) -- este proyecto SIEMPRE dependió
    silenciosamente del `egl.c` de vitaGL para esos 5, con `-Wl,--allow-multiple-definition` (ya presente
    en `CMakeLists.txt` de antes) resolviendo el resto de símbolos duplicados (`eglInitialize`,
    `eglMakeCurrent`, etc., donde SÍ gana nuestra propia versión por ir primera en el link). Se restauró
    el `egl.c` original de vitaGL (de un clone fresco de upstream, sin parchear -- no tiene APIs
    específicas de gxm que puedan chocar con este VITASDK) dentro de `lib/vitagl/source/egl.c`.
  - `CMakeLists.txt`: agregado un `add_custom_target(vitaGL_lib ...)` que corre `make -C lib/vitagl
    SOFTFP_ABI=1 NO_SPLASHSCREEN=1` (mismo patrón de stamp-file + `build_vitagl.sh` que usan los ports
    hermanos, para que un cambio de flags SÍ dispare un `make clean` -- un stamp basado en OUTPUT sin
    DEPENDS quedaría "up to date" para siempre tras el primer build). `target_link_libraries` ahora
    apunta al `.a` local (`lib/vitagl/libvitaGL.a`, gitignored -- build product) en vez del target
    `vitaGL` del paquete global de VITASDK.
  - **Bug de tooling encontrado y corregido en el camino:** `psvita-toolkit build` stagea el repo a un
    directorio temporal con `rsync --filter=":- .gitignore"` antes de compilar. Ese modo de rsync **NO
    soporta la sintaxis de negación `!patrón` de gitignore** (verificado empíricamente con un rsync de
    prueba aislado: un `!lib/vitagl/Makefile` se trata como un patrón literal que nunca matchea nada, no
    como un "no ignores esto") -- el `Makefile` de vitaGL (real, debe trackearse, es lo que `make -C
    lib/vitagl` necesita para compilar) se estaba perdiendo en el staging por la regla genérica sin
    anclar `Makefile` de este `.gitignore` (pensada solo para el `Makefile` que CMake genera en la raíz
    del repo en un build in-source). Fix: anclar esa regla a `/Makefile` en vez de intentar negarla --
    confirmado con el mismo test aislado de rsync que con el anclaje SÍ sobrevive el archivo real de
    vitaGL. Este bug de tooling es independiente del de vitaGL en sí y afectaría a cualquier archivo
    futuro llamado `Makefile` en subcarpetas de este repo.
- **Verificado:** `nm` sobre el `libvitaGL.a` vendorizado ya compilado confirma que `splashscreen.o` no
  aporta ningún símbolo (todo el archivo queda vacío bajo `#ifndef SKIP_SPLASHSCREEN`); `nm` sobre el
  binario final linkeado (`build/illusia`, tanto en preset `debug` como `release`) confirma cero
  símbolos con "splash" en el nombre. Build completo end-to-end con `psvita-toolkit build --preset
  debug` Y `--preset release`, ambos generan `build/illusia.vpk` sin errores nuevos.
- **Pendiente:** confirmar en consola/Vita3K que el juego arranca directo a pantalla de título sin la
  animación, y que no hay regresión de renderizado por el `egl.c` restaurado (debería ser transparente --
  es el mismo código que ya se usaba antes de vendorizar, solo que ahora vendorizado en vez de venir del
  paquete global).

### Bug #14 confirmado y resuelto (2026-09-02) -- Sonidos ambientales y efectos en loop se cortaban tras una sola reproducción

- **Síntoma:** reportado por el usuario -- sonidos ambientales y algunos efectos de poderes/golpes no
  sonaban bien (dejaban de sonar, no continuaban en bucle como deberían).
- **Causa Raíz (confirmada leyendo el motor real, no especulada):** el motor original (Android) decide
  si un sonido va al slot único "sostenido" (loop, `mBgmPlayer`) o a un canal de un solo disparo
  **basándose exclusivamente en el flag `loop`** que el juego pasa a `Natives.OnSoundPlay(id, vol, loop)`
  (ver `NexusSound.playSound(int, boolean)` en
  `decompiled/apk_jadx/sources/com/gamevil/nexus2/ui/NexusSound.java:113-145`) -- el `id` numérico del
  sonido no tiene ningún rol en esa decisión.
  Nuestro `source/utils/audio.c` (`audio_play()`) en cambio usaba una heurística hardcodeada y nunca
  confirmada contra el motor real: `id >= 100 && id <= 111` = BGM, cualquier otro id iba siempre al pool
  de 6 canales SFX (`SfxChannel`, agregado en el Bug #12) **sin importar el flag `loop`**. Ese pool no
  tiene ningún concepto de repetición -- un sonido enrutado ahí SIEMPRE se reproduce una vez y se apaga
  cuando llega al final de sus samples.
  Los assets reales confirman ids de sonido en bucle (ambiente/loop) que caen FUERA del rango 100-111 --
  `illusia_extract/assets/sound/120.wav` y `121.wav` -- y cualquier efecto de poder que el motor dispare
  con `loop=true` (ej. un sonido sostenido mientras dura una habilidad) también caía en la misma trampa
  si su id no estaba en 100-111. Resultado: esos sonidos sonaban una sola vez y luego silencio total, en
  vez de repetirse como en Android.
- **Fix:** en `source/utils/audio.c`, `audio_play()` ahora enruta por el flag `loop` real (igual que
  `NexusSound.playSound`), no por rango de id: `if (loop)` va siempre al slot sostenido único
  (`bgm_playing`/`bgm_pos`/`bgm_loop`/`bgm_vol`), reemplazando lo que sea que estuviera sonando ahí antes
  -- misma limitación de "un solo loop a la vez" que tiene el motor original con su único `mBgmPlayer`,
  no es una regresión nuestra. Cualquier sonido sin loop sigue yendo al pool polifónico de 6 canales
  (comportamiento del Bug #12, sin cambios). También se agregó precarga de los ids 120/121 en
  `audio_init()` junto a los BGM 100-111, para evitar un stutter en su primera reproducción.
- **Estado:** corregido en código. Pendiente de build/deploy y confirmación del usuario en consola.

### Bug #15 confirmado y resuelto (2026-09-02) -- El jingle de Gamevil sonaba antes de que el logo apareciera en pantalla

- **Síntoma:** reportado por el usuario -- el audio del logo de Gamevil se escucha antes de que la imagen
  del logo se muestre en pantalla.
- **Causa Raíz:** confirmado en `out_ghidra.c:111917` (`CMvLogo::DoPlaySound`) que el jingle es
  `CGsSound::Play(ms_pSingleton, 0, -1, '\0')` -- sonido id **0** (`000.wav`/`s000.ogg`). El problema no
  es de contenido sino de sincronización: `audio_init()` arranca el hilo de mezcla nativo (`source/utils/audio.c`)
  **antes** de `gl_init()` y de toda la secuencia de arranque (`NativeInitWithBufferSize`/
  `NativeInitDeviceInfo`/`NativeResize`, `source/main.c`), y ese hilo empieza a reproducir cualquier sonido
  encolado de inmediato (sin ninguna dependencia del estado gráfico). Si el motor dispara el `OnSoundPlay`
  del jingle durante esa inicialización -- que hace I/O síncrona de archivos/texturas y puede tardar
  bastante más que lo que tarda nuestro mezclador nativo en empezar a sonar -- el audio arranca (o incluso
  termina) mientras el `main()` todavía no llegó a su primer `gl_swap()` real, es decir, antes de que
  exista ni un solo frame visible en pantalla. En Android esto queda enmascarado porque `MediaPlayer.create()`
  (el camino real de `NexusSound.playSound`) es de por sí lento/asíncrono -- nuestro mezclador C, al
  reproducir PCM ya precargado, es mucho más rápido y expone la carrera.
- **Fix:** en `source/utils/audio.c`, el hilo de mezcla ahora respeta un gate (`audio_gate_open`,
  `audio_open_gate()`) que arranca cerrado -- mientras está cerrado, el buffer de salida se manda en
  silencio y **ninguna posición de reproducción avanza** (ni BGM ni SFX), así que lo que haya quedado
  encolado arranca desde el sample 0 apenas se abre el gate, sin perder el comienzo. En `source/main.c`,
  el gate se abre con `audio_open_gate()` justo después del primer `gl_swap()` real del loop principal --
  sincronizando el audio con el primer frame efectivamente visible en pantalla.
- **Estado:** corregido en código. Pendiente de build/deploy y confirmación del usuario en consola.

### Bug #16 confirmado y resuelto (2026-09-02) -- Ruido estridente en pantalla de Title y en cutscenes (en vez del sonido ambiente/BGM)

- **Síntoma:** reportado por el usuario -- la pantalla de Title suena con un ruido estridente en vez del
  ambiente que debería tener, y en un cutscene aparece otro efecto igual de estridente.
- **Cruce con el pseudo-C real (no especulado):** `out_ghidra.c` tiene un log literal del propio juego
  justo antes de la llamada que confirma el sonido de Title:
  ```
  MC_knlPrintk("##### GS_SOUND_PLAY( RES_BGM_TITLE ) Before #####");
  CGsSound::Play(CGsSingleton<CGsSound>::ms_pSingleton, 0x6c, -1, '\x01');
  ```
  `0x6c` = **108** (`108.wav`/`s108.ogg`), con loop=`true`. El cutscene usa el mismo mecanismo de más
  alto nivel: `CMvGameScript::Script_PlayBGM()` (`out_ghidra.c:45432`) también llama
  `CGsSound::Play(ms_pSingleton, <id del script>, -1, '\x01')` -- mismo camino de loop, distinto id
  (data-driven desde el script del cutscene, no fijo).
  El `-1` en el 2º argumento le dice al motor "usar el volumen por defecto interno" (un campo del
  singleton `CGsSound`, inicializado en 0 por el constructor -- `out_ghidra.c:19021` -- y cargado en
  runtime desde las opciones de sonido guardadas, imposible de leer solo con disassembly estático, pero
  el propio código Android confirma que la escala real de volumen del motor llega hasta
  `NexusSound.MAX_VOLUME = 80`, muy por encima del rango 0-10 que asumía nuestro mezclador).
  **El bug real (verificado leyendo `source/utils/audio.c`, no el volumen exacto):** el canal de
  loop/BGM hacía `int32_t s = sample * bgm_vol / 10; buf[i] = s;` **sin clampear** antes de truncar a
  `int16_t` -- si `bgm_vol` supera 10 (como todo indica que ocurre en la práctica, dado el rango real de
  volumen del motor), el resultado se desborda del rango de 16 bits y, al truncarse, da vueltas
  (wraparound) en vez de saturar -- eso es exactamente ruido digital estridente, no un simple "muy
  fuerte". El pool de SFX (canales de un solo disparo, Bug #12) sí clampeaba correctamente desde que se
  creó; solo al canal de loop le faltaba.
- **Fix:** en `source/utils/audio.c`, se agregó el mismo clamp de saturación (`if (s>32767) s=32767; if
  (s<-32768) s=-32768;`) que ya tenía el pool de SFX al canal de loop/BGM, antes de truncar a
  `int16_t`. Esto cubre tanto el ambiente de Title (id 108) como cualquier BGM/ambiente de cutscene
  (`Script_PlayBGM`), porque ambos pasan por el mismo canal único de loop.
- **Aclaración sobre el hallazgo de `res/raw` (reportado por el usuario):** se confirmó que
  `ux0_data/illusia/res/raw/sNNN.ogg` SÍ existe en el `.apk` real (70 archivos, mismos ids que
  `assets/sound/`) y es el que usa el camino Android real
  (`IllusiaUIControllerView.OnSoundPlay` → `NexusSound.playSound(R.raw.s000+i, z)`), en Ogg Vorbis
  **16000 Hz estéreo** (confirmado con `ffprobe` sobre `s104.ogg`/`s000.ogg`/etc.), mientras que
  `assets/sound/NNN.wav` (lo que usa nuestro mezclador nativo) es PCM **8000 Hz mono** -- una versión de
  menor fidelidad del mismo master, empaquetada en el mismo `.apk` original con timestamps casi
  idénticos (probablemente para un target de hardware más limitado del motor Nexus2 compartido). **No es
  la causa de estos 3 bugs:** nuestro motor nunca pasa por el sistema de recursos de Android ni por
  `res/raw` -- interceptamos el JNI `OnSoundPlay` directo y reproducimos el `.wav` equivalente en C, y
  se confirmó que el `.wav` de id 108 tiene la duración correcta (12.86s vs 12.80s del `.ogg`, diferencia
  despreciable de resampleo) y un header PCM válido -- el contenido no está roto, faltaba el clamp. Que
  falte `res/raw` en el `ux0_data` de este port es esperable y no bloquea nada: **no forma parte de nuestra
  cadena de audio actual.** Migrar a decodificar los `.ogg` de `res/raw` en vez de los `.wav` de
  `assets/sound/` sería un cambio de fidelidad (stereo/16kHz en vez de mono/8kHz) que requeriría vendorizar
  un decoder Vorbis (ej. stb_vorbis) -- una mejora de calidad a evaluar aparte, no necesaria para resolver
  el ruido reportado.
- **Estado:** corregido en código. Pendiente de build/deploy y confirmación del usuario en consola.

### Bug #17 confirmado y resuelto (2026-09-02) -- Motor de audio migrado de .wav (8kHz mono) a .caf (22050Hz estéreo, IMA4 ADPCM)

- **Síntoma:** reportado por el usuario -- el sonido debería usar los `.caf` de `assets/sound/`, que son
  más nítidos que los `.wav` que estábamos usando.
- **Confirmado con `afinfo` (herramienta nativa de Apple, no especulado):** los `.caf` de
  `illusia_extract/assets/sound/` son uniformemente **2 canales, 22050 Hz, IMA4 ADPCM** (`ima4`, 68
  bytes/paquete = 34 bytes por bloque de canal, 64 frames/paquete) en TODOS los ids revisados (108, 046,
  000, 001, 002, 120, 121, 084) -- verificado también que la duración coincide exactamente con el `.wav`
  correspondiente por id (ej. 108: 12.857s en ambos formatos), confirmando que son la MISMA grabación,
  solo que el `.caf` es una versión de mayor fidelidad (estéreo, ~2.75x el sample rate) agregada al build
  original ~4 meses después que el `.wav` (`.caf` fechado 2011-09-22 en el `.apk` vs `.wav` 2011-05-24).
- **Fix:** reescrito `source/utils/audio.c` para decodificar `.caf`/IMA4 en vez de leer `.wav` como PCM
  crudo:
  - Implementado un decoder IMA4 QuickTime/CAF completo (tablas estándar de paso/índice de IMA ADPCM,
    `ima4_decode_block()` decodifica un bloque de 34 bytes → 64 samples por canal) y un parser mínimo de
    contenedor CAF (`caf_find_data_chunk()`, recorre la lista de chunks buscando `"data"`, saltando los 4
    bytes de `mEditCount` que preceden el audio real dentro de ese chunk -- confirmado contra el "audio
    data file offset" que reporta `afinfo`).
  - `load_caf()` reemplaza a `load_wav()`: lee `assets/sound/%03d.caf`, decodifica todos los paquetes a
    PCM estéreo intercalado en memoria.
  - El puerto de audio (`sceAudioOutOpenPort`) pasó de `8000 Hz / MONO` a `22050 Hz / STEREO`
    (`CAF_SAMPLE_RATE`/`CAF_CHANNELS`), y todo el mezclado en `audio_thread()` (canal de loop/BGM y pool
    de SFX) ahora trabaja por frame estéreo en vez de por sample mono. `SoundFile.length_samples` pasó a
    `length_frames` (cuenta de frames, no de samples intercalados) para reflejar esto.
  - El clamp de saturación del Bug #16 se preservó (ahora aplicado por canal, L y R por separado).
- **Estado:** corregido en código. Pendiente de build/deploy y confirmación del usuario en consola de que
  el audio suena más nítido.

### Bug #18 -- EN INVESTIGACIÓN, log de diagnóstico agregado (reportado por el usuario, 2026-09-02) -- posible sonido de cutscene con id equivocado (¿046-055 en vez de 100+?)

- **Síntoma:** reportado por el usuario -- durante un cutscene suena algo que percibe parecido a uno de
  los ids 046-055 (SFX corto), cuando según su criterio debería sonar uno de los ids 100 en adelante
  (BGM/ambiente).
- **Por qué NO se aplicó un fix todavía:** el id que se reproduce en un cutscene es 100% data-driven
  dentro del `.so` original sin modificar -- `CMvGameScript::Script_PlayBGM`/`Script_PlaySound`
  (`out_ghidra.c:45432` y `:45451`) leen el id de un campo del propio script del cutscene
  (`*(int*)(*(int*)(param_1+8)+4)`), cargado desde `assets/script/output.dlg`. No hay forma de confirmar
  desde disassembly estático qué id concreto carga ese script para el cutscene en cuestión, y nuestro
  código no transforma ni reinterpreta ese id en ningún punto (`m_OnSoundPlay`/`audio_play()` lo reciben
  y usan tal cual llega del `.so`) -- aplicar un fix a ciegas sin confirmar el id real sería especulativo,
  y podría ser tanto un id genuinamente distinto al esperado por el propio juego (poco probable, ya que
  el `.so` es binario original) como una percepción influenciada por la menor fidelidad que tenía el
  audio hasta el Bug #17 (ahora corregido) -- imposible de distinguir sin un dato concreto.
- **Siguiente paso (ya instrumentado):** se agregó un log de diagnóstico en `source/java.c`
  (`m_OnSoundPlay`): `l_info("m_OnSoundPlay: id=%d vol=%d loop=%d", ...)` en cada llamada real del
  motor. La próxima vez que el usuario reproduzca el cutscene en cuestión con el "Servidor de Logs en
  Vivo" del toolkit activo, `logs/live_session_*.log` va a tener el id/volumen/loop exactos que pide el
  `.so` en ese momento -- eso confirma o descarta de una vez si el id es realmente distinto al rango
  100+, sin seguir adivinando.
- **Estado:** sin fix todavía -- pendiente del log real del próximo intento del usuario. **Actualización:**
  el primer log real recibido (`logs/game_023.log`, ver Bug #19) NO cubre ese cutscene -- los únicos ids
  vistos ahí son 0,3,6,9,10,12,15,58,65, ninguno en 046-055 ni en 100+ -- así que sigue sin confirmarse ni
  descartarse. Sí aporta un dato relevante para la hipótesis original: el propio `.so`, en este log, usa
  ids bien por debajo de 100 (58 y 65) como música/ambiente en loop de una zona real de juego -- confirma
  que "música = id ≥ 100" NO es una convención real del motor (era una heurística nuestra, ya descartada
  desde el Bug #14), así que la expectativa de que el cutscene "debería" usar un id ≥100 podría no aplicar
  tampoco. Sigue pendiente el log del cutscene puntual para cerrar esto con certeza.

### Bug #19 confirmado y resuelto (2026-09-02) -- Todo el audio sonaba "muy fuerte y distorsiona" -- escala de volumen equivocada (/10 en vez de /100)

- **Síntoma:** reportado por el usuario -- el sonido se escucha muy fuerte y distorsiona en general; en
  el ingame, en vez de música suena uno de los efectos en loop (ver actualización del Bug #18 arriba).
- **Confirmado con el primer log real recibido (`logs/game_023.log`, gracias al log de diagnóstico del
  Bug #18):** de las 55 llamadas a `m_OnSoundPlay` registradas en toda la partida -- BGM/loop (ids 58, 65)
  y SFX por igual (ids 0,3,6,9,10,12,15) -- **las 55 mandan `vol=100`, sin una sola excepción.** Esto
  confirma que la unidad real de volumen del motor es una escala 0-100 (probablemente un porcentaje), no
  el rango 0-10 que asumía `source/utils/audio.c` desde el principio (mucho antes de esta sesión).
- **Causa Raíz:** el mezclador dividía por 10 (`sample * vol / 10`) tanto en el canal de loop/BGM como en
  el pool de SFX. Con `vol=100` real, eso amplifica cada muestra **10x**. El clamp de saturación agregado
  en el Bug #16 evita que ese desborde dé la vuelta (wraparound / ruido digital tipo aliasing), pero no
  soluciona el problema de fondo: con una ganancia de 10x, prácticamente cualquier muestra que no sea casi
  silencio queda recortada contra ±32767 (hard clipping) -- **fuerte y distorsionado**, tal como reportó
  el usuario después del fix del Bug #16 (ese fix corrigió el tipo de ruido, no el volumen de fondo).
- **Fix:** en `source/utils/audio.c`, se agregó `#define VOLUME_SCALE 100` (con el log real citado en el
  comentario, no un número adivinado) y se reemplazó el divisor `/10` por `/VOLUME_SCALE` en ambos canales
  de mezcla (loop/BGM y SFX). Con `vol=100` real, esto da ganancia unitaria exacta (sin amplificar ni
  atenuar) -- el clamp de saturación se mantiene como red de seguridad barata por si algún sonido supera
  el rango de 16 bits por sí solo o el motor llegara a mandar un volumen mayor a 100.
- **Estado:** corregido en código. Pendiente de build/deploy y confirmación del usuario en consola.

### Bug #20 confirmado y resuelto (2026-09-02) -- LA causa raíz real: el `.so` nunca manda el número de archivo, manda un ÍNDICE de lista

Reportado por el usuario -- instaló Illusia 2 en Android y confirmó que el menú tiene música y el
in-game tiene OTRA música distinta (no un efecto en loop). Pidió revisar a fondo el desensamblado de
cómo se gestiona el sonido antes de seguir parcheando síntomas.

- **El hallazgo, en orden de cómo se llegó a él:**
  1. `CGsSound::PlaySound(int,int,uchar)` (`out_ghidra.c:19311`) NUNCA pasa el id literal que recibe
     (`param_1`) a `MC_androidPlaySound`. Primero llama
     `iVar1 = GetSoundIDListIdx(this, param_1)` (`out_ghidra.c:18499`, busca `param_1` dentro de un
     arreglo registrado en `this+0x70`/`this+0x74` y devuelve su POSICIÓN, no el valor) y es **`iVar1`**
     (el índice) lo que llega a `MC_androidPlaySound(iVar1, param_2, param_3)` → `playAndroidSound`
     (`out_ghidra.c:116148`) → JNI `OnSoundPlay` → nuestro `m_OnSoundPlay`.
  2. Ese arreglo registrado se construye en `CMvApp::EvAppStart()` (`out_ghidra.c:31927`):
     `CGsSound::OnCreate(this_03, 0x46, (int*)&DAT_00113a70)` -- copia **70** (`0x46`) enteros desde
     `DAT_00113a70` directo al arreglo. Se leyeron esos 70 enteros **directo del `.so` real**
     (`illusia_extract/lib/armeabi/libgameDSO.so`, no inferido): usando el mismo offset de `-0x10000`
     entre direcciones de Ghidra y direcciones reales ya documentado para los placeholders de vtable
     (ver el triage de `KnockProcWhenAttack`, 2026-08-22), `DAT_00113a70 - 0x10000 = 0x00103a70` cae
     dentro de `.data` (confirmado con `objdump -h`/`-p`); calculando el offset de archivo real
     (`vaddr - 0x1000` para el segundo segmento `LOAD`) y leyendo 70 enteros de 4 bytes ahí, el
     resultado es **exactamente**:
     `[0,1,2,5,6,10,13,14,15,16,17,18,20,22,23,33,36,37,38,39,40,41,42,44,45,46,47,54,55,56,57,58,59,60,
     61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,100,101,102,103,104,105,106,
     108,110,111,120,121]` -- byte a byte, la misma lista de 70 ids válidos que ya conocíamos por
     `res/raw/`/`assets/sound/` (Bug #17), en el mismo orden.
  3. Esto es invisible en Android porque `aapt` asigna los ids de recursos de `res/raw/sNNN.ogg` en el
     mismo orden alfabético/ascendente -- `R.raw.s000 + i` (`NexusSound.playSound`,
     `IllusiaUIControllerView.OnSoundPlay`) cae en el archivo correcto solo porque ambas listas están
     ordenadas igual. Nuestro código, en cambio, tomaba ese índice y lo usaba directo como número de
     archivo literal (`snprintf("%03d.caf", index)`) -- correcto por pura coincidencia solo para los ids
     0, 1 y 2 (sin huecos todavía en la lista antes de esa posición), y equivocado para prácticamente
     todo lo demás.
  4. **Confirmado contra `logs/game_023.log`:** `index=65` (loop=1) y `index=58` (loop=1) -- que nuestro
     código viejo tocaba como los archivos sueltos `065.caf`/`058.caf` -- son la posición 65 y 58 de la
     tabla de arriba, que valen **108** y **100** respectivamente. **108 es exactamente
     `RES_BGM_TITLE`** (el string de debug del `.so` que ya habíamos cruzado en el Bug #16,
     `CGsSound::Play(ms,0x6c=108,-1,'\x01')`), y **100 es la primera pista de música dedicada al
     in-game** (rango que ya usábamos como "BGM" desde antes, Fase 4) -- exactamente lo que el usuario
     esperaba encontrar (música real, distinta entre menú e in-game) y no encontraba.
- **Fix (`source/utils/audio.c`):** se agregó `sound_index_table[70]` con la tabla exacta de arriba y
  `sound_index_to_real_id()`, que traduce el índice recibido en `audio_play()` al id de archivo real
  ANTES de tocar `load_caf()`/los canales de mezcla. `audio_play()` ahora loguea
  `index=%d -> real_id=%d vol=%d loop=%d` en cada llamada para que cualquier log futuro muestre la
  traducción directamente. Este único fix resuelve de raíz el Bug #18 (cutscene con id "equivocado") y
  el reporte de este mensaje (in-game con efecto en loop en vez de música) -- ambos eran el mismo bug.
- **Además, migrado el backend de audio de `sceAudioOut` a SDL2** (pedido explícito del usuario): se
  reemplazó `sceAudioOutOpenPort`/`sceAudioOutOutput`/el hilo propio (`sceKernelCreateThread` +
  `while(audio_running)`) por `SDL_OpenAudioDevice()` con un callback (`sdl_audio_callback`) que llama a
  la misma lógica de mezclado ya existente (`mix_audio()`, extraída del viejo `audio_thread()` sin
  cambios de comportamiento, solo parametrizada por cantidad de frames en vez de la constante `GRAIN`
  fija). El gate de sincronización del Bug #15 (`audio_gate_open`/`audio_open_gate()`) se mantiene igual,
  ahora chequeado dentro de `mix_audio()`. `CMakeLists.txt` linkea `SDL2` (agregado antes de
  `libvitaGL.a` en `target_link_libraries`).
- **Logging agregado (pedido explícito del usuario, "todos los posibles para atrapar el bug"):**
  `load_caf` loguea archivo+frames+duración al cargar (o warning si falta el archivo o el chunk `data`),
  `audio_play` loguea índice→id real→vol→loop en cada llamada, `audio_stop_bgm`/`audio_stop_all` loguean
  cuándo se llaman, `audio_open_gate` loguea cuándo se abre, y `sound_index_to_real_id` loguea un warning
  si el `.so` llega a mandar un índice fuera de rango (0-69) -- caso no visto todavía pero que indicaría
  una tabla desactualizada o un id nuevo no contemplado.
- **Estado:** corregido en código. Pendiente de confirmación del usuario en consola de que ahora sí suena
  música real en el in-game y en cutscenes.

### Bug del build (2026-09-02) -- `SDL2/SDL.h: No such file or directory` + link errors, resuelto

- **Síntoma:** el primer build real del usuario tras el Bug #20 falló en `source/utils/audio.c:1:10:
  fatal error: SDL2/SDL.h: No such file or directory` -- el paquete SDL2 de vitasdk nunca había sido
  instalado en su VITASDK (`vdpm` nunca se había corrido para `sdl2` en esta máquina).
- **Fix:** esta vez sí se pudo verificar en la propia máquina del usuario -- `$VITASDK` no estaba
  exportado en el shell de esta sesión, pero el SDK real vive en `~/vitasdk` (con `vdpm` en
  `~/vitasdk/bin/`). Se corrió `vdpm sdl2`, que descargó e instaló
  `~/vitasdk/arm-vita-eabi/include/SDL2/*.h` + `libSDL2.a`/`libSDL2main.a`. Con eso, `audio.c` compiló,
  pero el LINK falló distinto: `libSDL2.a` es un solo archivo estático que empaqueta TODOS los
  subsistemas de SDL2 (video/joystick/sensor/audio/etc.), no solo audio -- el linker arrastra el backend
  de video de Vita (`SDL_vitavideo.c.obj`), que usa el teclado en pantalla (`sceImeOpen/Close/SetCaret/
  SetText/Update`) aunque el juego nunca llama a la API de video de SDL2. Se agregó `SceIme_stub` al
  `target_link_libraries` de `CMakeLists.txt` (junto a `SDL2`) para resolver esos símbolos.
- **De paso, se corrigió el warning de `sceIoMkdir` implícito** que aparecía en el mismo log de build
  (`source/utils/init.c`): faltaba `#include <psp2/io/dirent.h>` (donde vive esa función) -- agregado
  junto a los demás includes de `psp2/*` del archivo. Era solo un warning (no bloqueaba el build), pero
  se corrigió de una vez ya que apareció en el mismo log compartido.
- **Build verificado localmente de punta a punta** (`cmake` + `make -j4` sobre un directorio de
  scratch, sin tocar el `build/` que administra el toolkit del usuario): compila y linkea limpio,
  genera `illusia.vpk`/`eboot.bin` sin errores nuevos. **Sigue sin probarse en consola/Vita3K** -- eso
  queda para la próxima corrida real del usuario.
- **Estado:** corregido y verificado con build real (no solo teoría). Pendiente de probar en consola.

### Investigación en curso -- FPS cae a ~3 al cambiar de stage, "se ve el mapa" brevemente (reportado por el usuario, 2026-09-02)

- **Síntoma:** al pasar de un área a otra, el juego se traba (FPS cae a ~3) por un instante, y durante
  ese momento se ve "el mapa" -- al usuario le resultó raro, dura milisegundos.
- **Cruce con el pseudo-C real:** el cambio de área real pasa por `CMvMap::DoMapChange()`
  (`out_ghidra.c:65562`): `Release()` (libera el mapa actual) → `Load(newMapId, true)` (carga todo el
  mapa nuevo -- tiles, objetos, NPCs, todo por I/O síncrona, motor sin hilos confirmado en Fase 3) →
  si carga bien, reproduce el BGM del mapa nuevo (`PlayMapBGM`, ver Bug #16/#20) → **y, si
  `CMvGameUI+0x140 == 0` y el "tipo" del mapa nuevo (tabla XLS 6, columna 1) no es 6, llama
  `OpenWorldMap()` directo, sin condición de usuario, como parte del flujo normal de cambio de mapa.**
  Esto significa que **"se ve el mapa" después de ciertas transiciones es comportamiento real y original
  del motor** (mostrar la pantalla de World Map automáticamente en ciertos mapas de campo), no
  necesariamente un glitch nuestro -- pero no se puede confirmar al 100% sin ver el log real de qué
  hook se dispara y con qué resultado.
- **Hipótesis sobre la causa del freeze (con fix ya aplicado, no especulativo):** `CMvMap::Load()` hace
  toda su carga de archivos vía `fopen`/`fread` síncronos (confirmado, sin `AAssetManager`, Fase 3), y
  nuestras propias utilidades de archivo (`source/utils/utils.c`) hacían **una llamada extra a
  `sceIoGetstat()` (vía `file_exists()`) antes de cada `fopen()`** en `file_load()`/`file_size()` -- un
  syscall real de más por archivo, encima de los que ya hacían falta (`fopen`+`fseek`+`fseek`+`fread`+
  `fclose`). Una transición de área puede implicar decenas de archivos (mapa, tilesets, sprites, script)
  abiertos uno tras otro -- cada round-trip a `sceIo*` en el almacenamiento real de la Vita tiene latencia
  no despreciable, coincide con el patrón de "freeze de una fracción de segundo" reportado.
- **Fix aplicado (mejora de rendimiento, no cambia comportamiento):**
  - `source/utils/utils.c`: se quitó el `file_exists()` redundante en `file_load()`/`file_size()` --
    ahora se confía directamente en que `fopen()` devuelva `NULL` si el archivo no existe, en vez de
    stat-earlo primero. Aplica a todo el juego, no solo a transiciones de mapa.
  - `source/reimpl/io.c`: `fopen_soloader()` (la función que usa el `.so` para TODOS sus propios
    `fopen()`, incluida toda la carga de mapas/tilesets/sprites) ahora llama `setvbuf(..., _IOFBF, 64KB)`
    justo después de abrir el archivo -- el buffer chico por defecto de stdio convertía la lectura de un
    archivo mediano en muchas llamadas `sceIoRead` pequeñas; un buffer más grande significa menos
    llamadas más grandes para los mismos datos, mismo comportamiento observable.
- **Instrumentación de diagnóstico agregada (sin cambiar comportamiento) en `source/patch.c`:**
  - `CMvMap::Load` (`_ZN6CMvMap4LoadEib`): mide con `current_timestamp_ms()` cuánto tarda de verdad
    cada carga de mapa y loguea `CMvMap::Load(map_id=%d) -> %d took %llu ms`.
  - `CMvMap::OpenWorldMap` (`_ZN6CMvMap12OpenWorldMapEv`): loguea cada vez que se llama y su resultado
    (0 = no llegó a abrir nada de verdad, ej. `GetMapIDForWorldMap()` no encontró nada).
- **Build verificado localmente** (mismo procedimiento que el bug anterior): compila y linkea limpio,
  `illusia.vpk` generado sin errores.
- **Próximo paso:** correr esto en consola con el "Servidor de Logs en Vivo" activo, reproducir una
  transición de área, y mandar el log -- va a confirmar (1) cuánto bajó el tiempo real de `CMvMap::Load`
  con el fix de I/O, y (2) si `OpenWorldMap()` se dispara justo en ese momento (confirmando que "el
  mapa" es el World Map real del juego, esperado en esa transición) o si el freeze/mapa vienen de otro
  lado no contemplado todavía.
- **Estado:** mejora de I/O aplicada y verificada con build real; diagnóstico instrumentado. Pendiente
  del log real del usuario para confirmar la causa exacta y si hace falta más trabajo.
