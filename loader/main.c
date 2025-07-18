/* main.c -- Grand Theft Auto: San Andreas .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <kubridge.h>
#include <vitaGL.h>
#include <vitasdk.h>
#include <vitashark.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <math.h>
#include <math_neon.h>

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "config.h"
#include "dialog.h"
#include "fios.h"
#include "gfx_patch.h"
#include "jni_patch.h"
#include "main.h"
#include "mpg123_patch.h"
#include "openal_patch.h"
#include "opengl_patch.h"
#include "scripts_patch.h"
#include "so_util.h"

#include "libc_bridge.h"

int sceLibcHeapSize = MEMORY_SCELIBC_MB * 1024 * 1024;
int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

int SCREEN_W = DEF_SCREEN_W;
int SCREEN_H = DEF_SCREEN_H;

unsigned int _oal_thread_priority = 64;
unsigned int _oal_thread_affinity = 0x40000;

SceTouchPanelInfo panelInfoFront, panelInfoBack;

so_module gtasa_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n) {
    return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n) {
    return sceClibMemmove(dest, src, n);
}

void *__wrap_memset(void *s, int c, size_t n) { return sceClibMemset(s, c, n); }

int debugPrintf(char *text, ...) {
#ifdef DEBUG
    va_list list;
    char string[512];

    va_start(list, text);
    vsprintf(string, text, list);
    va_end(list);

    SceUID fd = sceIoOpen("ux0:data/gtasa_log.txt",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, string, strlen(string));
        sceIoClose(fd);
    }
#endif
    return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG
    va_list list;
    char string[512];

    va_start(list, fmt);
    vsprintf(string, fmt, list);
    va_end(list);

    debugPrintf("%s: %s\n", tag, string);
#endif
    return 0;
}

int ret0(void) { return 0; }

int ret1(void) { return 1; }

int OS_SystemChip(void) {
    return 19; // default
}

int OS_ScreenGetHeight(void) { return SCREEN_H; }

int OS_ScreenGetWidth(void) { return SCREEN_W; }

// only used for NVEventAppMain
int pthread_create_fake(int r0, int r1, int r2, void *arg) {
    int (*func)() = *(void **) (arg + 4);
    return func();
}

int pthread_mutex_init_fake(SceKernelLwMutexWork **work) {
    *work = (SceKernelLwMutexWork *) memalign(8, sizeof(SceKernelLwMutexWork));
    if (sceKernelCreateLwMutex(*work, "mutex",
                               0x2000 | SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0,
                               NULL) < 0)
        return -1;
    return 0;
}

int pthread_mutex_destroy_fake(SceKernelLwMutexWork **work) {
    if (sceKernelDeleteLwMutex(*work) < 0)
        return -1;
    free(*work);
    return 0;
}

int pthread_mutex_lock_fake(SceKernelLwMutexWork **work) {
    if (!*work)
        pthread_mutex_init_fake(work);
    if (sceKernelLockLwMutex(*work, 1, NULL) < 0)
        return -1;
    return 0;
}

int pthread_mutex_unlock_fake(SceKernelLwMutexWork **work) {
    if (sceKernelUnlockLwMutex(*work, 1) < 0)
        return -1;
    return 0;
}

int sem_init_fake(int *uid) {
    *uid = sceKernelCreateSema("sema", 0, 0, 0x7fffffff, NULL);
    if (*uid < 0)
        return -1;
    return 0;
}

int sem_post_fake(int *uid) {
    if (sceKernelSignalSema(*uid, 1) < 0)
        return -1;
    return 0;
}

int sem_wait_fake(int *uid) {
    if (sceKernelWaitSema(*uid, 1, NULL) < 0)
        return -1;
    return 0;
}

int sem_destroy_fake(int *uid) {
    if (sceKernelDeleteSema(*uid) < 0)
        return -1;
    return 0;
}

int thread_stub(SceSize args, uintptr_t *argp) {
    int (*func)(void *arg) = (void *) argp[0];
    void *arg = (void *) argp[1];
    char *out = (char *) argp[2];
    out[0x41] = 1; // running
    func(arg);
    return sceKernelExitDeleteThread(0);
}

// CdStream with cpu 3 and priority 3
// RenderQueue with cpu 2 and priority 3
// MainThread with cpu 1 and priority 2
// StreamThread with cpu 3 and priority 1
// BankLoader with cpu 4 and priority 0
void *OS_ThreadLaunch(int (*func)(), void *arg, int cpu, char *name, int unused,
                      int priority) {
    int vita_priority;
    int vita_affinity;

    switch (priority) {
        case 0:
            vita_priority = 127;
            break;
        case 1:
            vita_priority = 106;
            break;
        case 2:
            vita_priority = 85;
            break;
        case 3:
            vita_priority = 64;
            break;
        default:
            vita_priority = 0x10000100;
            break;
    }

    switch (cpu) {
        case 1:
            vita_affinity = 0x10000;
            break;
        case 2:
            vita_affinity = 0x20000;
            break;
        case 3:
            vita_affinity = 0x40000;
            break;
        case 4:
            vita_affinity = 0x40000;
            break;
        default:
            vita_affinity = 0;
            break;
    }

    SceUID thid = sceKernelCreateThread(
            name, (SceKernelThreadEntry) thread_stub, vita_priority, 128 * 1024,
            0, vita_affinity, NULL);
    if (thid >= 0) {
        char *out = malloc(0x48);
        *(int *) (out + 0x24) = thid;

        uintptr_t args[3];
        args[0] = (uintptr_t) func;
        args[1] = (uintptr_t) arg;
        args[2] = (uintptr_t) out;
        sceKernelStartThread(thid, sizeof(args), args);

        return out;
    }

    return NULL;
}

void *OS_ThreadSetValue(void *RenderQueue) {
    *(uint8_t *) (RenderQueue + 601) = 0;
    return NULL;
}

#define HARRIER_NOZZLE_ROTATE_LIMIT 5000
#define HARRIER_NOZZLE_ROTATERATE 25.0f

static void *(*CPad__GetPad)(int pad);
static int (*CPad__GetCarGunUpDown)(void *pad, int r1, int r2, float r3,
                                    int r4);
static int (*CPad__GetSteeringLeftRight)(void *pad);
static int (*CPad__GetTurretLeft)(void *pad);
static int (*CPad__GetTurretRight)(void *pad);

static float *CTimer__ms_fTimeStep;

float CPlane__ProcessControlInputs_Rudder(void *this, int pad) {
    float val;

    uint16_t modelIndex = *(uint16_t *) (this + 0x26);
    if (modelIndex == 539) {
        val = (float) CPad__GetSteeringLeftRight(CPad__GetPad(pad)) / 128.0f -
              *(float *) (this + 0x99C);
    } else {
        if (CPad__GetTurretLeft(CPad__GetPad(pad)))
            val = (-1.0f - *(float *) (this + 0x99C));
        else if (CPad__GetTurretRight(CPad__GetPad(pad)))
            val = (1.0f - *(float *) (this + 0x99C));
        else
            val = (0.0f - *(float *) (this + 0x99C));
    }

    *(float *) (this + 0x99C) += val * 0.2f * *CTimer__ms_fTimeStep;
    return *(float *) (this + 0x99C);
}

__attribute__((naked)) void CPlane__ProcessControlInputs_Rudder_stub(void) {
    asm volatile("push {r0-r11}\n"
                 "mov r0, r4\n"
                 "mov r1, r8\n"
                 "bl CPlane__ProcessControlInputs_Rudder\n"
                 "vmov s0, r0\n");

    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x0057611A + 0x1;

    asm volatile("pop {r0-r11}\n"
                 "bx %0\n" ::"r"(retAddr));
}

void CPlane__ProcessControlInputs_Harrier(void *this, int pad) {
    uint16_t modelIndex = *(uint16_t *) (this + 0x26);
    if (modelIndex == 520) {
        float rightStickY = (float) CPad__GetCarGunUpDown(CPad__GetPad(pad), 0,
                                                          0, 2500.0f, 0);
        if (fabsf(rightStickY) > 10.0f) {
            *(int16_t *) (this + 0x882) = *(int16_t *) (this + 0x880);
            *(int16_t *) (this + 0x880) +=
                    (int16_t) (rightStickY / 128.0f *
                               HARRIER_NOZZLE_ROTATERATE *
                               *CTimer__ms_fTimeStep);
            if (*(int16_t *) (this + 0x880) < 0)
                *(int16_t *) (this + 0x880) = 0;
            else if (*(int16_t *) (this + 0x880) > HARRIER_NOZZLE_ROTATE_LIMIT)
                *(int16_t *) (this + 0x880) = HARRIER_NOZZLE_ROTATE_LIMIT;
        }
    }
}

__attribute__((naked)) void CPlane__ProcessControlInputs_Harrier_stub(void) {
    asm volatile("push {r0-r11}\n"
                 "mov r0, r4\n"
                 "mov r1, r8\n"
                 "bl CPlane__ProcessControlInputs_Harrier\n");

    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x005765F0 + 0x1;

    asm volatile("pop {r0-r11}\n"
                 "bx %0\n" ::"r"(retAddr));
}

int CCam__Process_FollowCar_SA_camSetArrPos(void *this) {
    uint16_t modelIndex = *(uint16_t *) (this + 0x26);
    if (modelIndex == 520 && *(int16_t *) (this + 0x880) >= 3000) {
        return 2; // heli
    } else {
        return modelIndex == 539 ? 0 : 3; // car or plane
    }
}

__attribute__((naked)) void CCam__Process_FollowCar_SA_camSetArrPos_stub(void) {
    asm volatile("push {r0-r8, r10-r11}\n"
                 "mov r0, r11\n"
                 "bl CCam__Process_FollowCar_SA_camSetArrPos\n"
                 "mov r9, r0\n");

    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x003C033A + 0x1;

    asm volatile("pop {r0-r8, r10-r11}\n"
                 "bx %0\n" ::"r"(retAddr));
}

uint64_t CCam__Process_FollowCar_SA_yMovement(void *this, uint32_t xMovement,
                                              uint32_t yMovement) {
    uint16_t modelIndex = *(uint16_t *) (this + 0x26);
    switch (modelIndex) {
        // case 564:
        // xMovement = 0;
        // // Fall-through
        // case 406:
        // case 443:
        // case 486:
        case 520:
            // case 524:
            // case 525:
            // case 530:
            // case 531:
            // case 592:
            yMovement = 0;
            break;
        default:
            break;
    }

    return (uint64_t) xMovement | (uint64_t) yMovement << 32;
}

__attribute__((naked)) void CCam__Process_FollowCar_SA_yMovement_stub(void) {
    asm volatile("push {r0-r11}\n"
                 "mov r0, r11\n"
                 "vmov r1, s21\n"
                 "vmov r2, s28\n"
                 "bl CCam__Process_FollowCar_SA_yMovement\n"
                 "vmov s28, r1\n"
                 "vmov s21, r0\n");

    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x003C1308 + 0x1;

    asm volatile("pop {r0-r11}\n"
                 "bx %0\n" ::"r"(retAddr));
}

typedef enum {
    MATRIX_PROJ_ID,
    MATRIX_VIEW_ID,
    MATRIX_OBJ_ID,
    MATRIX_TEX_ID
} RQShaderMatrixConstantID;

static void *(*GetCurrentProjectionMatrix)();
static void *(*GetCurrentViewMatrix)();
static void *(*GetCurrentObjectMatrix)();

void SetMatrixConstant(void *this, RQShaderMatrixConstantID id, float *matrix) {
    void *UniformMatrix = this + 0x4C * id;
    float *UniformMatrixData = UniformMatrix + 0x2AC;
    if (sceClibMemcmp(UniformMatrixData, matrix, 16 * 4) != 0) {
        sceClibMemcpy(UniformMatrixData, matrix, 16 * 4);
        *(uint8_t *) (UniformMatrix + 0x2A8) = 1;
        *(uint8_t *) (UniformMatrix + 0x2EC) = 1;
    }
}

void ES2Shader__SetMatrixConstant(void *this, RQShaderMatrixConstantID id,
                                  float *matrix) {
    if (id == MATRIX_TEX_ID) {
        SetMatrixConstant(this, id, matrix);
    } else {
        float *ProjMatrix = GetCurrentProjectionMatrix();
        float *ViewMatrix = GetCurrentViewMatrix();
        float *ObjMatrix = GetCurrentObjectMatrix();

        int forced =
                ((id == MATRIX_PROJ_ID) && !((uint8_t *) ProjMatrix)[64]) ||
                ((id == MATRIX_VIEW_ID) && !((uint8_t *) ViewMatrix)[64]) ||
                ((id == MATRIX_OBJ_ID) && !((uint8_t *) ObjMatrix)[64]);
        if (forced || ((uint8_t *) ProjMatrix)[64] ||
            ((uint8_t *) ViewMatrix)[64] || ((uint8_t *) ObjMatrix)[64]) {
            float mv[16], mvp[16];
            matmul4_neon(ViewMatrix, ObjMatrix, mv);
            matmul4_neon(ProjMatrix, mv, mvp);

            SetMatrixConstant(this, MATRIX_PROJ_ID, mvp);
        }

        if (forced || ((uint8_t *) ViewMatrix)[64])
            SetMatrixConstant(this, MATRIX_VIEW_ID, ViewMatrix);
        if (forced || ((uint8_t *) ObjMatrix)[64])
            SetMatrixConstant(this, MATRIX_OBJ_ID, ObjMatrix);

        ((uint8_t *) ProjMatrix)[64] = 0;
        ((uint8_t *) ViewMatrix)[64] = 0;
        ((uint8_t *) ObjMatrix)[64] = 0;
    }
}

// size of skin_map is 128 * 4 * 4 * 3 = 0x1800
static float *skin_map;
static int *skin_dirty;
static int *skin_num;

int emu_InternalSkinGetVectorCount(void) { return 4 * *skin_num; }

void SkinSetMatrices(void *skin, float *matrix) {
    int num = *(int *) (skin + 4);
    sceClibMemcpy(skin_map, matrix, 64 * num);
    *skin_dirty = 1;
    *skin_num = num;
}

static float *CDraw__ms_fFOV;
static float *CDraw__ms_fAspectRatio;
static float fake_fov;

float CDraw__SetFOV(float fov) {
    *CDraw__ms_fFOV =
            (((*CDraw__ms_fAspectRatio - 1.3333f) * 11.0f) / 0.88888f) + fov;
    fake_fov =
            (((1.0f / *CDraw__ms_fAspectRatio - 1.3333f) * 11.0f) / 0.88888f) +
            fov;
    return fake_fov;
}

__attribute__((naked)) void CCamera__Process_fov_stub(void) {
    asm volatile("mov r0, %0\n"
                 "vldr s2, [r0]\n" ::"r"(&fake_fov));

    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x003DD888 + 0x1;

    asm volatile("bx %0\n" ::"r"(retAddr));
}

static uint32_t CCheat__m_aCheatHashKeys[] = {
        0xDE4B237D,
        0xB22A28D1,
        0x5A783FAE,
        // WEAPON4, TIMETRAVEL, SCRIPTBYPASS, SHOWMAPPINGS
        0x5A1B5E9A,
        0x00000000,
        0x00000000,
        0x00000000,
        // INVINCIBILITY, SHOWTAPTOTARGET, SHOWTARGETING
        0x7B64E263,
        0x00000000,
        0x00000000,
        0xEECCEA2B,
        0x42AF1E28,
        0x555FC201,
        0x2A845345,
        0xE1EF01EA,
        0x771B83FC,
        0x5BF12848,
        0x44453A17,
        0x00000000,
        0xB69E8532,
        0x8B828076,
        0xDD6ED9E9,
        0xA290FD8C,
        0x00000000,
        0x43DB914E,
        0xDBC0DD65,
        0x00000000,
        0xD08A30FE,
        0x37BF1B4E,
        0xB5D40866,
        0xE63B0D99,
        0x675B8945,
        0x4987D5EE,
        0x2E8F84E8,
        0x00000000,
        0x00000000,
        0x0D5C6A4E,
        0x00000000,
        0x00000000,
        0x66516EBC,
        0x4B137E45,
        0x00000000,
        0x00000000,
        0x3A577325,
        0xD4966D59,
        // THEGAMBLER
        0x00000000,
        0x5FD1B49D,
        0xA7613F99,
        0x1792D871,
        0xCBC579DF,
        0x4FEDCCFF,
        0x44B34866,
        0x2EF877DB,
        0x2781E797,
        0x2BC1A045,
        0xB2AFE368,
        0x00000000,
        0x00000000,
        0x1A5526BC,
        0xA48A770B,
        0x00000000,
        0x00000000,
        0x00000000,
        0x7F80B950,
        0x6C0FA650,
        0xF46F2FA4,
        0x70164385,
        0x00000000,
        0x885D0B50,
        0x151BDCB3,
        0xADFA640A,
        0xE57F96CE,
        0x040CF761,
        0xE1B33EB9,
        0xFEDA77F7,
        0x00000000,
        0x00000000,
        0xF53EF5A5,
        0xF2AA0C1D,
        0xF36345A8,
        0x00000000,
        0xB7013B1B,
        0x00000000,
        0x31F0C3CC,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0xF01286E9,
        0xA841CC0A,
        0x31EA09CF,
        0xE958788A,
        0x02C83A7C,
        0xE49C3ED4,
        0x171BA8CC,
        0x86988DAE,
        0x2BDD2FA1,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
        0x00000000,
};

static void (*CCheat__AddToCheatString)(char c);

char *input_keyboard = NULL;
int input_cheat = 0;

void CCheat__DoCheats(void) {
    if (input_keyboard) {
        for (int i = 0; input_keyboard[i]; i++)
            CCheat__AddToCheatString(toupper(input_keyboard[i]));
        input_keyboard = NULL;
    }
}

int ProcessEvents(void) {
    if (!input_cheat) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
        if ((pad.buttons & SCE_CTRL_L1) && (pad.buttons & SCE_CTRL_SELECT)) {
            init_ime_dialog("Insert cheat code", "");
            input_cheat = 1;
        }
    } else {
        input_keyboard = get_ime_dialog_result();
        if (input_keyboard)
            input_cheat = 0;
    }

    return 0; // 1 is exit!
}

static void *CHIDJoystickPS3__vtable;
static void *(*CHIDJoystick__CHIDJoystick)(void *this, const char *name);
static void (*CHIDJoystick__AddMapping)(void *this, int button_id,
                                        HIDMapping mapping);

void *CHIDJoystickPS3__CHIDJoystickPS3(void *this, const char *name) {
    CHIDJoystick__CHIDJoystick(this, name);
    *(uintptr_t *) this = (uintptr_t) CHIDJoystickPS3__vtable + 8;

    for (int i = 0; i < mapping_count; i++) {
        CHIDJoystick__AddMapping(this, button_mapping[i].button_id,
                                 button_mapping[i].hid_mapping);
    }

    return this;
}

static int (*CHID__IsJustPressed)(HIDMapping mapping);
static int (*CHID__IsReleased)(HIDMapping mapping);

int CHID__IsReleased_Hook(HIDMapping mapping) {
    switch (mapping) {
        case MAPPING_JUMP:
        case MAPPING_CROUCH:
        case MAPPING_ENTER_CAR:
        case MAPPING_CAMERA_CLOSER:
        case MAPPING_CAMERA_FARTHER:
            return CHID__IsJustPressed(mapping);
        default:
            return CHID__IsReleased(mapping);
    }
}

typedef struct CVector {
    float x;
    float y;
    float z;
} CVector;

CVector multiply_scalar(const CVector vec, const float scalar) {
    CVector result;
    result.x = vec.x * scalar;
    result.y = vec.y * scalar;
    result.z = vec.z * scalar;
    return result;
}

CVector add_vectors(const CVector vec1, const CVector vec2) {
    CVector result;
    result.x = vec1.x + vec2.x;
    result.y = vec1.y + vec2.y;
    result.z = vec1.z + vec2.z;
    return result;
}

static int (*CPed__IsPlayer)(void *this);
static void (*CPhysical__ApplyMoveForce)(void *this, CVector force);
static void (*CPhysical__ApplyTurnForce)(void *this, CVector force,
                                         CVector point);

static void CAutomobile__BoostJumpControl(void *this) {
    if ((*(void **) (this + 0x464) != NULL) &&
        (CPed__IsPlayer(*(void **) (this + 0x464)) == 1)) {
        int pressed = CHID__IsJustPressed(MAPPING_TAXI_BOOST_JUMP);
        if (pressed && (*(float *) (this + 0x7E8) < 1.0f)) {
            CVector vector = {0.0f, 0.0f, *(float *) (this + 0x90) * 0.15f};
            CPhysical__ApplyMoveForce((void *) this, vector);
            CVector force = {*(float *) ((this + 0x14) + 0x94) * 0.01f *
                                     *(float *) (this + 0x20),
                             *(float *) ((this + 0x14) + 0x94) * 0.01f *
                                     *(float *) (this + 0x24),
                             *(float *) ((this + 0x14) + 0x94) * 0.01f *
                                     *(float *) (this + 0x28)};
            CVector point = {*(float *) ((this + 0x14) + 0x10),
                             *(float *) ((this + 0x14) + 0x14),
                             *(float *) ((this + 0x14) + 0x18)};
            CPhysical__ApplyTurnForce((void *) this, force, point);
        }
    }
}

static int hydraulics_locked = 0;

static int CPad__GetHydraulicJump(void *this) {
    if (*(short *) (this + 0x110) == 0) {
        int pressed = CHID__IsJustPressed(MAPPING_LOCK_HYDRAULICS);
        if (pressed) {
            hydraulics_locked = hydraulics_locked ? 0 : 1;
        }
        if (hydraulics_locked) {
            // zero out pending suspension state so it doesn't raise the vehicle
            asm volatile("mov r1, #0x0\n"
                         "vmov s21, r1\n"
                         "vmov s22, r1\n"
                         "vstr s22,[sp,#0x14]\n"
                         "vstr s22,[sp,#0x18]\n"
                         "vstr s22,[sp,#0x1C]\n");
        }
        return hydraulics_locked;
    }
    return 0;
}

__attribute__((naked)) void CPad__AimWeaponUpDown_stub(void) {
    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x003FC782 + 0x1;

    asm volatile("cbz r0, %=f\n"
                 "ldrb.w r0, [r0, #0x392]\n"
                 "lsls r0, r0, #0x1e\n" // "hydraulics installed" flag
                 "bpl %=f\n"
                 "mov r0, %1\n"
                 "ldr r0, [r0]\n"
                 "cbnz r0, %=f\n"
                 "add sp, #0x10\n" // 0x003FC744
                 "pop.w {r11}\n"
                 "pop {r4-r7, pc}\n"
                 "%=:\n"
                 "bx %0\n" ::"r"(retAddr),
                 "r"(&hydraulics_locked));
}

__attribute__((naked)) void CPad__AimWeaponLeftRight_stub(void) {
    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x003FC490 + 0x1;

    asm volatile("cbz r0, %=f\n"
                 "ldrb.w r0, [r0, #0x392]\n"
                 "lsls r0, r0, #0x1e\n" // "hydraulics installed" flag
                 "bpl %=f\n"
                 "mov r0, %1\n"
                 "ldr r0, [r0]\n"
                 "cbnz r0, %=f\n"
                 "vmov r0, s16\n" // 0x003FC44C
                 "add sp, #0x10\n"
                 "vpop {d8}\n"
                 "pop.w {r11}\n"
                 "pop {r4-r7, pc}\n"
                 "%=:\n"
                 "bx %0\n" ::"r"(retAddr),
                 "r"(&hydraulics_locked));
}

static int freeAim = 0;
static int (*CPad__ShiftTargetRightJustDown)(void *pad);
static int (*CPlayerPed__FindNextWeaponLockOnTarget)(void *pad, void *a2,
                                                     int a3);
static void (*CPlayerPed__ClearWeaponTarget)(void *playerPed);

__attribute__((naked)) void
CTaskSimplePlayerOnFoot__ProcessPlayerWeapon_stub(void) {
    void *pad, *playerPed, *prevTarget;
    asm volatile("push {r0-r11}\n"
                 "mov r0, r5\n"
                 "str r0, %0\n"
                 "ldr r1, [r4,#0x720]\n"
                 "str r1, %1\n"
                 "str r4, %2\n"
                 : "=m"(pad), "=m"(prevTarget), "=m"(playerPed));

    if (CPad__ShiftTargetRightJustDown(pad))
        CPlayerPed__FindNextWeaponLockOnTarget(playerPed, prevTarget, 0);
    else if (CHID__IsJustPressed(MAPPING_ENTER_FREE_AIM)) {
        freeAim = 1;
        CPlayerPed__ClearWeaponTarget(playerPed);
    }

    register uintptr_t retAddr asm("r12") =
            (uintptr_t) gtasa_mod.text_base + 0x00538A54 + 0x1;
    asm volatile("pop {r0-r11}\n"
                 "bx %0\n" ::"r"(retAddr));
}

static void (*CEntity__CleanUpOldReference)(void *this, void **param_1);

void CPlayerPed__Clear3rdPersonMouseTarget(void *this) {
    freeAim = 0;
    if (*(void **) (this + 0x7A4) != NULL)
        CEntity__CleanUpOldReference(*(void **) (this + 0x7A4),
                                     (void **) (this + 0x7A4));
}

static int *dword_6E04BC;

int MobileSettings__IsFreeAimMode(void *this) {
    if (freeAim)
        return 1;
    return *dword_6E04BC;
}

static int (*CGenericGameStorage__CheckSlotDataValid)(int slot,
                                                      int deleteRwObjects);
static void (*C_PcSave__GenerateGameFilename)(void *this, int slot,
                                              char *filename);
static uint64_t (*OS_FileGetDate)(int area, const char *path);
static void *PcSaveHelper;
static int *lastSaveForResume;

int MainMenuScreen__HasCPSave(void) {
    if (*lastSaveForResume == -1) {
        uint64_t latestDate = 0;
        for (int i = 0; i < 10; i++) {
            char filename[256];
            C_PcSave__GenerateGameFilename(&PcSaveHelper, i, filename);
            uint64_t date = OS_FileGetDate(1, filename);
            if (latestDate < date) {
                latestDate = date;
                *lastSaveForResume = i;
            }
        }
    }

    return CGenericGameStorage__CheckSlotDataValid(*lastSaveForResume, 1);
}

static int (*SaveGameForPause)(int type, char *cmd);

int MainMenuScreen__OnExit(void) {
    SaveGameForPause(3, NULL);
    return sceKernelExitProcess(0);
}

enum eSwimState {
    SWIM_TREAD,
    SWIM_SPRINT,
    SWIM_SPRINTING,
    SWIM_DIVE_UNDERWATER,
    SWIM_UNDERWATER_SPRINTING,
    SWIM_BACK_TO_SURFACE
};

enum eswimAnimGroup {
    ANIM_ID_SWIM_BREAST = 311,
    ANIM_ID_SWIM_CRAWL = 312,
    ANIM_ID_SWIM_DIVE_UNDER = 313,
    ANIM_ID_SWIM_UNDER = 314,
    ANIM_ID_SWIM_GLIDE = 315,
    ANIM_ID_SWIM_JUMPOUT = 316,
    ANIM_ID_CLIMB_JUMP = 128
};

typedef struct {
    float x;
    float y;
} CVector2D;

static void *(*RpAnimBlendClumpGetAssociationU)(void *clump, uint32_t animId);
static void (*ApplyMoveForce)(void *ped, float x, float y, float z);
static uint8_t (*IsPlayer)(void *ped);
static uint8_t (*GetWaterLevelFull)(float x, float y, float z,
                                    float *pOutWaterLevel,
                                    uint8_t bTouchingWater,
                                    CVector *pVecNormals);
static float *ms_fTimeStep;

float GetTimeStepInSeconds(void) { return *ms_fTimeStep / 50.0f; }

float GetTimeStepMagic(void) { return *ms_fTimeStep / (50.0f / 30.0f); }

float GetTimeStepInvMagic(void) { return 50.0f / 30.0f / *ms_fTimeStep; }

float min(const float a, const float b) { return a < b ? a : b; }

float max(const float a, const float b) { return a > b ? a : b; }

float clamp(const float value, const float minValue, const float maxValue) {
    return min(max(value, minValue), maxValue);
}

void ProcessSwimmingResistance(void *task, void *ped) {
    float fSubmergeZ = -1.0f;
    CVector vecPedMoveSpeed;

    switch (*(uint16_t *) (task + 0xA)) {
        case SWIM_TREAD:
        case SWIM_SPRINT:
        case SWIM_SPRINTING: {
            float fAnimBlendSum = 0.0f;
            float fAnimBlendDifference = 1.0f;

            void *animSwimBreast = RpAnimBlendClumpGetAssociationU(
                    *(void **) (ped + 0x18), ANIM_ID_SWIM_BREAST);
            if (animSwimBreast) {
                fAnimBlendSum = 0.4f * *(float *) (animSwimBreast + 0x18);
                fAnimBlendDifference =
                        1.0f - *(float *) (animSwimBreast + 0x18);
            }

            void *animSwimCrawl = RpAnimBlendClumpGetAssociationU(
                    *(void **) (ped + 0x18), ANIM_ID_SWIM_CRAWL);
            if (animSwimCrawl) {
                fAnimBlendSum += 0.2f * *(float *) (animSwimCrawl + 0x18);
                fAnimBlendDifference -= *(float *) (animSwimCrawl + 0x18);
            }
            if (fAnimBlendDifference < 0.0f) {
                fAnimBlendDifference = 0.0f;
            }

            fSubmergeZ = fAnimBlendDifference * 0.55f + fAnimBlendSum;

            vecPedMoveSpeed = multiply_scalar(
                    *(CVector *) (*(void **) (ped + 0x14) + 0x0),
                    *(float *) (ped + 0x4E4));
            vecPedMoveSpeed = add_vectors(
                    vecPedMoveSpeed,
                    multiply_scalar(
                            *(CVector *) (*(void **) (ped + 0x14) + 0x10),
                            *(float *) (ped + 0x4E8)));
            break;
        }
        case SWIM_DIVE_UNDERWATER: {
            vecPedMoveSpeed = multiply_scalar(
                    *(CVector *) (*(void **) (ped + 0x14) + 0x0),
                    *(float *) (ped + 0x4E4));
            vecPedMoveSpeed = add_vectors(
                    vecPedMoveSpeed,
                    multiply_scalar(
                            *(CVector *) (*(void **) (ped + 0x14) + 0x10),
                            *(float *) (ped + 0x4E8)));

            void *animSwimDiveUnder = RpAnimBlendClumpGetAssociationU(
                    *(void **) (ped + 0x18), ANIM_ID_SWIM_DIVE_UNDER);
            if (animSwimDiveUnder) {
                vecPedMoveSpeed.z =
                        *(float *) (animSwimDiveUnder + 0x20) /
                        *(float *) (*(void **) (animSwimDiveUnder + 0x14) +
                                    0x10) *
                        (-0.1f * GetTimeStepMagic());
            }
            break;
        }
        case SWIM_UNDERWATER_SPRINTING: {
            vecPedMoveSpeed = multiply_scalar(
                    *(CVector *) (*(void **) (ped + 0x14) + 0x0),
                    *(float *) (ped + 0x4E4));
            vecPedMoveSpeed = add_vectors(
                    vecPedMoveSpeed,
                    multiply_scalar(
                            *(CVector *) (*(void **) (ped + 0x14) + 0x10),
                            cosf(*(float *) (task + 0x24)) *
                                    *(float *) (ped + 0x4E8)));
            vecPedMoveSpeed.z +=
                    (sinf(*(float *) (task + 0x24)) * *(float *) (ped + 0x4E8) +
                     0.01f) /
                    GetTimeStepMagic();
            break;
        }
        case SWIM_BACK_TO_SURFACE: {
            void *animClimb = RpAnimBlendClumpGetAssociationU(
                    *(void **) (ped + 0x18), ANIM_ID_CLIMB_JUMP);
            if (!animClimb)
                animClimb = RpAnimBlendClumpGetAssociationU(
                        *(void **) (ped + 0x18), ANIM_ID_SWIM_JUMPOUT);

            if (animClimb) {
                if (*(float *) (*(void **) (animClimb + 0x14) + 0x10) >
                            *(float *) (animClimb + 0x20) &&
                    (*(float *) (animClimb + 0x18) >= 1.0f ||
                     *(float *) (animClimb + 0x1C) > 0.0f)) {
                    const float fMoveForceZ = *ms_fTimeStep *
                                              *(float *) (ped + 0x8C) * 0.3f *
                                              0.008f;
                    ApplyMoveForce(ped, 0.0f, 0.0f, fMoveForceZ);
                }
            }
            return;
        }
        default: {
            return;
        }
    }

    const float fTheTimeStep = powf(0.9f, *ms_fTimeStep);
    vecPedMoveSpeed = multiply_scalar(
            vecPedMoveSpeed, (1.0f - fTheTimeStep) * GetTimeStepInvMagic());
    *(CVector *) (ped + 0x48) =
            multiply_scalar(*(CVector *) (ped + 0x48), fTheTimeStep);
    if (IsPlayer(ped))
        vecPedMoveSpeed = multiply_scalar(vecPedMoveSpeed, 1.25f);
    *(CVector *) (ped + 0x48) =
            add_vectors(*(CVector *) (ped + 0x48), vecPedMoveSpeed);

    CVector pedPos = *(CVector *) (*(void **) (ped + 0x14) + 0x30);
    uint8_t bUpdateRotationX = SCE_TRUE;
    const CVector vecCheckWaterLevelPos = add_vectors(
            pedPos, multiply_scalar(*(CVector *) (ped + 0x48), *ms_fTimeStep));

    float fWaterLevel = 0.0f;
    if (!GetWaterLevelFull(vecCheckWaterLevelPos.x, vecCheckWaterLevelPos.y,
                           vecCheckWaterLevelPos.z, &fWaterLevel, SCE_TRUE,
                           NULL)) {
        fSubmergeZ = -1.0f;
        bUpdateRotationX = SCE_FALSE;
    } else {
        if (*(uint16_t *) (task + 0xA) != SWIM_UNDERWATER_SPRINTING ||
            *(float *) (task + 0x34) < 0.0f) {
            bUpdateRotationX = SCE_FALSE;
        } else {
            if (pedPos.z + 0.65f > fWaterLevel &&
                *(float *) (task + 0x24) > 0.7854f) {
                *(uint16_t *) (task + 0xA) = SWIM_TREAD;
                *(float *) (task + 0x34) = 0.0f;
                bUpdateRotationX = SCE_FALSE;
            }
        }
    }

    if (bUpdateRotationX) {
        if (*(float *) (task + 0x24) >= 0.0f) {
            if (pedPos.z + 0.65f <= fWaterLevel) {
                if (*(float *) (task + 0x34) <= 0.001f)
                    *(float *) (task + 0x34) = 0.0f;
                else
                    *(float *) (task + 0x34) *= 0.95f;
            } else {
                const float fMinimumSpeed = 0.05f * 0.5f;
                if (*(float *) (task + 0x34) > fMinimumSpeed) {
                    *(float *) (task + 0x34) *= 0.95f;
                }
                if (*(float *) (task + 0x34) < fMinimumSpeed) {
                    *(float *) (task + 0x34) += GetTimeStepInSeconds() / 10.0f;
                    *(float *) (task + 0x34) =
                            min(fMinimumSpeed, *(float *) (task + 0x34));
                }
                *(float *) (task + 0x24) +=
                        *ms_fTimeStep * *(float *) (task + 0x34);
                fSubmergeZ = (0.55f - 0.2f) *
                                     (*(float *) (task + 0x24) * 4.0f / M_PI) *
                                     0.75f +
                             0.2f;
            }
        } else {
            if (pedPos.z - sinf(*(float *) (task + 0x24)) + 0.65f <=
                fWaterLevel) {
                if (*(float *) (task + 0x34) > 0.001f)
                    *(float *) (task + 0x34) *= 0.95f;
                else
                    *(float *) (task + 0x34) = 0.0f;
            } else {
                *(float *) (task + 0x34) += GetTimeStepInSeconds() / 10.0f;
                *(float *) (task + 0x34) = min(*(float *) (task + 0x34), 0.05f);
            }
            *(float *) (task + 0x24) +=
                    *ms_fTimeStep * *(float *) (task + 0x34);
        }
    }

    if (fSubmergeZ > 0.0f) {
        fWaterLevel -= fSubmergeZ + pedPos.z;
        float fTimeStepMoveSpeedZ = fWaterLevel / *ms_fTimeStep;
        float fTimeStep = *ms_fTimeStep * 0.1f;
        fTimeStepMoveSpeedZ = clamp(fTimeStepMoveSpeedZ, -fTimeStep, fTimeStep);
        fTimeStepMoveSpeedZ -= ((CVector *) (ped + 0x48))->z;
        fTimeStep = GetTimeStepInSeconds();
        fTimeStepMoveSpeedZ = clamp(fTimeStepMoveSpeedZ, -fTimeStep, fTimeStep);
        ((CVector *) (ped + 0x48))->z += fTimeStepMoveSpeedZ;
    }

    if (pedPos.z < -69.0f) {
        pedPos.z = -69.0f;
        ((CVector *) (ped + 0x48))->z =
                max(((CVector *) (ped + 0x48))->z, 0.0f);
    }
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void patch_game(void) {
    *(uint8_t *) so_symbol(&gtasa_mod, "UseCloudSaves") = 0;
    *(uint8_t *) so_symbol(&gtasa_mod, "UseTouchSense") = 0;

    if (config.disable_detail_textures)
        *(int *) so_symbol(&gtasa_mod, "gNoDetailTextures") = 1;

    hook_addr(so_symbol(&gtasa_mod, "_Z14IsRemovedTracki"), (uintptr_t) ret0);

    // QueueUpTracksForStation
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003A152A + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003A1602 + 0x1);

    // ChooseMusicTrackIndex
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003A35F6 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003A369A + 0x1);

    // ChooseIdentIndex
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003A37C2 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003A385E + 0x1);

    // ChooseAdvertIndex
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003A3A1E + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003A3AA2 + 0x1);

    // ChooseTalkRadioShow
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003A4374 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003A4416 + 0x1);

    // ChooseDJBanterIndexFromList
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003A44D6 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003A4562 + 0x1);

    // Water fix
    RpAnimBlendClumpGetAssociationU = (void *) so_symbol(
            &gtasa_mod, "_Z30RpAnimBlendClumpGetAssociationP7RpClumpj");
    ms_fTimeStep = (float *) so_symbol(&gtasa_mod, "_ZN6CTimer12ms_fTimeStepE");
    ApplyMoveForce = (void *) so_symbol(
            &gtasa_mod, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    IsPlayer = (void *) so_symbol(&gtasa_mod, "_ZNK4CPed8IsPlayerEv");
    GetWaterLevelFull = (void *) so_symbol(
            &gtasa_mod, "_ZN11CWaterLevel13GetWaterLevelEfffPfbP7CVector");

    hook_addr(
            so_symbol(&gtasa_mod,
                      "_ZN15CTaskSimpleSwim25ProcessSwimmingResistanceEP4CPed"),
            (uintptr_t) ProcessSwimmingResistance);

    // Fix Second Siren
    const uint16_t nopInstruction = 0xBF00;
    const uint32_t patchInstruction = 0x8000F3AF;
    uintptr_t firstPatchAddress = gtasa_mod.text_base + 0x00590133 + 0x1;
    uintptr_t secondPatchAddress = gtasa_mod.text_base + 0x00590168;

    // Add 2 NOP instructions
    for (size_t i = 0; i < 2; ++i) {
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (firstPatchAddress + (i * sizeof(uint16_t))),
                &nopInstruction, sizeof(nopInstruction));
        kuKernelCpuUnrestrictedMemcpy((void *) (secondPatchAddress),
                                      &patchInstruction,
                                      sizeof(patchInstruction));
    }

    // Show muzzle flash for the last bullet in magazine
    uintptr_t destinationAddress = gtasa_mod.text_base + 0x4DDCCA;

    // Add 10 NOP instructions
    for (int i = 0; i < 10; ++i) {
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (destinationAddress + (i * sizeof(uint16_t))),
                &nopInstruction, sizeof(nopInstruction));
    }

    // An ability to remove FOV-effect while driving a car
    if (config.car_fov_effects) {
        uintptr_t PlaseNOP = gtasa_mod.text_base + 0x3C07E5 + 0x1;
        uintptr_t PlaseNOP2 = gtasa_mod.text_base + 0x3C082B + 0x1;

        for (size_t i = 0; i < 2; ++i) {
            kuKernelCpuUnrestrictedMemcpy((void *) (PlaseNOP), &nopInstruction,
                                          sizeof(nopInstruction));
            kuKernelCpuUnrestrictedMemcpy((void *) (PlaseNOP2), &nopInstruction,
                                          sizeof(nopInstruction));
        }
    }

    // Aiming with Country Rifle is now in 3rd person
    const uint32_t patch = 0xFF;
    uintptr_t firstPatch = gtasa_mod.text_base + 0x5378C0;
    uintptr_t secondPatch = gtasa_mod.text_base + 0x53813C;

    for (size_t i = 0; i < 3; ++i) {
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (firstPatch + (i * sizeof(uint16_t))), &nopInstruction,
                sizeof(nopInstruction));
        kuKernelCpuUnrestrictedMemcpy((void *) (secondPatch), &patch,
                                      sizeof(patch));
    }

    if (config.fix_heli_plane_camera) {
        // Dummy all FindPlayerVehicle calls so the right analog stick can be
        // used as camera again
        uint32_t movs_r0_0 = 0xBF002000;
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x003C0866), &movs_r0_0,
                sizeof(movs_r0_0));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x003C1518), &movs_r0_0,
                sizeof(movs_r0_0));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x003C198A), &movs_r0_0,
                sizeof(movs_r0_0));

        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x003FC462), &movs_r0_0,
                sizeof(movs_r0_0));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x003FC754), &movs_r0_0,
                sizeof(movs_r0_0));

        // Fix Harrier thruster control
        hook_addr((uintptr_t) (gtasa_mod.text_base + 0x003C057C + 0x1),
                  (uintptr_t) CCam__Process_FollowCar_SA_camSetArrPos_stub);
        hook_addr((uintptr_t) (gtasa_mod.text_base + 0x003C12F4 + 0x1),
                  (uintptr_t) CCam__Process_FollowCar_SA_yMovement_stub);

        CPad__GetPad = (void *) so_symbol(&gtasa_mod, "_ZN4CPad6GetPadEi");
        CPad__GetCarGunUpDown = (void *) so_symbol(
                &gtasa_mod, "_ZN4CPad15GetCarGunUpDownEbP11CAutomobilefb");
        CPad__GetSteeringLeftRight = (void *) so_symbol(
                &gtasa_mod, "_ZN4CPad20GetSteeringLeftRightEv");
        CPad__GetTurretLeft =
                (void *) so_symbol(&gtasa_mod, "_ZN4CPad13GetTurretLeftEv");
        CPad__GetTurretRight =
                (void *) so_symbol(&gtasa_mod, "_ZN4CPad14GetTurretRightEv");
        CTimer__ms_fTimeStep =
                (float *) so_symbol(&gtasa_mod, "_ZN6CTimer12ms_fTimeStepE");
        hook_addr((uintptr_t) (gtasa_mod.text_base + 0X005760BA + 0x1),
                  (uintptr_t) CPlane__ProcessControlInputs_Rudder_stub);
        hook_addr((uintptr_t) (gtasa_mod.text_base + 0x00576432 + 0x1),
                  (uintptr_t) CPlane__ProcessControlInputs_Harrier_stub);
    }

    // Force using GL_UNSIGNED_SHORT
    if (config.fix_skin_weights) {
        uint16_t movs_r1_1 = 0x2101;
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x001C8064), &movs_r1_1,
                sizeof(movs_r1_1));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x001C8082), &movs_r1_1,
                sizeof(movs_r1_1));
    }

    if (config.enable_high_detail_player)
        hook_addr(so_symbol(&gtasa_mod, "_Z17UseHiDetailPlayerv"),
                  (uintptr_t) ret1);

    if (config.enable_bones_optimization) {
        skin_map = (float *) so_symbol(&gtasa_mod, "skin_map");
        skin_dirty = (int *) so_symbol(&gtasa_mod, "skin_dirty");
        skin_num = (int *) so_symbol(&gtasa_mod, "skin_num");
        hook_addr(so_symbol(&gtasa_mod, "_Z30emu_InternalSkinGetVectorCountv"),
                  (uintptr_t) emu_InternalSkinGetVectorCount);
        hook_addr((uintptr_t) gtasa_mod.text_base + 0x001C8670 + 0x1,
                  (uintptr_t) SkinSetMatrices);
    }

    if (config.enable_mvp_optimization) {
        GetCurrentProjectionMatrix = (void *) so_symbol(
                &gtasa_mod, "_Z26GetCurrentProjectionMatrixv");
        GetCurrentViewMatrix =
                (void *) so_symbol(&gtasa_mod, "_Z20GetCurrentViewMatrixv");
        GetCurrentObjectMatrix =
                (void *) so_symbol(&gtasa_mod, "_Z22GetCurrentObjectMatrixv");
        hook_addr(so_symbol(&gtasa_mod, "_ZN9ES2Shader17SetMatrixConstantE24RQS"
                                        "haderMatrixConstantIDPKf"),
                  (uintptr_t) ES2Shader__SetMatrixConstant);
    }

    // Ignore widgets and popups introduced in mobile
    if (config.ignore_mobile_stuff) {
        uint16_t nop16 = 0xbf00;
        uint32_t nop32 = 0xbf00bf00;

        // Ignore cutscene skip button
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x0043A7A0), &nop32,
                sizeof(nop32));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x004627E6), &nop32,
                sizeof(nop32));

        // Ignore side mission buttons (vigilante, paramedic, etc)
        hook_addr(so_symbol(&gtasa_mod,
                            "_ZN25CWidgetButtonMissionStart6UpdateEv"),
                  (uintptr_t) ret0);
        hook_addr(so_symbol(&gtasa_mod,
                            "_ZN26CWidgetButtonMissionCancel6UpdateEv"),
                  (uintptr_t) ret0);

        // Ignore steering control popup
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x003F91B6), &nop16,
                sizeof(nop16));

        // Ignore app rating popup
        hook_addr(so_symbol(&gtasa_mod, "_Z12Menu_ShowNagv"), (uintptr_t) ret0);

        // Ignore items in the controls menu
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x0029E4AE), &nop32,
                sizeof(nop32));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x0029E4E6), &nop32,
                sizeof(nop32));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x0029E50A), &nop32,
                sizeof(nop32));
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x0029E530), &nop32,
                sizeof(nop32));
    }

    // Remove map highlight (explored zones) since alpha blending is very
    // expensive
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x002AADE0 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x002AAF9A + 0x1);

    // fix free aiming
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x004C6D16 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x004C6E28 + 0x1);

    // Fix target switching firing twice
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x003C73F8 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x003C7424 + 0x1);

    // Disable auto landing gear deployment/retraction
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x0057629C + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x005762BC + 0x1);

    // Fix emergency vehicles
    CDraw__ms_fFOV = (void *) so_symbol(&gtasa_mod, "_ZN5CDraw7ms_fFOVE");
    CDraw__ms_fAspectRatio =
            (void *) so_symbol(&gtasa_mod, "_ZN5CDraw15ms_fAspectRatioE");
    hook_thumb(so_symbol(&gtasa_mod, "_ZN5CDraw6SetFOVEfb"),
               (uintptr_t) CDraw__SetFOV);
    hook_thumb((uintptr_t) (gtasa_mod.text_base + 0x003DD880),
               (uintptr_t) CCamera__Process_fov_stub);

    // Nuke telemetry
    hook_addr(so_symbol(&gtasa_mod, "_Z13SaveTelemetryv"), (uintptr_t) ret0);
    hook_addr(so_symbol(&gtasa_mod, "_Z13LoadTelemetryv"), (uintptr_t) ret0);
    hook_addr(so_symbol(&gtasa_mod, "_Z11updateUsageb"), (uintptr_t) ret0);

    hook_addr(so_symbol(&gtasa_mod, "_Z14AND_FileUpdated"), (uintptr_t) ret0);
    hook_addr(so_symbol(&gtasa_mod, "_Z17AND_BillingUpdateb"),
              (uintptr_t) ret0);

    hook_addr(so_symbol(&gtasa_mod, "__cxa_guard_acquire"),
              (uintptr_t) &__cxa_guard_acquire);
    hook_addr(so_symbol(&gtasa_mod, "__cxa_guard_release"),
              (uintptr_t) &__cxa_guard_release);

    hook_addr(so_symbol(&gtasa_mod, "_Z24NVThreadGetCurrentJNIEnvv"),
              (uintptr_t) NVThreadGetCurrentJNIEnv);

    // do not use pthread
    hook_addr(so_symbol(&gtasa_mod,
                        "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"),
              (uintptr_t) OS_ThreadLaunch);

    // do not use mutex for RenderQueue
    hook_addr(so_symbol(&gtasa_mod, "_Z17OS_ThreadSetValuePv"),
              (uintptr_t) OS_ThreadSetValue);

    hook_addr(so_symbol(&gtasa_mod, "_Z17OS_ScreenGetWidthv"),
              (uintptr_t) OS_ScreenGetWidth);
    hook_addr(so_symbol(&gtasa_mod, "_Z18OS_ScreenGetHeightv"),
              (uintptr_t) OS_ScreenGetHeight);

    // TODO: set deviceChip, definedDevice
    hook_addr(so_symbol(&gtasa_mod, "_Z20AND_SystemInitializev"),
              (uintptr_t) ret0);

    // TODO: implement touch here
    hook_addr(so_symbol(&gtasa_mod, "_Z13ProcessEventsb"),
              (uintptr_t) ProcessEvents);

    // no adjustable
    hook_addr(so_symbol(&gtasa_mod, "_ZN14CAdjustableHUD10SaveToDiskEv"),
              (uintptr_t) ret0);
    hook_addr(so_symbol(&gtasa_mod,
                        "_ZN15CTouchInterface27RepositionAdjustableWidgetsEv"),
              (uintptr_t) ret0);

    // cheats support
    CCheat__AddToCheatString =
            (void *) so_symbol(&gtasa_mod, "_ZN6CCheat16AddToCheatStringEc");
    kuKernelCpuUnrestrictedMemcpy(
            (void *) so_symbol(&gtasa_mod, "_ZN6CCheat16m_aCheatHashKeysE"),
            CCheat__m_aCheatHashKeys, sizeof(CCheat__m_aCheatHashKeys));
    hook_addr(so_symbol(&gtasa_mod, "_ZN6CCheat8DoCheatsEv"),
              (uintptr_t) CCheat__DoCheats);

    // hook buttons mapping
    CHIDJoystickPS3__vtable =
            (void *) so_symbol(&gtasa_mod, "_ZTV15CHIDJoystickPS3");
    CHIDJoystick__CHIDJoystick =
            (void *) so_symbol(&gtasa_mod, "_ZN12CHIDJoystickC2EPKc");
    CHIDJoystick__AddMapping = (void *) so_symbol(
            &gtasa_mod, "_ZN12CHIDJoystick10AddMappingEi10HIDMapping");
    if (mapping_count == 0)
        hook_addr(so_symbol(&gtasa_mod, "_ZN15CHIDJoystickPS3C2EPKc"),
                  (uintptr_t) so_symbol(&gtasa_mod,
                                        "_ZN19CHIDJoystickXbox360C2EPKc"));
    else
        hook_addr(so_symbol(&gtasa_mod, "_ZN15CHIDJoystickPS3C2EPKc"),
                  (uintptr_t) CHIDJoystickPS3__CHIDJoystickPS3);

    // Change IsReleased to IsJustPressed for some mappings
    CHID__IsJustPressed = (void *) so_symbol(
            &gtasa_mod, "_ZN4CHID13IsJustPressedE10HIDMapping");
    CHID__IsReleased =
            (void *) so_symbol(&gtasa_mod, "_ZN4CHID10IsReleasedE10HIDMapping");
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x0018DFC4,
              (uintptr_t) CHID__IsReleased_Hook);

    // Redirect taxi boost jump to custom binding
    CPed__IsPlayer = (void *) so_symbol(&gtasa_mod, "_ZNK4CPed8IsPlayerEv");
    CPhysical__ApplyMoveForce = (void *) so_symbol(
            &gtasa_mod, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    CPhysical__ApplyTurnForce = (void *) so_symbol(
            &gtasa_mod, "_ZN9CPhysical14ApplyTurnForceE7CVectorS0_");
    hook_addr(so_symbol(&gtasa_mod, "_ZN11CAutomobile16BoostJumpControlEv"),
              (uintptr_t) CAutomobile__BoostJumpControl);

    // Fix hydraulics lock so the right analog stick can be used as camera again
    hook_addr(so_symbol(&gtasa_mod, "_ZN4CPad16GetHydraulicJumpEv"),
              (uintptr_t) CPad__GetHydraulicJump);
    hook_addr((uintptr_t) (gtasa_mod.text_base + 0x003FC778 + 0x1),
              (uintptr_t) CPad__AimWeaponUpDown_stub);
    hook_addr((uintptr_t) (gtasa_mod.text_base + 0x003FC486 + 0x1),
              (uintptr_t) CPad__AimWeaponLeftRight_stub);

    // Add custom Free aim binding
    CPad__ShiftTargetRightJustDown = (void *) so_symbol(
            &gtasa_mod, "_ZN4CPad24ShiftTargetRightJustDownEv");
    CPlayerPed__FindNextWeaponLockOnTarget = (void *) so_symbol(
            &gtasa_mod,
            "_ZN10CPlayerPed26FindNextWeaponLockOnTargetEP7CEntityb");
    CPlayerPed__ClearWeaponTarget = (void *) so_symbol(
            &gtasa_mod, "_ZN10CPlayerPed17ClearWeaponTargetEv");
    CEntity__CleanUpOldReference = (void *) so_symbol(
            &gtasa_mod, "_ZN7CEntity19CleanUpOldReferenceEPPS_");
    dword_6E04BC = (int *) (uintptr_t) (gtasa_mod.text_base + 0x006E04BC);
    hook_addr((uintptr_t) (gtasa_mod.text_base + 0x005387FC + 0x1),
              (uintptr_t) CTaskSimplePlayerOnFoot__ProcessPlayerWeapon_stub);
    hook_addr(so_symbol(&gtasa_mod,
                        "_ZN10CPlayerPed25Clear3rdPersonMouseTargetEv"),
              (uintptr_t) CPlayerPed__Clear3rdPersonMouseTarget);
    hook_addr(so_symbol(&gtasa_mod, "_ZN14MobileSettings13IsFreeAimModeEv"),
              (uintptr_t) MobileSettings__IsFreeAimMode);


    // make resume load the latest save
    CGenericGameStorage__CheckSlotDataValid = (void *) so_symbol(
            &gtasa_mod, "_ZN19CGenericGameStorage18CheckSlotDataValidEib");
    C_PcSave__GenerateGameFilename = (void *) so_symbol(
            &gtasa_mod, "_ZN8C_PcSave20GenerateGameFilenameEiPc");
    OS_FileGetDate = (void *) so_symbol(
            &gtasa_mod, "_Z14OS_FileGetDate14OSFileDataAreaPKc");
    PcSaveHelper = (void *) so_symbol(&gtasa_mod, "PcSaveHelper");
    lastSaveForResume = (void *) so_symbol(&gtasa_mod, "lastSaveForResume");
    hook_addr(so_symbol(&gtasa_mod, "_ZN14MainMenuScreen9HasCPSaveEv"),
              (uintptr_t) MainMenuScreen__HasCPSave);

    // Always drawable wanted stars
    if (config.show_wanted_stars) {
        hook_addr((uintptr_t) gtasa_mod.text_base + 0x2BDF82 + 0x1,
                  (uintptr_t) gtasa_mod.text_base + 0x2BDFA4 + 0x1);
    }

    // re3: Make cars and peds to not despawn when you look away
    // Vehicles
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x2EC660 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x2EC6D6 + 0x1);
    // Peds
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x4CE4EA + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x4CE55C + 0x1);

    // CJ magnetting to stealable objects only when very close
    uint32_t nop17 = 0x0A00EEB7;
    kuKernelCpuUnrestrictedMemcpy((void *) (gtasa_mod.text_base + 0x40B162),
                                  &nop17, sizeof(nop17));

    // re3: Road reflections
    if (config.road_reflections) {
        uint32_t nop18 = 0xbf00982F;
        kuKernelCpuUnrestrictedMemcpy(
                (void *) (gtasa_mod.text_base + 0x005A2E9C), &nop18,
                sizeof(nop18));
    }

    // Fixed coronas stretching while the weather is foggy
    uint32_t nop19 = 0x0A44EEB0;
    kuKernelCpuUnrestrictedMemcpy((void *) (gtasa_mod.text_base + 0x005A27EC),
                                  &nop19, sizeof(nop19));

    // Fixed breathing underwater (bubbles)
    uint64_t nop20 = 0x0238F8D00B8CEDD0;
    kuKernelCpuUnrestrictedMemcpy((void *) (gtasa_mod.text_base + 0x53C4A0),
                                  &nop20, sizeof(nop20));

    // Reflections are now bigger in quality
    uint32_t nop21 = 0x6480F44F;
    kuKernelCpuUnrestrictedMemcpy((void *) (gtasa_mod.text_base + 0x5C49E6),
                                  &nop21, sizeof(nop21));

    // Dont kill peds when jacking their car, monster!
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x4F5FC4 + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x4F5FD6 + 0x1);

    // Sprinting on any surface is allowed
    hook_addr(so_symbol(&gtasa_mod, "_ZN14SurfaceInfos_c12CantSprintOnEj"),
              (uintptr_t) ret0);

    // Radar streaming should be fixed
    hook_addr((uintptr_t) gtasa_mod.text_base + 0x44313A + 0x1,
              (uintptr_t) gtasa_mod.text_base + 0x443146 + 0x1);

    // Remove "ExtraAirResistance" flag
    hook_addr(so_symbol(&gtasa_mod,
                        "_ZN10CCullZones29DoExtraAirResistanceForPlayerEv"),
              (uintptr_t) ret0);

    // support graceful exit
    SaveGameForPause = (void *) so_symbol(&gtasa_mod,
                                          "_Z16SaveGameForPause10eSaveTypesPc");
    hook_addr(so_symbol(&gtasa_mod, "_ZN14MainMenuScreen6OnExitEv"),
              (uintptr_t) MainMenuScreen__OnExit);
}

void glTexImage2DHook(GLenum target, GLint level, GLint internalformat,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *data) {
    if (level == 0)
        glTexImage2D(target, level, internalformat, width, height, border,
                     format, type, data);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format,
                                GLsizei width, GLsizei height, GLint border,
                                GLsizei imageSize, const void *data) {
    // mips for PVRTC textures break when they're under 1 block in size
    if (level == 0 ||
        (!config.disable_mipmaps && ((width >= 4 && height >= 4) ||
                                     (format != 0x8C01 && format != 0x8C02))))
        glCompressedTexImage2D(target, level, format, width, height, border,
                               imageSize, data);
}

extern void *_ZdaPv;
extern void *_ZdlPv;
extern void *_Znaj;
extern void *_Znwj;

extern void *__aeabi_d2ulz;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_l2d;
extern void *__aeabi_l2f;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_ul2d;
extern void *__aeabi_ul2f;
extern void *__aeabi_uldivmod;

extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__stack_chk_fail;
extern void *__stack_chk_guard;

int __signbit(double d) { return signbit(d); }

int __isfinite(double d) { return isfinite(d); }

void *sceClibMemclr(void *dst, SceSize len) {
    return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch) {
    return sceClibMemset(dst, ch, len);
}

int stat_hook(const char *pathname, void *statbuf) {
    struct stat st;
    int res = stat(pathname, &st);
    if (res == 0)
        *(int *) (statbuf + 0x50) = st.st_mtime;
    return res;
}

static int EnterGameFromSCFunc = 0;
static int SigningOutfromApp = 0;
static int hasTouchScreen = 0;

static int __stack_chk_guard_fake = 0x42424242;

static FILE *stderr_fake;
static FILE __sF_fake[0x100][3];

static so_default_dynlib default_dynlib[] = {
        {"_ZdaPv", (uintptr_t) &_ZdaPv},
        {"_ZdlPv", (uintptr_t) &_ZdlPv},
        {"_Znaj", (uintptr_t) &_Znaj},
        {"_Znwj", (uintptr_t) &_Znwj},

        {"__aeabi_d2ulz", (uintptr_t) &__aeabi_d2ulz},
        {"__aeabi_idivmod", (uintptr_t) &__aeabi_idivmod},
        {"__aeabi_idiv", (uintptr_t) &__aeabi_idiv},
        {"__aeabi_l2d", (uintptr_t) &__aeabi_l2d},
        {"__aeabi_l2f", (uintptr_t) &__aeabi_l2f},
        {"__aeabi_ldivmod", (uintptr_t) &__aeabi_ldivmod},
        {"__aeabi_uidivmod", (uintptr_t) &__aeabi_uidivmod},
        {"__aeabi_uidiv", (uintptr_t) &__aeabi_uidiv},
        {"__aeabi_ul2d", (uintptr_t) &__aeabi_ul2d},
        {"__aeabi_ul2f", (uintptr_t) &__aeabi_ul2f},
        {"__aeabi_uldivmod", (uintptr_t) &__aeabi_uldivmod},

        {"__aeabi_memclr", (uintptr_t) &sceClibMemclr},
        {"__aeabi_memclr4", (uintptr_t) &sceClibMemclr},
        {"__aeabi_memclr8", (uintptr_t) &sceClibMemclr},
        {"__aeabi_memcpy", (uintptr_t) &sceClibMemcpy},
        {"__aeabi_memcpy4", (uintptr_t) &sceClibMemcpy},
        {"__aeabi_memcpy8", (uintptr_t) &sceClibMemcpy},
        {"__aeabi_memmove", (uintptr_t) &sceClibMemmove},
        {"__aeabi_memmove4", (uintptr_t) &sceClibMemmove},
        {"__aeabi_memmove8", (uintptr_t) &sceClibMemmove},
        {"__aeabi_memset", (uintptr_t) &sceClibMemset2},
        {"__aeabi_memset4", (uintptr_t) &sceClibMemset2},
        {"__aeabi_memset8", (uintptr_t) &sceClibMemset2},

        {"__android_log_print", (uintptr_t) &__android_log_print},
        // { "__assert2", (uintptr_t)&__assert2 },
        {"__cxa_atexit", (uintptr_t) &__cxa_atexit},
        {"__cxa_finalize", (uintptr_t) &__cxa_finalize},
        {"__errno", (uintptr_t) &__errno},
        {"__isfinite", (uintptr_t) &__isfinite},
        {"__sF", (uintptr_t) &__sF_fake},
        {"__signbit", (uintptr_t) &__signbit},
        {"__stack_chk_fail", (uintptr_t) &__stack_chk_fail},
        {"__stack_chk_guard", (uintptr_t) &__stack_chk_guard_fake},

        {"AAssetManager_fromJava", (uintptr_t) &ret0},
        {"AAssetManager_open", (uintptr_t) &ret0},
        {"AAsset_close", (uintptr_t) &ret0},
        {"AAsset_getLength", (uintptr_t) &ret0},
        {"AAsset_getRemainingLength", (uintptr_t) &ret0},
        {"AAsset_read", (uintptr_t) &ret0},
        {"AAsset_seek", (uintptr_t) &ret0},

        {"_Z13SetJNEEnvFuncPFPvvE", (uintptr_t) &ret0},

        {"EnterGameFromSCFunc", (uintptr_t) &EnterGameFromSCFunc},
        {"SigningOutfromApp", (uintptr_t) &SigningOutfromApp},
        {"hasTouchScreen", (uintptr_t) &hasTouchScreen},
        {"IsProfileStatsBusy", (uintptr_t) &ret1},
        {"_Z15EnterSocialCLubv", (uintptr_t) &ret0},
        {"_Z12IsSCSignedInv", (uintptr_t) &ret0},

        {"pthread_attr_destroy", (uintptr_t) &ret0},
        {"pthread_cond_init", (uintptr_t) &ret0},
        {"pthread_create", (uintptr_t) &pthread_create_fake},
        {"pthread_getspecific", (uintptr_t) &ret0},
        {"pthread_key_create", (uintptr_t) &ret0},
        {"pthread_mutexattr_init", (uintptr_t) &ret0},
        {"pthread_mutexattr_settype", (uintptr_t) &ret0},
        {"pthread_mutexattr_destroy", (uintptr_t) &ret0},
        {"pthread_mutex_destroy", (uintptr_t) &pthread_mutex_destroy_fake},
        {"pthread_mutex_init", (uintptr_t) &pthread_mutex_init_fake},
        {"pthread_mutex_lock", (uintptr_t) &pthread_mutex_lock_fake},
        {"pthread_mutex_unlock", (uintptr_t) &pthread_mutex_unlock_fake},
        {"pthread_setspecific", (uintptr_t) &ret0},

        {"sem_destroy", (uintptr_t) &sem_destroy_fake},
        // { "sem_getvalue", (uintptr_t)&sem_getvalue },
        {"sem_init", (uintptr_t) &sem_init_fake},
        {"sem_post", (uintptr_t) &sem_post_fake},
        // { "sem_trywait", (uintptr_t)&sem_trywait },
        {"sem_wait", (uintptr_t) &sem_wait_fake},

        {"sigaction", (uintptr_t) &ret0},
        {"sigemptyset", (uintptr_t) &ret0},

        {"acosf", (uintptr_t) &acosf},
        {"asinf", (uintptr_t) &asinf},
        {"atan2f", (uintptr_t) &atan2f},
        {"atanf", (uintptr_t) &atanf},
        {"ceilf", (uintptr_t) &ceilf},
        {"cos", (uintptr_t) &cos},
        {"cosf", (uintptr_t) &cosf},
        {"exp2", (uintptr_t) &exp2},
        {"exp2f", (uintptr_t) &exp2f},
        {"exp", (uintptr_t) &exp},
        {"floor", (uintptr_t) &floor},
        {"floorf", (uintptr_t) &floorf},
        {"fmodf", (uintptr_t) &fmodf},
        {"ldexpf", (uintptr_t) &ldexpf},
        {"log10f", (uintptr_t) &log10f},
        {"log", (uintptr_t) &log},
        {"logf", (uintptr_t) &logf},
        {"modf", (uintptr_t) &modf},
        {"modff", (uintptr_t) &modff},
        {"pow", (uintptr_t) &pow},
        {"powf", (uintptr_t) &powf},
        {"sin", (uintptr_t) &sin},
        {"sinf", (uintptr_t) &sinf},
        {"tan", (uintptr_t) &tan},
        {"tanf", (uintptr_t) &tanf},

        {"atof", (uintptr_t) &atof},
        {"atoi", (uintptr_t) &atoi},

        {"islower", (uintptr_t) &islower},
        {"isprint", (uintptr_t) &isprint},
        {"isspace", (uintptr_t) &isspace},

        {"calloc", (uintptr_t) &calloc},
        {"free", (uintptr_t) &free},
        {"malloc", (uintptr_t) &malloc},
        {"realloc", (uintptr_t) &realloc},

        // { "clock_gettime", (uintptr_t)&clock_gettime },
        {"ctime", (uintptr_t) &ctime},
        {"gettimeofday", (uintptr_t) &gettimeofday},
        {"gmtime", (uintptr_t) &gmtime},
        {"localtime_r", (uintptr_t) &localtime_r},
        {"time", (uintptr_t) &time},

        {"eglGetDisplay", (uintptr_t) &ret0},
        {"eglGetProcAddress", (uintptr_t) &ret0},
        {"eglQueryString", (uintptr_t) &ret0},

        {"abort", (uintptr_t) &abort},
        {"exit", (uintptr_t) &exit},

        {"fclose", (uintptr_t) &sceLibcBridge_fclose},
        // { "fdopen", (uintptr_t)&fdopen },
        // { "fegetround", (uintptr_t)&fegetround },
        {"feof", (uintptr_t) &sceLibcBridge_feof},
        {"ferror", (uintptr_t) &sceLibcBridge_ferror},
        // { "fesetround", (uintptr_t)&fesetround },
        // { "fflush", (uintptr_t)&fflush },
        // { "fgetc", (uintptr_t)&fgetc },
        // { "fgets", (uintptr_t)&fgets },
        {"fopen", (uintptr_t) &sceLibcBridge_fopen},
        {"fprintf", (uintptr_t) &fprintf},
        {"fputc", (uintptr_t) &fputc},
        // { "fputs", (uintptr_t)&fputs },
        // { "fputwc", (uintptr_t)&fputwc },
        {"fread", (uintptr_t) &sceLibcBridge_fread},
        {"fseek", (uintptr_t) &sceLibcBridge_fseek},
        {"ftell", (uintptr_t) &sceLibcBridge_ftell},
        {"fwrite", (uintptr_t) &sceLibcBridge_fwrite},

        {"getenv", (uintptr_t) &getenv},
        // { "gettid", (uintptr_t)&gettid },

        {"glActiveTexture", (uintptr_t) &glActiveTexture},
        {"glAttachShader", (uintptr_t) &glAttachShader},
        {"glBindAttribLocation", (uintptr_t) &glBindAttribLocation},
        {"glBindBuffer", (uintptr_t) &glBindBuffer},
        {"glBindFramebuffer", (uintptr_t) &glBindFramebuffer},
        {"glBindRenderbuffer", (uintptr_t) &ret0},
        {"glBindTexture", (uintptr_t) &glBindTexture},
        {"glBlendFunc", (uintptr_t) &glBlendFunc},
        {"glBlendFuncSeparate", (uintptr_t) &glBlendFuncSeparate},
        {"glBufferData", (uintptr_t) &glBufferData},
        {"glCheckFramebufferStatus", (uintptr_t) &glCheckFramebufferStatus},
        {"glClear", (uintptr_t) &glClear},
        {"glClearColor", (uintptr_t) &glClearColor},
        {"glClearDepthf", (uintptr_t) &glClearDepthf},
        {"glClearStencil", (uintptr_t) &glClearStencil},
        {"glCompileShader", (uintptr_t) &glCompileShader},
        {"glCompressedTexImage2D", (uintptr_t) &glCompressedTexImage2DHook},
        {"glCreateProgram", (uintptr_t) &glCreateProgram},
        {"glCreateShader", (uintptr_t) &glCreateShader},
        {"glCullFace", (uintptr_t) &glCullFace},
        {"glDeleteBuffers", (uintptr_t) &glDeleteBuffers},
        {"glDeleteFramebuffers", (uintptr_t) &glDeleteFramebuffers},
        {"glDeleteProgram", (uintptr_t) &glDeleteProgram},
        {"glDeleteRenderbuffers", (uintptr_t) &ret0},
        {"glDeleteShader", (uintptr_t) &glDeleteShader},
        {"glDeleteTextures", (uintptr_t) &glDeleteTextures},
        {"glDepthFunc", (uintptr_t) &glDepthFunc},
        {"glDepthMask", (uintptr_t) &glDepthMask},
        {"glDisable", (uintptr_t) &glDisable},
        {"glDisableVertexAttribArray", (uintptr_t) &glDisableVertexAttribArray},
        {"glDrawArrays", (uintptr_t) &glDrawArrays},
        {"glDrawElements", (uintptr_t) &glDrawElements},
        {"glEnable", (uintptr_t) &glEnable},
        {"glEnableVertexAttribArray", (uintptr_t) &glEnableVertexAttribArray},
        {"glFramebufferRenderbuffer", (uintptr_t) &ret0},
        {"glFramebufferTexture2D", (uintptr_t) &glFramebufferTexture2D},
        {"glFrontFace", (uintptr_t) &glFrontFace},
        {"glGenBuffers", (uintptr_t) &glGenBuffers},
        {"glGenFramebuffers", (uintptr_t) &glGenFramebuffers},
        {"glGenRenderbuffers", (uintptr_t) &ret0},
        {"glGenTextures", (uintptr_t) &glGenTextures},
        {"glGetAttribLocation", (uintptr_t) &glGetAttribLocation},
        {"glGetError", (uintptr_t) &glGetError},
        {"glGetIntegerv", (uintptr_t) &glGetIntegerv},
        {"glGetProgramInfoLog", (uintptr_t) &glGetProgramInfoLog},
        {"glGetProgramiv", (uintptr_t) &glGetProgramiv},
        {"glGetShaderInfoLog", (uintptr_t) &glGetShaderInfoLog},
        {"glGetShaderiv", (uintptr_t) &glGetShaderiv},
        {"glGetString", (uintptr_t) &glGetString},
        {"glGetUniformLocation", (uintptr_t) &glGetUniformLocation},
        {"glHint", (uintptr_t) &glHint},
        {"glLinkProgram", (uintptr_t) &glLinkProgram},
        {"glPolygonOffset", (uintptr_t) &glPolygonOffset},
        {"glReadPixels", (uintptr_t) &glReadPixels},
        {"glRenderbufferStorage", (uintptr_t) &ret0},
        {"glScissor", (uintptr_t) &glScissor},
        {"glShaderSource", (uintptr_t) &glShaderSource},
        {"glTexImage2D", (uintptr_t) &glTexImage2DHook},
        {"glTexParameterf", (uintptr_t) &glTexParameterf},
        {"glTexParameteri", (uintptr_t) &glTexParameteri},
        {"glUniform1fv", (uintptr_t) &glUniform1fv},
        {"glUniform1i", (uintptr_t) &glUniform1i},
        {"glUniform2fv", (uintptr_t) &glUniform2fv},
        {"glUniform3fv", (uintptr_t) &glUniform3fv},
        {"glUniform4fv", (uintptr_t) &glUniform4fv},
        {"glUniformMatrix3fv", (uintptr_t) &glUniformMatrix3fv},
        {"glUniformMatrix4fv", (uintptr_t) &glUniformMatrix4fv},
        {"glUseProgram", (uintptr_t) &glUseProgram},
        {"glVertexAttrib4fv", (uintptr_t) &glVertexAttrib4fv},
        {"glVertexAttribPointer", (uintptr_t) &glVertexAttribPointer},
        {"glViewport", (uintptr_t) &glViewport},

        {"longjmp", (uintptr_t) &longjmp},
        {"setjmp", (uintptr_t) &setjmp},

        {"memchr", (uintptr_t) &sceClibMemchr},
        {"memcmp", (uintptr_t) &sceClibMemcmp},

        {"puts", (uintptr_t) &puts},
        {"qsort", (uintptr_t) &qsort},

        // { "raise", (uintptr_t)&raise },
        // { "rewind", (uintptr_t)&rewind },

        {"rand", (uintptr_t) &rand},
        {"srand", (uintptr_t) &srand},

        {"sscanf", (uintptr_t) &sscanf},

        // { "close", (uintptr_t)&close },
        // { "closedir", (uintptr_t)&closedir },
        // { "lseek", (uintptr_t)&lseek },
        {"mkdir", (uintptr_t) &mkdir},
        // { "open", (uintptr_t)&open },
        // { "opendir", (uintptr_t)&opendir },
        // { "read", (uintptr_t)&read },
        // { "readdir", (uintptr_t)&readdir },
        // { "remove", (uintptr_t)&remove },
        {"stat", (uintptr_t) &stat_hook},

        {"stderr", (uintptr_t) &stderr_fake},
        {"strcasecmp", (uintptr_t) &strcasecmp},
        {"strcat", (uintptr_t) &strcat},
        {"strchr", (uintptr_t) &strchr},
        {"strcmp", (uintptr_t) &sceClibStrcmp},
        {"strcpy", (uintptr_t) &strcpy},
        {"strerror", (uintptr_t) &strerror},
        {"strlen", (uintptr_t) &strlen},
        {"strncasecmp", (uintptr_t) &sceClibStrncasecmp},
        {"strncat", (uintptr_t) &sceClibStrncat},
        {"strncmp", (uintptr_t) &sceClibStrncmp},
        {"strncpy", (uintptr_t) &sceClibStrncpy},
        {"strpbrk", (uintptr_t) &strpbrk},
        {"strrchr", (uintptr_t) &sceClibStrrchr},
        {"strstr", (uintptr_t) &sceClibStrstr},
        {"strtof", (uintptr_t) &strtof},
        {"strtok", (uintptr_t) &strtok},
        {"strtol", (uintptr_t) &strtol},
        {"strtoul", (uintptr_t) &strtoul},

        {"toupper", (uintptr_t) &toupper},
        {"vasprintf", (uintptr_t) &vasprintf},

        // { "nanosleep", (uintptr_t)&nanosleep },
        {"usleep", (uintptr_t) &usleep},
};

int check_kubridge(void) {
    int search_unk[2];
    return _vshKernelSearchModuleByName("kubridge", search_unk);
}

int file_exists(const char *path) {
    SceIoStat stat;
    return sceIoGetstat(path, &stat) >= 0;
}

int main(int argc, char *argv[]) {
    // Check if we want to start the companion app
    sceAppUtilInit(&(SceAppUtilInitParam) {}, &(SceAppUtilBootParam) {});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/companion.bin", NULL, NULL);
    }

    sceKernelChangeThreadPriority(0, 127);
    sceKernelChangeThreadCpuAffinityMask(0, 0x40000);

    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK,
                             SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &panelInfoFront);
    sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &panelInfoBack);

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    read_config(CONFIG_PATH);
    read_controller_config(CONTROLLER_CONFIG_PATH);

    if (check_kubridge() < 0)
        fatal_error("Error kubridge.skprx is not installed.");

    if (!file_exists("ur0:/data/libshacccg.suprx") &&
        !file_exists("ur0:/data/external/libshacccg.suprx"))
        fatal_error("Error libshacccg.suprx is not installed.");

    if (!file_exists(RT_INI_PATH))
        fatal_error("No smoke. No streets.\nMissing files at %s.", DATA_PATH);

    if (so_load(&gtasa_mod, SO_PATH, LOAD_ADDRESS) < 0)
        fatal_error("Error could not load %s.", SO_PATH);

    stderr_fake = stderr;
    so_relocate(&gtasa_mod);
    so_resolve(&gtasa_mod, default_dynlib, sizeof(default_dynlib), 0);

    patch_mpg123();
    patch_openal();
    patch_opengl();
    patch_game();
    patch_gfx();
    patch_scripts();
    so_flush_caches(&gtasa_mod);

    so_initialize(&gtasa_mod);

    if (fios_init() < 0)
        fatal_error("Error could not initialize fios.");

    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE,
                                  SHARK_ENABLE);
    vglSetVDMBufferSize(512 * 1024); // default 128 * 1024
    vglSetVertexBufferSize(8 * 1024 * 1024); // default 2 * 1024 * 1024
    vglSetFragmentBufferSize(2 * 1024 * 1024); // default 512 * 1024
    vglSetUSSEBufferSize(64 * 1024); // default 16 * 1024
    vglSetVertexPoolSize(48 * 1024 * 1024);
    vglSetupGarbageCollector(127, 0x20000);
    int has_low_res = vglInitExtended(0, SCREEN_W, SCREEN_H,
                                      MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024,
                                      config.aa_mode);
    if (has_low_res) {
        SCREEN_W = DEF_SCREEN_W;
        SCREEN_H = DEF_SCREEN_H;
    }

    jni_load();

    return 0;
}
