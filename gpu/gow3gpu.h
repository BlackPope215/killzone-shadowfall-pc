/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef GOW3GPU_H
#define GOW3GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} Gow3GpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void gow3gpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int gow3gpu_init(const Gow3GpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t gow3gpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int gow3gpu_handle_fault(void *ucontext, void *address);
/* GOW3_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void gow3gpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int gow3gpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int gow3gpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int gow3gpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int gow3gpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int gow3gpu_overlay_captures_input(void);
/* Mouse look (relative mode while the game window has the focus): configure once; sample takes
 * the motion (pixels) and wheel steps since the last call and the buttons held (1 left, 2 right,
 * 4 middle, 8 and 16 side). Returns 1 while the mouse is captured. */
void gow3gpu_mouse_configure(int look);
/* Where the guest image starts (fault diagnostics and per-instruction fault policies). */
void gow3gpu_set_guest_image(const void *base);
int gow3gpu_mouse_sample(float *dx, float *dy, uint32_t *buttons, int *wheel);
/* Controller vibration strength set in the menu, 0 (off) to 100 (the game's own). */
int gow3gpu_vibration_percent(void);
/* Controls set in the launcher or the in-game menu (Controls tab), read live by the pad code. */
typedef struct {
    float mouse_sens;       /* mouse look sensitivity */
    float mouse_turn_boost; /* fast turns: turn speed factor while the mouse moves */
    int mouse_invert;       /* invert the mouse's Y axis */
    float pad_dead_zone;    /* extra stick dead zone, 0..0.3 */
    int quick_menu;         /* hold Select/Back/Share: quick menu */
    int prompt_style;       /* button prompts: 0 last device used, 1 keyboard/mouse, 2 PS4 */
} Gow3InputSettings;
void gow3gpu_input_settings(Gow3InputSettings *out);
/* Controller speaker sounds, 0..100 percent (live). */
int gow3gpu_pad_speaker_percent(void);
/* Opens the quick menu (Resume / Settings / Exit game) over the game. */
void gow3gpu_quick_menu_open(void);
/* The FOV hook's float (pi/180 times the FOV scale) that the menu rewrites when the FOV changes. */
void gow3gpu_fov_register(float *cell);
/* The LOD distance hook's float (scale of the game's LOD distance multipliers) the menu rewrites. */
void gow3gpu_lod_register(float *cell);
/* Post-processing effects turned off in the settings, read once at start (they apply on restart):
 * bit 0 motion blur, 1 depth of field, 2 bloom, 3 film grain (src/postfx_hook.h). */
unsigned gow3gpu_post_effects_off(void);
/* The camera shake hook's int (nonzero: the game's shake, 0: none) that the menu rewrites. */
void gow3gpu_camera_shake_register(int *cell);
/* The scene size cell of the upscaler quality modes (src/scene_res_hook.h Gow3SceneSize: requested
 * width, height, then the applied ones); the menu writes the requested pair to switch live. */
void gow3gpu_scene_size_register(uint32_t *cell);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned gow3gpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
