#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"

#include <stddef.h>

#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

// El default del SDK para el stack del hilo principal (256 KB, ver
// samples/basic_program) es chico para este motor: el combate real (CMvMob
// AI -> CMvCharacter/CMvPlayer, todo con dispatch virtual anidado) puede
// meter muchos más niveles de llamada por frame que el boot/render inicial.
// Crash confirmado en consola con vtable dispatch saltando a memoria
// heap/no mapeada dentro de CMvMob::KnockProcWhenAttack durante combate
// real (ver port_progress.md, dump psp2core-1787381851) -- consistente con
// overflow de stack corrompiendo memoria adyacente en vez de un dato de
// diagnóstico claro. Subir el stack es inocuo (solo reserva más RAM) y es
// la mitigación estándar para este síntoma en ports de motores C++
// Android->Vita; no reemplaza confirmar la causa con un log real (pendiente,
// ver bitácora).
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;

so_module so_mod;

// Fixed game-logic resolution used by the engine internally
// (com.gamevil.illusia.IllusiaLauncher.onCreateIllusiaLauncher() hardcodes
// gameScreenWidth/Height to 480x320 regardless of the real display metrics.
// See port_progress.md Fase 3 for the full Activity -> Natives.* mapping.)
#define GAME_SCREEN_WIDTH  480
#define GAME_SCREEN_HEIGHT 320

// Real EGL surface size we render into; vitaGL is initialized at native
// resolution in gl_init() (utils/glutil.c).
#define SURFACE_WIDTH  960
#define SURFACE_HEIGHT 544

int main() {
    soloader_init_all();

    int (* JNI_OnLoad)(void * vm) = (void *) so_symbol(&so_mod, "JNI_OnLoad");
    JNI_OnLoad(&jvm);

    // com.gamevil.nexus2.NexusGLActivity.onResume()
    void (* Natives_InitializeJNIGlobalRef)(void * env, void * clazz) =
            (void *) so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_InitializeJNIGlobalRef");

    // com.gamevil.nexus2.NexusGLRenderer.surfaceCreated()
    void (* Natives_NativeInitWithBufferSize)(void * env, void * clazz, int w, int h) =
            (void *) so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeInitWithBufferSize");

    // com.gamevil.nexus2.NexusGLRenderer.surfaceChanged()
    void (* Natives_NativeInitDeviceInfo)(void * env, void * clazz, int w, int h) =
            (void *) so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeInitDeviceInfo");
    void (* Natives_NativeResize)(void * env, void * clazz, int w, int h) =
            (void *) so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeResize");

    // com.gamevil.nexus2.NexusGLRenderer.drawFrame(), called once per frame
    void (* Natives_NativeRender)(void * env, void * clazz) =
            (void *) so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_NativeRender");

    gl_init();

    Natives_InitializeJNIGlobalRef(&jni, NULL);
    Natives_NativeInitWithBufferSize(&jni, NULL, GAME_SCREEN_WIDTH, GAME_SCREEN_HEIGHT);
    Natives_NativeInitDeviceInfo(&jni, NULL, GAME_SCREEN_WIDTH, GAME_SCREEN_HEIGHT);
    Natives_NativeResize(&jni, NULL, SURFACE_WIDTH, SURFACE_HEIGHT);

    void (* Natives_handleCletEvent)(void * env, void * clazz, int event, int param1, int param2, int pointerId) =
            (void *) so_symbol(&so_mod, "Java_com_gamevil_nexus2_Natives_handleCletEvent");

    l_success("Entering the render loop.");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    SceCtrlData pad;
    SceTouchData touch;
    uint32_t old_buttons = 0;
    int lastX[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int lastY[SCE_TOUCH_MAX_REPORT] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t active_ids[SCE_TOUCH_MAX_REPORT] = {0};

    struct {
        uint32_t mask;
        int code;
    } buttons[] = {
        { SCE_CTRL_UP, -1 },
        { SCE_CTRL_DOWN, -2 },
        { SCE_CTRL_LEFT, -3 },
        { SCE_CTRL_RIGHT, -4 },
        { SCE_CTRL_CROSS, -5 },
        { SCE_CTRL_CIRCLE, -8 },
        { SCE_CTRL_SQUARE, -16 }, // Clear / attack?
        { SCE_CTRL_TRIANGLE, -6 }, // Softkey / menu?
        { SCE_CTRL_START, -7 },
        // Raw GxKey 0x23 ('#'/MH_KEY_POUND) is the ONLY code that
        // CGsInputKey::GxKey2GsKey() translates to GsVKey 0xb, which is the
        // one CMvGameScriptMgr::OnKeyPress() checks to call
        // CMvGameScript::DoSkip() (see decompiled/.../out_ghidra.c:10011 and
        // :10111). Scripted cutscenes (as opposed to plain dialogue boxes,
        // which do respond to taps via UIFullTouch) only listen for this key
        // -- no physical button or touch tap reaches it otherwise, which is
        // why a scripted cutscene could get stuck with no visible way to
        // advance it. Mapped to SELECT (previously a duplicate of CIRCLE's
        // -8, unused) as a dedicated "skip cutscene" button.
        { SCE_CTRL_SELECT, 0x23 },
    };

    uint32_t last_tick = sceKernelGetProcessTimeLow();
    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

        if (pad.ly < 64) pad.buttons |= SCE_CTRL_UP;
        if (pad.ly > 192) pad.buttons |= SCE_CTRL_DOWN;
        if (pad.lx < 64) pad.buttons |= SCE_CTRL_LEFT;
        if (pad.lx > 192) pad.buttons |= SCE_CTRL_RIGHT;

        uint32_t changed_buttons = pad.buttons ^ old_buttons;
        for (int i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (changed_buttons & buttons[i].mask) {
                if (pad.buttons & buttons[i].mask) {
                    Natives_handleCletEvent(&jni, NULL, 2, buttons[i].code, 0, 0); // Key Down
                } else {
                    Natives_handleCletEvent(&jni, NULL, 3, buttons[i].code, 0, 0); // Key Up
                }
            }
        }
        old_buttons = pad.buttons;

        int current_touches[SCE_TOUCH_MAX_REPORT] = {0};

        for (int i = 0; i < touch.reportNum; i++) {
            uint32_t id = touch.report[i].id;
            int slot = -1;
            // Find existing slot
            for (int j = 0; j < SCE_TOUCH_MAX_REPORT; j++) {
                if (active_ids[j] == id) {
                    slot = j;
                    break;
                }
            }
            // If not found, find empty slot
            if (slot == -1) {
                for (int j = 0; j < SCE_TOUCH_MAX_REPORT; j++) {
                    if (active_ids[j] == 0) {
                        active_ids[j] = id;
                        slot = j;
                        break;
                    }
                }
            }
            if (slot != -1) {
                int x = (touch.report[i].x * GAME_SCREEN_WIDTH) / 1920;
                int y = (touch.report[i].y * GAME_SCREEN_HEIGHT) / 1088;
                current_touches[slot] = 1;
                
                if (lastX[slot] == -1 || lastY[slot] == -1) {
                    Natives_handleCletEvent(&jni, NULL, 23, x, y, slot); // Touch Down
                } else if (lastX[slot] != x || lastY[slot] != y) {
                    Natives_handleCletEvent(&jni, NULL, 25, x, y, slot); // Touch Move
                }
                
                lastX[slot] = x;
                lastY[slot] = y;
            }
        }

        for (int slot = 0; slot < SCE_TOUCH_MAX_REPORT; slot++) {
            if (!current_touches[slot]) {
                if (lastX[slot] != -1 && lastY[slot] != -1) {
                    Natives_handleCletEvent(&jni, NULL, 24, lastX[slot], lastY[slot], slot); // Touch Up
                    lastX[slot] = -1;
                    lastY[slot] = -1;
                    active_ids[slot] = 0;
                }
            }
        }

        Natives_NativeRender(&jni, NULL);
        gl_swap();
        
        uint32_t current_tick = sceKernelGetProcessTimeLow();
        uint32_t target_tick = last_tick + 33333;
        if (current_tick < target_tick) {
            sceKernelDelayThread(target_tick - current_tick);
        }
        last_tick = sceKernelGetProcessTimeLow();
    }

    sceKernelExitDeleteThread(0);
}
