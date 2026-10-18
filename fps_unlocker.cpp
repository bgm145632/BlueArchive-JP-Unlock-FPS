#include <jni.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <elf.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <errno.h>

#define TAG "BlueArchiveFPSUnlocker"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef void* (*resolve_icall_t)(const char* name);
typedef void (*set_fps_t)(int fps);

static int g_fps = 120;
static volatile int g_loaded = 0;

#ifndef PKG_NAME
#define PKG_NAME "com.YostarJP.BlueArchive"
#endif

static char g_dir[256] = {0};

static bool dir_writable(const char* d) {
    char t[400];
    snprintf(t, sizeof(t), "%s/.wtest", d);
    FILE* f = fopen(t, "w");
    if (!f) return false;
    fclose(f);
    unlink(t);
    return true;
}

static bool try_dir(const char* base, const char* pkg, char* out, size_t n) {
    if (!pkg || !pkg[0]) return false;
    snprintf(out, n, "%s/%s/files", base, pkg);
    return dir_writable(out);
}

static bool pkg_from_maps(char* out, size_t n) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        const char* p = strstr(line, "/data/app/");
        if (!p) continue;
        const char* dir = strchr(p + 10, '/');
        if (!dir) continue;
        const char* end = strchr(dir + 1, '/');
        if (!end) continue;
        size_t len = (size_t)(end - dir - 1);
        if (len == 0 || len >= n) continue;
        memcpy(out, dir + 1, len);
        out[len] = 0;

        char* dash = strrchr(out, '-');
        if (dash && dash != out) *dash = 0;

        if (strchr(out, '.')) { found = true; break; }
    }
    fclose(fp);
    return found;
}

static const char* app_dir() {
    if (g_dir[0]) return g_dir;

    char pkg[160] = {0};

    if (try_dir("/data/user/0", PKG_NAME, g_dir, sizeof(g_dir))) return g_dir;
    if (try_dir("/data/data", PKG_NAME, g_dir, sizeof(g_dir))) return g_dir;

    if (pkg_from_maps(pkg, sizeof(pkg))) {
        if (try_dir("/data/user/0", pkg, g_dir, sizeof(g_dir))) return g_dir;
        if (try_dir("/data/data", pkg, g_dir, sizeof(g_dir))) return g_dir;
    }

    char raw[160] = {0};
    FILE* fp = fopen("/proc/self/cmdline", "r");
    if (fp) {
        size_t rn = fread(raw, 1, sizeof(raw) - 1, fp);
        fclose(fp);
        if (rn > 0) raw[rn] = 0;
    }
    char* colon = strchr(raw, ':');
    if (colon) *colon = 0;
    if (raw[0] && !strchr(raw, '/') && strchr(raw, '.')) {
        if (try_dir("/data/user/0", raw, g_dir, sizeof(g_dir))) return g_dir;
        if (try_dir("/data/data", raw, g_dir, sizeof(g_dir))) return g_dir;
    }

    strcpy(g_dir, "/data/local/tmp");
    return g_dir;
}

static void path_in(char* out, size_t n, const char* name) {
    snprintf(out, n, "%s/%s", app_dir(), name);
}


static void dbg(const char* fmt, ...) {
    char p[320];
    path_in(p, sizeof(p), ".bauni_log");
    FILE* f = fopen(p, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    chmod(p, 0666);
}

static int read_int_file(const char* path, int lo, int hi) {
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[32] = {0};
    int v = 0;
    if (fgets(buf, sizeof(buf), fp)) {
        v = atoi(buf);
    }
    fclose(fp);
    return (v >= lo && v <= hi) ? v : 0;
}

// ---------------------------------------------------------------------------
//   fps=144            目标帧率
//   res=1920x1358      渲染分辨率；res=original 或 res=0 = 保持原始
//   overlay=1          是否开悬浮窗（SO 不用，仅给启动器记录）
// ---------------------------------------------------------------------------
static int g_fps_target = 144;
static int g_res_w = 0;
static int g_res_h = 0;

static void trim(char* s) {
    if (!s) return;
    char* p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = 0;
}

static void parse_config(const char* path) {
    FILE* fp = fopen(path, "r");
    if (!fp) return;
    char line[160];
    while (fgets(line, sizeof(line), fp)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = line;
        char* v = eq + 1;
        trim(k); trim(v);
        if (!strcmp(k, "fps")) {
            int x = atoi(v);
            if (x >= 40 && x <= 185) g_fps_target = x;
        } else if (!strcmp(k, "res")) {
            if (!strcmp(v, "original") || !strcmp(v, "0") || !v[0]) {
                g_res_w = 0; g_res_h = 0;
            } else if (strchr(v, 'x') || strchr(v, 'X')) {
                int w = 0, h = 0;
                if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
                    g_res_w = w; g_res_h = h;
                }
            } else {
                int w = atoi(v);
                if (w >= 320) { g_res_w = w; g_res_h = 0; }   // 高度按屏幕比例推
            }
        }
    }
    fclose(fp);
}

static void read_config() {
    char p[320];
    path_in(p, sizeof(p), ".bauni.conf");
    parse_config(p);
    path_in(p, sizeof(p), ".bauni.conf");      // 兼容旧文件名
    parse_config(p);
    LOGI("[+] config: fps=%d res=%dx%d", g_fps_target, g_res_w, g_res_h);
}

static int env_fps() { return g_fps_target; }

// ---------------------------------------------------------------------------
// 把 il2cpp 里的函数 patch 成 `ret`，让游戏后续对它的调用全部变成空操作。
//   游戏自己会反复调 Application.targetFrameRate = 30 把它改回去。
//   把 set_targetFrameRate 直接 patch 成 ret 之后，游戏所有后续调用都失效，
// ---------------------------------------------------------------------------
#include <sys/mman.h>

static int change_mem_perm(void* p, size_t n, int prot) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    void* base = (void*)((uintptr_t)p & ~(uintptr_t)(ps - 1));
    size_t sz = (uintptr_t)p + n - (uintptr_t)base;
    if (sz % (size_t)ps) sz += (size_t)ps - (sz % (size_t)ps);
    return mprotect(base, sz, prot);
}

static void* follow_b_thunk(void* addr) {
    uint32_t op = 0;
    memcpy(&op, addr, 4);
    if ((op & 0xFC000000) == 0x14000000) {          
        int32_t off = (int32_t)(op << 8) >> 6;
        return (void*)((intptr_t)addr + off);
    }
    return addr;
}

static bool patch_to_ret(void* addr) {
    void* real = follow_b_thunk(addr);
    if (change_mem_perm(real, 4, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        dbg("patch: mprotect 失败 errno=%d (%s)", errno, strerror(errno));
        return false;
    }
    unsigned char ret_insn[4] = {0xC0, 0x03, 0x5F, 0xD6};
    memcpy(real, ret_insn, 4);
    __builtin___clear_cache((char*)real, (char*)real + 4);
    change_mem_perm(real, 4, PROT_READ | PROT_EXEC);
    dbg("patched to ret: 0x%lx (原始入口 0x%lx)", (unsigned long)real, (unsigned long)addr);
    return true;
}

static uintptr_t find_base() {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[1024];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "libil2cpp")) {
            unsigned long s, e, o;
            if (sscanf(line, "%lx-%lx %*s %lx", &s, &e, &o) >= 2 && o == 0) {
                base = s; break;
            }
        }
    }
    fclose(fp);
    return base;
}

static uintptr_t find_sym(uintptr_t base, const char* name) {
    Elf64_Ehdr ehdr;
    memcpy(&ehdr, (void*)base, sizeof(ehdr));

    uintptr_t dyn_addr = 0;
    size_t dyn_size = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        memcpy(&phdr, (void*)(base + ehdr.e_phoff + i * ehdr.e_phentsize), sizeof(phdr));
        if (phdr.p_type == PT_DYNAMIC) {
            dyn_addr = base + phdr.p_vaddr;
            dyn_size = phdr.p_memsz;
            break;
        }
    }
    if (!dyn_addr) return 0;

    uintptr_t symtab = 0, strtab = 0;
    size_t strsz = 0;
    Elf64_Dyn dyn;
    for (size_t off = 0; off < dyn_size; off += sizeof(dyn)) {
        memcpy(&dyn, (void*)(dyn_addr + off), sizeof(dyn));
        if (dyn.d_tag == DT_SYMTAB) symtab = dyn.d_un.d_ptr;
        if (dyn.d_tag == DT_STRTAB) strtab = dyn.d_un.d_ptr;
        if (dyn.d_tag == DT_STRSZ) strsz = dyn.d_un.d_val;
        if (dyn.d_tag == DT_NULL) break;
    }
    if (!symtab || !strtab || !strsz) return 0;
    if (symtab < base) { symtab += base; strtab += base; }

    size_t cnt = (strtab - symtab) / sizeof(Elf64_Sym);
    if (cnt > 100000) cnt = 100000;

    for (size_t i = 0; i < cnt; i++) {
        Elf64_Sym sym;
        memcpy(&sym, (void*)(symtab + i * sizeof(Elf64_Sym)), sizeof(sym));
        if (sym.st_name >= strsz || sym.st_value == 0) continue;
        char n[128] = {0};
        size_t mr = strsz - sym.st_name;
        if (mr > 127) mr = 127;
        memcpy(n, (void*)(strtab + sym.st_name), mr);
        if (strcmp(n, name) == 0) return base + sym.st_value;
    }
    return 0;
}

static void write_status(int fps) {
    char p[320];
    path_in(p, sizeof(p), ".bauni_status");
    FILE* f = fopen(p, "w");
    if (f) {
        fprintf(f, "INJECTED|FPS=%d|PID=%d", fps, getpid());
        fclose(f);
        chmod(p, 0666);
    }
}

static void write_current(float fps) {
    char p[320];
    path_in(p, sizeof(p), ".bauni_now");
    FILE* f = fopen(p, "w");
    if (f) {
        fprintf(f, "%.0f", fps);
        fclose(f);
        chmod(p, 0666);
    }
}

static void write_res(int w, int h, double pct) {
    char p[320];
    path_in(p, sizeof(p), ".bauni_res");
    FILE* f = fopen(p, "w");
    if (f) {
        fprintf(f, "%dx%d|%.0f%%", w, h, pct);
        fclose(f);
        chmod(p, 0666);
    }
}

static void* worker(void* arg) {
    int fps = g_fps;
    dbg("=== worker start: target fps=%d appdir=%s pid=%d ===", fps, app_dir(), getpid());
    LOGI("bluearchive fps unlock v1.1");
    LOGI("Target FPS: %d", fps);
    LOGI("[*] app dir: %s", app_dir());

    uintptr_t base = 0;
    for (int i = 0; i < 600; i++) {
        base = find_base();
        if (base) break;
        usleep(100000);
    }
    if (!base) { dbg("FAIL: libil2cpp not found"); LOGE("[-] libil2cpp not found"); return NULL; }
    dbg("libil2cpp base = 0x%lx", (unsigned long)base);
    LOGI("[+] Base: 0x%lx", base);

    sleep(3);

    resolve_icall_t resolve = (resolve_icall_t)find_sym(base, "il2cpp_resolve_icall");
    if (!resolve) {
        resolve = (resolve_icall_t)(base + 0xc330dc);
        dbg("il2cpp_resolve_icall NOT in dynsym; using hardcoded base+0xc330dc = %p", resolve);
        LOGI("[+] Hardcoded resolve_icall: %p", resolve);
    } else {
        dbg("il2cpp_resolve_icall = %p", resolve);
    }

    void* ptr = NULL;
    for (int i = 0; i < 120; i++) {              // 最多 60s
        ptr = resolve("UnityEngine.Application::set_targetFrameRate");
        if (ptr) break;
        if (i == 0) dbg("resolve 返回 NULL，等 il2cpp 初始化（最多 60s）...");
        if (i % 10 == 9) dbg("  仍在等待 icall 注册: %ds", (i + 1) / 2);
        usleep(500000);
    }
    if (!ptr) {
        dbg("FAIL: il2cpp_resolve_icall(\"UnityEngine.Application::set_targetFrameRate\") 60s 内一直为 NULL");
        LOGE("[-] set_targetFrameRate not found");
        return NULL;
    }
    dbg("set_targetFrameRate = %p", ptr);

    set_fps_t set_fps = (set_fps_t)ptr;

    // ----vSync 关掉并 patch 掉 ----
    // 为什么必须关：Unity 在 vSyncCount != 0 时会【忽略】targetFrameRate，
    // 为什么必须 patch 而不是周期重设：改 swap interval 会触发 EGL/显示重配，
    // 每 500ms 来一次就是"时不时卡一下"；而且游戏会把它改回去。
    typedef void (*set_vsync_t)(int);
    void* vsync_ptr = resolve("UnityEngine.QualitySettings::set_vSyncCount");
    bool vsync_off = false;
    if (vsync_ptr) {
        ((set_vsync_t)vsync_ptr)(0);
        dbg("QualitySettings.set_vSyncCount(0) called");
        vsync_off = patch_to_ret(vsync_ptr);
        LOGI("[*] set_vSyncCount patched = %s", vsync_off ? "YES" : "NO");
    } else {
        dbg("vSyncCount icall not found");
    }

    // ---- ② 设目标帧率，然后 patch 掉 ----
    // 不 patch 的话游戏会反复把它改回 30
    set_fps(fps);
    dbg("set_targetFrameRate(%d) called OK", fps);
    LOGI("[+] FPS set to %d", fps);

    bool patched = patch_to_ret(ptr);
    LOGI("[*] set_targetFrameRate patched = %s", patched ? "YES" : "NO (回退到周期重设)");
    if (!patched) {
        dbg("!! patch 失败，回退到每 500ms 重设（会有周期性掉帧）");
    }

    // ---- ③ 用 Screen.SetResolution 请求显示模式 ----
    // upstream 的 unity_engine.cc::SetResolution() 里有这一步：
    //     SetResolution_Internal(target_width, target_height, 1, &preferred_refresh_rate);
    //     Utility::NopFunc(SetResolution_Internal);
    // 第 4 个参数是 int* preferredRefreshRate —— Unity 会拿它去【向 Android 请求显示模式】。
    {
        struct UnityResolution { int32_t w; int32_t h; int32_t refresh; };
        typedef void (*get_res_t)(void*);
        typedef void (*set_res_t)(int, int, int, void*);

        int cur_w = 0, cur_h = 0;
        get_res_t get_res = (get_res_t)resolve("UnityEngine.Screen::get_currentResolution_Injected");
        if (get_res) {
            UnityResolution r = {0, 0, 0};
            get_res(&r);
            cur_w = r.w; cur_h = r.h;
            dbg("currentResolution = %dx%d @%d", r.w, r.h, r.refresh);
        } else {
            dbg("get_currentResolution_Injected not found");
        }

        set_res_t set_res = (set_res_t)resolve("UnityEngine.Screen::SetResolution");
        if (!set_res) set_res = (set_res_t)resolve("UnityEngine.Screen::SetResolution_Injected");
        if (set_res && cur_w > 0 && cur_h > 0) {
            int tw = cur_w, th = cur_h;

            // 分辨率：g_res_w 为 0 表示保持原始；否则按屏幕长宽比推导高度
            if (g_res_w > 0) {
                tw = g_res_w;
                th = (g_res_h > 0) ? g_res_h
                                   : (int)((long long)g_res_w * cur_h / cur_w);
                if (th < 240) th = 240;
            }
            double ratio = 100.0 * tw * th / ((double)cur_w * cur_h);

            int preferred = fps;                 // 顺便向系统请求这个刷新率
            dbg("SetResolution(%d, %d, 1, *%d)  原始 %dx%d  像素占比 %.0f%%",
                tw, th, preferred, cur_w, cur_h, ratio);
            set_res(tw, th, 1, &preferred);
            LOGI("[*] resolution %dx%d -> %dx%d (%.0f%% pixels), preferred=%dHz",
                 cur_w, cur_h, tw, th, ratio, preferred);
            write_res(tw, th, ratio);

            // patch 掉，防止游戏把分辨率/刷新率改回去
            patch_to_ret((void*)set_res);
            dbg("SetResolution patched to ret");
        } else {
            dbg("SetResolution 不可用（set_res=%p cur=%dx%d），跳过", (void*)set_res, cur_w, cur_h);
        }
    }

    write_status(fps);
    dbg(".bauni_status written");

    // 实测帧率：优先用 Time.frameCount 的 1 秒差值，
    // 拿不到再退回 deltaTime 的平均。
    typedef int (*get_framecount_t)(void);
    get_framecount_t get_fc = (get_framecount_t)resolve("UnityEngine.Time::get_frameCount");
    dbg("get_frameCount = %p", (void*)get_fc);

    // ------------------------------------------------------------------
    // 诊断用：把 Unity 自己认为的值读回来对质
    // 如果 patch 真的拦住了游戏，get_targetFrameRate 应该等于我们设的值。
    // ------------------------------------------------------------------
    typedef int (*get_int_t)(void);
    get_int_t get_tfr = (get_int_t)resolve("UnityEngine.Application::get_targetFrameRate");
    get_int_t get_vsc = (get_int_t)resolve("UnityEngine.QualitySettings::get_vSyncCount");
    dbg("诊断 getter: get_targetFrameRate=%p get_vSyncCount=%p", (void*)get_tfr, (void*)get_vsc);

    struct UnityResolution2 { int32_t w; int32_t h; int32_t refresh; };
    typedef void (*get_res2_t)(void*);
    get_res2_t get_res2 = (get_res2_t)resolve("UnityEngine.Screen::get_currentResolution_Injected");

    typedef float (*get_delta_t)(void);
    get_delta_t get_delta = (get_delta_t)resolve("UnityEngine.Time::get_unscaledDeltaTime");
    if (!get_delta) {
        get_delta = (get_delta_t)resolve("UnityEngine.Time::get_deltaTime");
    }
    dbg("get_deltaTime = %p", (void*)get_delta);

    int cycle = 0;
    int last_fc = -1;
    float fps_avg = 0;
    int fps_samples = 0;

    float smooth = -1.0f;      // 显示用的平滑值

    while (true) {
        usleep(500000);

        if (!patched) set_fps(fps);

        if (get_delta) {
            float dt = get_delta();
            if (dt > 0.0001f && dt < 1.0f) {
                fps_avg += 1.0f / dt;
                fps_samples++;
            }
        }

        if (++cycle % 2 == 0) {                 // 每 2 次循环 = 1 秒
            float measured = -1.0f;
            if (get_fc) {
                int fc = get_fc();
                if (last_fc >= 0 && fc > last_fc) {
                    measured = (float)(fc - last_fc);
                }
                last_fc = fc;
            }
            if (measured < 0 && fps_samples > 0) {
                measured = fps_avg / fps_samples;
            }
            if (measured >= 0) {
                smooth = (smooth < 0) ? measured : (smooth * 0.7f + measured * 0.3f);
                write_current(smooth);
                LOGI("[*] Real FPS: %.0f / %d", smooth, fps);

                if (cycle % 20 == 0) {
                    int tfr = get_tfr ? get_tfr() : -999;
                    int vsc = get_vsc ? get_vsc() : -999;
                    int rr = -1, rw = 0, rh = 0;
                    if (get_res2) {
                        UnityResolution2 r = {0, 0, 0};
                        get_res2(&r);
                        rr = r.refresh; rw = r.w; rh = r.h;
                    }
                    dbg("DIAG real=%.0f | Unity.targetFrameRate=%d (我们设 %d) | 渲染=%dx%d | 面板=%dHz",
                        smooth, tfr, fps, rw, rh, rr);
                    LOGI("[DIAG] real=%.0f unityTarget=%d(vs %d) render=%dx%d panel=%dHz",
                         smooth, tfr, fps, rw, rh, rr);
                }
            }
            fps_avg = 0;
            fps_samples = 0;
        }
    }
    return NULL;
}

static volatile int g_worker_running = 0;

static void* worker_trampoline(void* arg) {
    void* r = worker(arg);
    g_worker_running = 0;
    return r;
}

static void start_worker() {
    if (__sync_lock_test_and_set(&g_worker_running, 1)) return;   // 已有在跑
    pthread_t t;
    if (pthread_create(&t, NULL, worker_trampoline, NULL) != 0) {
        g_worker_running = 0;
        return;
    }
    pthread_detach(t);
}

__attribute__((constructor))
static void on_load() {
    if (__sync_lock_test_and_set(&g_loaded, 1)) return;
    dbg("=== SO loaded (constructor) ===");
    LOGI("[+] SO loaded");
    read_config();
    g_fps = env_fps();
    dbg("env_fps -> %d", g_fps);
    start_worker();
}

static volatile int g_jni = 0;
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* r) {
    if (__sync_lock_test_and_set(&g_jni, 1)) return JNI_VERSION_1_6;
    LOGI("[+] JNI_OnLoad");
    read_config();
    g_fps = env_fps();
    start_worker();
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT int fps_init() {
    dbg("=== fps_init() called (explicit) ===");
    read_config();
    g_fps = env_fps();
    start_worker();
    return 0;
}
