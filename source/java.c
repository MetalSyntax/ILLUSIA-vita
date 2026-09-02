#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/utils.h"
#include "utils/logger.h"

/*
 * JNI Methods
 *
 * All names below were confirmed by cross-referencing GetStaticMethodID()
 * call sites in the decompiled .so (Ghidra) against the two Java classes
 * they actually target: com/gamevil/nexus2/Natives and
 * com/gamevil/nexus2/NexusFont (decompiled/apk_jadx/sources/com/gamevil/nexus2/).
 * See port_progress.md Fase 3 for the full analysis.
 *
 * A subset of the names looked up by the .so (GFA_GetTextWidths,
 * GFA_GetStringLength, GFA_SetString*, GFA_SetAntiAlias, GFA_SetLocale,
 * blinkSButton, showComponent/hideComponent, showMylSButton, requestIAP,
 * netBillcomSocket*, netSocket) do NOT exist in this game's actual Java
 * classes -- they are dead code paths inherited from the shared Gamevil
 * Nexus2 engine (other titles built on it use them; Illusia's Java never
 * implements them, so on real Android they would never be reachable
 * without crashing with NoSuchMethodError). They are still registered here
 * with safe no-op/neutral-value stubs, in case some code path we haven't
 * exercised yet does reach them.
 *
 * IMPORTANT: JNI Call*Method(...) is a C varargs call, so `float` arguments
 * are promoted to `double` by the C calling convention before they reach
 * our va_list. Always read them with va_arg(args, jdouble), never
 * va_arg(args, jfloat).
 */

#define ENV (&jni)

/*
 * ---- NexusFont (GFA_*) stub state -----------------------------------
 *
 * Real text rendering (glyph rasterization) is NOT implemented yet -- see
 * port_progress.md Fase 3 "RIESGO ALTO". This is intentionally scoped out
 * of the loader bootstrap; it needs its own dedicated pass (Fase 6,
 * Graficos/UI) with stb_truetype, following the `text_rendering.md`
 * reference from the psvita-porting skill. For now we track just enough
 * state to answer layout queries (widths/heights/current font/color)
 * without crashing, and hand back a blank (transparent) pixel buffer so
 * any subsequent glTexImage2D upload has a valid, if invisible, source.
 */
static int      gfa_inited = 0;
static int      gfa_w = 0, gfa_h = 0, gfa_bpp = 32, gfa_colorkey = 0;
static void   * gfa_pixels = NULL;
static int      gfa_font_count = 0;
static int      gfa_cur_font = -1;
static int      gfa_color = 0xFF000000;
static float    gfa_text_size = 12.0f;

static int gfa_char_width(void)  { return (int) (gfa_text_size * 0.5f); }
static int gfa_char_height(void) { return (int) gfa_text_size; }

/*
 * ---- Small helpers ----------------------------------------------------
 */

// Reads the UTF-8 bytes out of a jstring produced by the fake JVM.
// Never returns NULL; falls back to "" so callers can strlen()/snprintf()
// unconditionally.
static const char * jni_utf8(jstring str) {
    if (!str) return "";
    JavaString * s = (JavaString *) str;
    if (jstr_utf16_to_utf8(s) == JNI_FALSE) return "";
    return (const char *) s->utf8->array;
}

static jobject new_byte_array_from_str(const char * s) {
    size_t len = strlen(s);
    JavaDynArray * jda = jda_alloc((jsize) len, FIELD_TYPE_BYTE);
    if (!jda) return NULL;
    memcpy(jda->array, s, len);
    return (jobject) jda;
}

/*
 * ---- com.gamevil.nexus2.Natives : void methods ------------------------
 */

static void m_OnAsyncTimerSet(jmethodID id, va_list args) {
    // Native-side async timer (network timeouts, etc). Not wired up yet;
    // NativeAsyncTimerCallBack is never fired back. Revisit if a real
    // bug shows a feature stuck waiting on this.
}

static void m_OnEvent(jmethodID id, va_list args) {}
void audio_play(int id, int vol, int loop);
void audio_stop_bgm();
void audio_stop_all();

static void m_OnSoundPlay(jmethodID id, va_list args) {
    int i = va_arg(args, int);
    int i2 = va_arg(args, int);
    int z = va_arg(args, int);
    // `i` here is NOT the assets/sound/%03d filename number -- it's the
    // position of the real id inside CGsSound's internal registered-id list
    // (CGsSound::PlaySound/GetSoundIDListIdx, out_ghidra.c:19311/18499).
    // See the confirmed translation table and its full explanation in
    // audio_play()/audio.c (port_progress.md Bug #20) -- audio_play() logs
    // the resolved real id right after this.
    l_info("m_OnSoundPlay: index=%d vol=%d loop=%d", i, i2, z);
    if (i2 == 0 && z) {
        audio_stop_bgm();
    } else {
        audio_play(i, i2, z);
    }
}
static void m_OnStopSound(jmethodID id, va_list args) {
    audio_stop_all();
}
static void m_OnUIStatusChange(jmethodID id, va_list args) {}
static void m_OnVibrate(jmethodID id, va_list args) {}
static void m_SetSpeed(jmethodID id, va_list args) {}
static void m_clearPlayerName(jmethodID id, va_list args) {}
static void m_hideLoadingDialog(jmethodID id, va_list args) {}
static void m_hideTitleComponent(jmethodID id, va_list args) {}
static void m_openUrl(jmethodID id, va_list args) {}
static void m_openStoreWithProductId(jmethodID id, va_list args) {}

// Bug: game could not save progress. The .so calls back into Java's
// Natives.saveFile(String, byte[]) to persist save data, but that method
// was never registered in nameToMethodId -- GetStaticMethodID() returned
// NULL and methodVoidCall() silently no-op'd (logged a warning, no crash).
static void m_saveFile(jmethodID id, va_list args) {
    jstring name = va_arg(args, jstring);
    JavaDynArray * data = (JavaDynArray *) va_arg(args, jbyteArray);

    char path[512];
    snprintf(path, sizeof(path), "%ssaves/%s", DATA_PATH, jni_utf8(name));
    l_info("m_saveFile: saving %s (%d bytes)", path, data ? data->len : 0);

    if (!data || !file_save(path, data->array, data->len)) {
        l_error("m_saveFile: failed to save %s", path);
    }
}

static void m_saveGLOptionLinear(jmethodID id, va_list args) {}
static void m_showLoadingDialog(jmethodID id, va_list args) {}
static void m_showTitleComponent(jmethodID id, va_list args) {}
static void m_trackEventDispatch(jmethodID id, va_list args) {}
static void m_trackPageViewDispatch(jmethodID id, va_list args) {}

// Dead code (see file header comment): never reachable from Illusia's own
// Java classes, registered defensively as harmless no-ops.
static void m_blinkSButton(jmethodID id, va_list args) {}
static void m_showComponent(jmethodID id, va_list args) {}
static void m_hideComponent(jmethodID id, va_list args) {}
static void m_showMylSButton(jmethodID id, va_list args) {}
static void m_requestIAP(jmethodID id, va_list args) {}

static void m_GFA_Release(jmethodID id, va_list args) {
    if (gfa_pixels) {
        free(gfa_pixels);
        gfa_pixels = NULL;
    }
    gfa_inited = 0;
    gfa_font_count = 0;
    gfa_cur_font = -1;
}

static void m_GFA_ReleaseFont(jmethodID id, va_list args) {
    jint idx = va_arg(args, jint);
    if (gfa_cur_font == idx) gfa_cur_font = -1;
}

static void m_GFA_SetColor(jmethodID id, va_list args) {
    gfa_color = va_arg(args, jint);
}

static void m_GFA_SetTextAlign(jmethodID id, va_list args) {}
static void m_GFA_SetAntiAlias(jmethodID id, va_list args) {} // dead code
static void m_GFA_SetLocale(jmethodID id, va_list args) {}    // dead code

static void m_GFA_SetTextSize(jmethodID id, va_list args) {
    gfa_text_size = (float) va_arg(args, jdouble); // promoted float -> double
}

/*
 * ---- com.gamevil.nexus2.Natives : int / boolean methods ---------------
 */

static jint m_getGLOptionLinear(jmethodID id, va_list args) {
    return 1; // GL_LINEAR filtering
}

static jint m_isAssetExist(jmethodID id, va_list args) {
    jstring name = va_arg(args, jstring);
    char path[512];
    snprintf(path, sizeof(path), "%sassets/%s", DATA_PATH, jni_utf8(name));
    l_info("m_isAssetExist: checking %s", path);
    return file_exists(path) ? (jint) file_size(path) : 0;
}

static jint m_isFileExist(jmethodID id, va_list args) {
    jstring name = va_arg(args, jstring);
    char path[512];
    snprintf(path, sizeof(path), "%ssaves/%s", DATA_PATH, jni_utf8(name));
    l_info("m_isFileExist: checking %s", path);
    return file_exists(path) ? (jint) file_size(path) : 0;
}

static jint m_isNetAvailable(jmethodID id, va_list args) {
    return -1; // offline: no network available
}

static jint m_netConnect(jmethodID id, va_list args) {
    return -1; // offline: no network available
}

// Dead code (see file header comment).
static jint m_netBillcomSocketWrite(jmethodID id, va_list args)   { return 0; }
static jint m_netBillcomSocketConnect(jmethodID id, va_list args) { return -1; }
static jint m_netBillcomSocketClose(jmethodID id, va_list args)   { return 0; }
static jint m_netSocket(jmethodID id, va_list args)               { return -1; }

static jint m_GFA_CharHeight(jmethodID id, va_list args) { return gfa_char_height(); }
static jint m_GFA_CharWidth(jmethodID id, va_list args)  { return gfa_char_width(); }

static jint m_GFA_CreateFont(jmethodID id, va_list args) {
    if (gfa_font_count >= 5) return -1;
    return gfa_font_count++;
}

static jint m_GFA_GetAscent(jmethodID id, va_list args)  { return (jint) (gfa_text_size * 0.8f); }
static jint m_GFA_GetColor(jmethodID id, va_list args)   { return gfa_color; }
static jint m_GFA_GetCurrentFont(jmethodID id, va_list args) { return gfa_cur_font; }
static jint m_GFA_GetDescent(jmethodID id, va_list args) { return (jint) (gfa_text_size * 0.2f); }
static jint m_GFA_GetStringLength(jmethodID id, va_list args) { return 0; } // dead code

static jint m_GFA_SetFont(jmethodID id, va_list args) {
    jint prev = gfa_cur_font;
    gfa_cur_font = va_arg(args, jint);
    return prev;
}

static jboolean m_GFA_Init(jmethodID id, va_list args) {
    if (gfa_inited) return JNI_TRUE;

    gfa_w = va_arg(args, jint);
    gfa_h = va_arg(args, jint);
    gfa_bpp = va_arg(args, jint);
    gfa_colorkey = va_arg(args, jint);

    size_t bytes = (size_t) gfa_w * gfa_h * (gfa_bpp == 16 ? 2 : 4);
    gfa_pixels = calloc(1, bytes ? bytes : 1);
    gfa_inited = 1;
    return JNI_TRUE;
}

static jboolean m_GFA_IsInitialized(jmethodID id, va_list args) {
    return gfa_inited ? JNI_TRUE : JNI_FALSE;
}

/*
 * ---- com.gamevil.nexus2.Natives : object methods -----------------------
 */

static jobject m_getAbsolueFilePath(jmethodID id, va_list args) {
    char path[512];
    snprintf(path, sizeof(path), "%ssaves", DATA_PATH);
    return jni->NewStringUTF(ENV, path);
}

static jobject m_getPhoneModel(jmethodID id, va_list args) {
    return new_byte_array_from_str("PSVita");
}

static jobject m_getPhoneNumber(jmethodID id, va_list args) {
    return new_byte_array_from_str("000000000000000");
}

static jobject m_getPlayerNameByte(jmethodID id, va_list args) {
    return new_byte_array_from_str("Traveler");
}

static jobject m_getVersion(jmethodID id, va_list args) {
    return jni->NewStringUTF(ENV, "1.0.0");
}

// Bug: game could not load a previously saved game. Same root cause as
// m_saveFile -- Natives.loadFile(String) was never registered either.
static jobject m_loadFile(jmethodID id, va_list args) {
    jstring name = va_arg(args, jstring);
    char path[512];
    snprintf(path, sizeof(path), "%ssaves/%s", DATA_PATH, jni_utf8(name));
    l_info("m_loadFile: loading %s", path);

    uint8_t * buf = NULL;
    size_t size = 0;
    if (!file_load(path, &buf, &size)) return NULL;

    // See m_readAssete below for why this isn't a plain jda_alloc().
    JavaDynArray * jda = malloc(sizeof(JavaDynArray) + size);
    if (jda) {
        jda->magic = 0xB00B1E5; // JDA_MAGIC
        jda->array = ((uint8_t*)jda) + sizeof(JavaDynArray);
        jda->len = size;
        jda->type = FIELD_TYPE_BYTE;
        memcpy(jda->array, buf, size);
    }
    free(buf);
    return (jobject) jda;
}

static jobject m_readAssete(jmethodID id, va_list args) {
    jstring name = va_arg(args, jstring);
    char path[512];
    snprintf(path, sizeof(path), "%sassets/%s", DATA_PATH, jni_utf8(name));
    l_info("m_readAssete: loading %s", path);

    uint8_t * buf = NULL;
    size_t size = 0;
    if (!file_load(path, &buf, &size)) return NULL;

    // Gamevil's engine bypasses JNI GetByteArrayElements and instead directly accesses
    // the Dalvik object memory layout by adding 0x10 to the jobject pointer.
    // To support this AND standard FalsoJNI calls, we allocate a contiguous block:
    // Header (0x10 bytes) + Data (size bytes). We set jda->array to point to the Data.
    JavaDynArray * jda = malloc(sizeof(JavaDynArray) + size);
    if (jda) {
        jda->magic = 0xB00B1E5; // JDA_MAGIC
        jda->array = ((uint8_t*)jda) + sizeof(JavaDynArray);
        jda->len = size;
        jda->type = FIELD_TYPE_BYTE;
        memcpy(jda->array, buf, size);
    }
    free(buf);
    return (jobject) jda;
}

// Dead code (see file header comment).
static jobject m_netBillcomSocketRead(jmethodID id, va_list args) {
    JavaDynArray * jda = jda_alloc(0, FIELD_TYPE_BYTE);
    return (jobject) jda;
}

static jobject m_GFA_DrawText(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    float x = (float) va_arg(args, jdouble);
    float y = (float) va_arg(args, jdouble);
    va_arg(args, jint); // draw length, unused by the stub
    float max_width = (float) va_arg(args, jdouble);

    jsize len = (jsize) strlen(jni_utf8(str));
    float width = (float) (len * gfa_char_width());
    if (width > max_width) width = max_width;

    JavaDynArray * jda = jda_alloc(4, FIELD_TYPE_FLOAT);
    if (jda) {
        jfloat * rect = (jfloat *) jda->array;
        rect[0] = x;
        rect[1] = y;
        rect[2] = width;
        rect[3] = (jfloat) gfa_char_height();
    }
    return (jobject) jda;
}

static jobject m_GFA_MeasureText(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    va_arg(args, jint); // substring length, unused by the stub
    float max_width = (float) va_arg(args, jdouble);

    jsize len = (jsize) strlen(jni_utf8(str));
    float width = (float) (len * gfa_char_width());
    if (width > max_width) width = max_width;

    JavaDynArray * jda = jda_alloc(2, FIELD_TYPE_FLOAT);
    if (jda) {
        jfloat * size = (jfloat *) jda->array;
        size[0] = width;
        size[1] = (jfloat) gfa_char_height();
    }
    return (jobject) jda;
}

static jobject m_GFA_GetPixels16(jmethodID id, va_list args) {
    jsize len = gfa_w * gfa_h;
    JavaDynArray * jda = jda_alloc(len, FIELD_TYPE_SHORT);
    if (jda && gfa_pixels) memcpy(jda->array, gfa_pixels, (size_t) len * 2);
    return (jobject) jda;
}

static jobject m_GFA_GetPixels32(jmethodID id, va_list args) {
    jsize len = gfa_w * gfa_h;
    JavaDynArray * jda = jda_alloc(len, FIELD_TYPE_INT);
    if (jda && gfa_pixels) memcpy(jda->array, gfa_pixels, (size_t) len * 4);
    return (jobject) jda;
}

/*
 * ---- com.gamevil.nexus2.Natives : float methods ------------------------
 */

// GFA_GetTextWidth is the real Java method name; GFA_GetTextWidths (with an
// extra "s") is what the .so actually looks up -- dead code either way, but
// both names are registered against the same implementation just in case.
static jfloat m_GFA_GetTextWidth(jmethodID id, va_list args) {
    jstring str = va_arg(args, jstring);
    jint n = va_arg(args, jint);
    (void) str;
    return (jfloat) (n * gfa_char_width());
}

/*
 * ---- Method tables ------------------------------------------------------
 */

NameToMethodID nameToMethodId[] = {
        { 1,  "OnAsyncTimerSet",          METHOD_TYPE_VOID },
        { 2,  "OnEvent",                  METHOD_TYPE_VOID },
        { 3,  "OnSoundPlay",              METHOD_TYPE_VOID },
        { 4,  "OnStopSound",              METHOD_TYPE_VOID },
        { 5,  "OnUIStatusChange",         METHOD_TYPE_VOID },
        { 6,  "OnVibrate",                METHOD_TYPE_VOID },
        { 7,  "SetSpeed",                 METHOD_TYPE_VOID },
        { 8,  "clearPlayerName",          METHOD_TYPE_VOID },
        { 9,  "hideLoadingDialog",        METHOD_TYPE_VOID },
        { 10, "hideTitleComponent",       METHOD_TYPE_VOID },
        { 11, "openUrl",                  METHOD_TYPE_VOID },
        { 12, "openStoreWithProductId",   METHOD_TYPE_VOID },
        { 13, "saveGLOptionLinear",       METHOD_TYPE_VOID },
        { 14, "showLoadingDialog",        METHOD_TYPE_VOID },
        { 15, "showTitleComponent",       METHOD_TYPE_VOID },
        { 16, "trackEventDispatch",       METHOD_TYPE_VOID },
        { 17, "trackPageViewDispatch",    METHOD_TYPE_VOID },
        { 18, "blinkSButton",             METHOD_TYPE_VOID },
        { 19, "showComponent",            METHOD_TYPE_VOID },
        { 20, "hideComponent",            METHOD_TYPE_VOID },
        { 21, "showMylSButton",           METHOD_TYPE_VOID },
        { 22, "requestIAP",               METHOD_TYPE_VOID },
        { 23, "GFA_Release",              METHOD_TYPE_VOID },
        { 24, "GFA_ReleaseFont",          METHOD_TYPE_VOID },
        { 25, "GFA_SetColor",             METHOD_TYPE_VOID },
        { 26, "GFA_SetTextAlign",         METHOD_TYPE_VOID },
        { 27, "GFA_SetAntiAlias",         METHOD_TYPE_VOID },
        { 28, "GFA_SetLocale",            METHOD_TYPE_VOID },
        { 29, "GFA_SetTextSize",          METHOD_TYPE_VOID },

        { 30, "getGLOptionLinear",        METHOD_TYPE_INT },
        { 31, "isAssetExist",             METHOD_TYPE_INT },
        { 32, "isFileExist",              METHOD_TYPE_INT },
        { 33, "isNetAvailable",           METHOD_TYPE_INT },
        { 34, "netConnect",               METHOD_TYPE_INT },
        { 35, "netBillcomSocketWrite",    METHOD_TYPE_INT },
        { 36, "netBillcomSocketConnect",  METHOD_TYPE_INT },
        { 37, "netBillcomSocketClose",    METHOD_TYPE_INT },
        { 38, "netSocket",                METHOD_TYPE_INT },
        { 39, "GFA_CharHeight",           METHOD_TYPE_INT },
        { 40, "GFA_CharWidth",            METHOD_TYPE_INT },
        { 41, "GFA_CreateFont",           METHOD_TYPE_INT },
        { 42, "GFA_GetAscent",            METHOD_TYPE_INT },
        { 43, "GFA_GetColor",             METHOD_TYPE_INT },
        { 44, "GFA_GetCurrentFont",       METHOD_TYPE_INT },
        { 45, "GFA_GetDescent",           METHOD_TYPE_INT },
        { 46, "GFA_GetStringLength",      METHOD_TYPE_INT },
        { 47, "GFA_SetFont",              METHOD_TYPE_INT },

        { 48, "GFA_Init",                 METHOD_TYPE_BOOLEAN },
        { 49, "GFA_IsInitialized",        METHOD_TYPE_BOOLEAN },

        { 50, "getAbsolueFilePath",       METHOD_TYPE_OBJECT },
        { 51, "getPhoneModel",            METHOD_TYPE_OBJECT },
        { 52, "getPhoneNumber",           METHOD_TYPE_OBJECT },
        { 53, "getPlayerNameByte",        METHOD_TYPE_OBJECT },
        { 54, "getVersion",               METHOD_TYPE_OBJECT },
        { 55, "readAssete",               METHOD_TYPE_OBJECT },
        { 56, "netBillcomSocketRead",     METHOD_TYPE_OBJECT },
        { 57, "GFA_DrawText",             METHOD_TYPE_OBJECT },
        { 58, "GFA_MeasureText",          METHOD_TYPE_OBJECT },
        { 59, "GFA_GetPixels16",          METHOD_TYPE_OBJECT },
        { 60, "GFA_GetPixels32",          METHOD_TYPE_OBJECT },

        { 61, "GFA_GetTextWidth",         METHOD_TYPE_FLOAT },
        { 62, "GFA_GetTextWidths",        METHOD_TYPE_FLOAT },

        // Dead code (never called by Illusia's own Java classes), but the
        // .so requests a string-buffer based font API that doesn't exist
        // here (see file header comment) -- register as safe void no-ops
        // so a GetStaticMethodID() lookup never returns NULL.
        { 63, "GFA_SetString",             METHOD_TYPE_VOID },
        { 64, "GFA_SetStringFromKSC5601",  METHOD_TYPE_VOID },
        { 65, "GFA_SetStringFromUnicode",  METHOD_TYPE_VOID },

        { 66, "saveFile",                  METHOD_TYPE_VOID },
        { 67, "loadFile",                  METHOD_TYPE_OBJECT },
};

MethodsVoid methodsVoid[] = {
        { 1,  m_OnAsyncTimerSet },
        { 2,  m_OnEvent },
        { 3,  m_OnSoundPlay },
        { 4,  m_OnStopSound },
        { 5,  m_OnUIStatusChange },
        { 6,  m_OnVibrate },
        { 7,  m_SetSpeed },
        { 8,  m_clearPlayerName },
        { 9,  m_hideLoadingDialog },
        { 10, m_hideTitleComponent },
        { 11, m_openUrl },
        { 12, m_openStoreWithProductId },
        { 13, m_saveGLOptionLinear },
        { 14, m_showLoadingDialog },
        { 15, m_showTitleComponent },
        { 16, m_trackEventDispatch },
        { 17, m_trackPageViewDispatch },
        { 18, m_blinkSButton },
        { 19, m_showComponent },
        { 20, m_hideComponent },
        { 21, m_showMylSButton },
        { 22, m_requestIAP },
        { 23, m_GFA_Release },
        { 24, m_GFA_ReleaseFont },
        { 25, m_GFA_SetColor },
        { 26, m_GFA_SetTextAlign },
        { 27, m_GFA_SetAntiAlias },
        { 28, m_GFA_SetLocale },
        { 29, m_GFA_SetTextSize },
        { 63, m_GFA_SetTextAlign }, // GFA_SetString: reuse a harmless no-op
        { 64, m_GFA_SetTextAlign }, // GFA_SetStringFromKSC5601: idem
        { 65, m_GFA_SetTextAlign }, // GFA_SetStringFromUnicode: idem
        { 66, m_saveFile },
};

MethodsInt methodsInt[] = {
        { 30, m_getGLOptionLinear },
        { 31, m_isAssetExist },
        { 32, m_isFileExist },
        { 33, m_isNetAvailable },
        { 34, m_netConnect },
        { 35, m_netBillcomSocketWrite },
        { 36, m_netBillcomSocketConnect },
        { 37, m_netBillcomSocketClose },
        { 38, m_netSocket },
        { 39, m_GFA_CharHeight },
        { 40, m_GFA_CharWidth },
        { 41, m_GFA_CreateFont },
        { 42, m_GFA_GetAscent },
        { 43, m_GFA_GetColor },
        { 44, m_GFA_GetCurrentFont },
        { 45, m_GFA_GetDescent },
        { 46, m_GFA_GetStringLength },
        { 47, m_GFA_SetFont },
};

MethodsBoolean methodsBoolean[] = {
        { 48, m_GFA_Init },
        { 49, m_GFA_IsInitialized },
};

MethodsObject methodsObject[] = {
        { 50, m_getAbsolueFilePath },
        { 51, m_getPhoneModel },
        { 52, m_getPhoneNumber },
        { 53, m_getPlayerNameByte },
        { 54, m_getVersion },
        { 55, m_readAssete },
        { 56, m_netBillcomSocketRead },
        { 57, m_GFA_DrawText },
        { 58, m_GFA_MeasureText },
        { 59, m_GFA_GetPixels16 },
        { 60, m_GFA_GetPixels32 },
        { 67, m_loadFile },
};

MethodsFloat methodsFloat[] = {
        { 61, m_GFA_GetTextWidth },
        { 62, m_GFA_GetTextWidth },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsLong methodsLong[] = {};
MethodsShort methodsShort[] = {};

/*
 * JNI Fields
*/

// System-wide constant that applications sometimes request
// https://developer.android.com/reference/android/content/Context.html#WINDOW_SERVICE
char WINDOW_SERVICE[] = "window";

// System-wide constant that's often used to determine Android version
// https://developer.android.com/reference/android/os/Build.VERSION.html#SDK_INT
// Possible values: https://developer.android.com/reference/android/os/Build.VERSION_CODES
const int SDK_INT = 19; // Android 4.4 / KitKat

NameToFieldID nameToFieldId[] = {
		{ 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
		{ 1, "SDK_INT", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
		{ 1, SDK_INT },
};
FieldsObject fieldsObject[] = {
		{ 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
