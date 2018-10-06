#include "copyright"

#include <stdio.h>

#include "snes9x.h"
#include "soundux.h"
#include "memmap.h"
#include "apu.h"
#include "cheats.h"
#include "display.h"
#include "gfx.h"
#include "cpuexec.h"
#include "spc7110.h"
#include "srtc.h"
#include "sa1.h"

#ifdef PSP
#include <pspkernel.h>
#include <pspgu.h>
#endif

#include <libretro.h>

#ifdef _3DS
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);
#endif

static retro_log_printf_t log_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_environment_t environ_cb = NULL;
struct retro_perf_callback perf_cb;

char retro_save_directory[PATH_MAX];
char retro_base_name[PATH_MAX];

#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

static int32_t samples_per_frame = 0;
static int32_t samplerate = (((SNES_CLOCK_SPEED * 6) / (32 * ONE_APU_CYCLE)));

#ifdef PERF_TEST
#define RETRO_PERFORMANCE_INIT(name) \
    retro_perf_tick_t current_ticks; \
    static struct retro_perf_counter name = {#name}; \
    if (!name.registered) \
       perf_cb.perf_register(&(name)); \
    current_ticks = name.total

#define RETRO_PERFORMANCE_START(name) \
    perf_cb.perf_start(&(name))

#define RETRO_PERFORMANCE_STOP(name) \
    perf_cb.perf_stop(&(name)); \
    current_ticks = name.total - current_ticks;
#else
#define RETRO_PERFORMANCE_INIT(name)
#define RETRO_PERFORMANCE_START(name)
#define RETRO_PERFORMANCE_STOP(name)
#endif

void retro_set_environment(retro_environment_t cb)
{
   struct retro_log_callback log;
   environ_cb = cb;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   (void) cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
   (void) in_port;
   (void) device;
}

unsigned retro_api_version()
{
   return RETRO_API_VERSION;
}

void S9xDeinitDisplay(void)
{
#ifdef DS2_DMA
   if (GFX.Screen_buffer)
      AlignedFree(GFX.Screen, PtrAdj.GFXScreen);
#elif defined(_3DS)
   if (GFX.Screen_buffer)
      linearFree(GFX.Screen_buffer);
#else
   if (GFX.Screen_buffer)
      free(GFX.Screen_buffer);
#endif
   if (GFX.SubScreen_buffer)
      free(GFX.SubScreen_buffer);
   if (GFX.ZBuffer_buffer)
      free(GFX.ZBuffer_buffer);
   if (GFX.SubZBuffer_buffer)
      free(GFX.SubZBuffer_buffer);

   GFX.Screen = NULL;
   GFX.Screen_buffer = NULL;
   GFX.SubScreen = NULL;
   GFX.SubScreen_buffer = NULL;
   GFX.ZBuffer = NULL;
   GFX.ZBuffer_buffer = NULL;
   GFX.SubZBuffer = NULL;
   GFX.SubZBuffer_buffer = NULL;
}

void S9xInitDisplay(void)
{
   int32_t h = IMAGE_HEIGHT;
   int32_t safety = 32;

   GFX.Pitch = IMAGE_WIDTH * 2;
#ifdef DS2_DMA
   GFX.Screen_buffer = (uint8_t *) AlignedMalloc(GFX.Pitch * h + safety, 32, &PtrAdj.GFXScreen);
#elif defined(_3DS)
   safety = 0x80;
   GFX.Screen_buffer = (uint8_t *) linearMemAlign(GFX.Pitch * h + safety, 0x80);
#else
   GFX.Screen_buffer = (uint8_t *) malloc(GFX.Pitch * h + safety);
#endif
   GFX.SubScreen_buffer = (uint8_t *) malloc(GFX.Pitch * h + safety);
   GFX.ZBuffer_buffer = (uint8_t *) malloc((GFX.Pitch >> 1) * h + safety);
   GFX.SubZBuffer_buffer = (uint8_t *) malloc((GFX.Pitch >> 1) * h + safety);

   GFX.Screen = GFX.Screen_buffer + safety;
   GFX.SubScreen = GFX.SubScreen_buffer + safety;
   GFX.ZBuffer = GFX.ZBuffer_buffer + safety;
   GFX.SubZBuffer = GFX.SubZBuffer_buffer + safety;

   GFX.Delta = (GFX.SubScreen - GFX.Screen) >> 1;
}

#ifndef __WIN32__
void _makepath(char* path, const char* drive, const char* dir, const char* fname, const char* ext)
{
   (void) drive;

   if (dir && *dir)
   {
      strcpy(path, dir);
      strcat(path, "/");
   }
   else
      *path = 0;

   if (fname)
      strcat(path, fname);

   if (ext && *ext)
   {
      strcat(path, ".");
      strcat(path, ext);
   }
}

void _splitpath (const char* path, char* drive, char* dir, char* fname, char* ext)
{
   const char* slash = strrchr(path, '/');
   const char* dot   = strrchr(path, '.');
   (void) drive;

   if (!slash)
      slash = strrchr((char*)path, '\\');

   if (dot && slash && dot < slash)
      dot = NULL;

   if (!slash)
   {
      *dir = 0;
      strcpy(fname, path);
      if (dot)
      {
         fname[dot - path] = 0;
         strcpy(ext, dot + 1);
      }
      else
         *ext = 0;
   }
   else
   {
      strcpy(dir, path);
      dir[slash - path] = 0;
      strcpy(fname, slash + 1);
      if (dot)
      {
         fname[dot - slash - 1] = 0;
         strcpy(ext, dot + 1);
      }
      else
         *ext = 0;
   }
}
#endif

const char* S9xGetFilename(const char* in)
{
   static char filename [PATH_MAX + 1];
   char drive [_MAX_DRIVE + 1];
   char dir [_MAX_DIR + 1];
   char fname [_MAX_FNAME + 1];
   char ext [_MAX_EXT + 1];
   _splitpath(Memory.ROMFilename, drive, dir, fname, ext);
   _makepath(filename, drive, dir, fname, in);
   return filename;
}

void GetBaseName(const char* ex)
{
   char drive [_MAX_DRIVE + 1];
   char dir [_MAX_DIR + 1];
   char fname [_MAX_FNAME + 1];
   char ext [_MAX_EXT + 1];
   (void) ex;
   _splitpath(Memory.ROMFilename, drive, dir, fname, ext);
   snprintf(retro_base_name,sizeof(retro_base_name),"%s",fname);
}

void init_sfc_setting(void)
{
   memset(&Settings, 0, sizeof(Settings));
   Settings.JoystickEnabled = false;
   Settings.SoundPlaybackRate = samplerate;
   Settings.CyclesPercentage = 100;

   Settings.DisableSoundEcho = false;
   Settings.InterpolatedSound = true;
   Settings.APUEnabled = true;

   Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
   Settings.FrameTimePAL = 20000;
   Settings.FrameTimeNTSC = 16667;
   Settings.DisableMasterVolume = false;
   Settings.Mouse = true;
   Settings.SuperScope = true;
   Settings.MultiPlayer5 = true;
   Settings.ControllerOption = SNES_JOYPAD;
#ifdef USE_BLARGG_APU
   Settings.SoundSync = false;
#endif
   Settings.ApplyCheats = true;
   Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
}

#ifdef USE_BLARGG_APU
static void S9xAudioCallback()
{
   size_t avail;
   /* Just pick a big buffer. We won't use it all. */
   static int16_t audio_buf[0x20000];

   S9xFinalizeSamples();
   avail = S9xGetSampleCount();
   S9xMixSamples(audio_buf, avail);
   audio_batch_cb(audio_buf, avail >> 1);
}
#endif

void retro_init(void)
{
   struct retro_log_callback log;
   enum retro_pixel_format rgb565;
   bool achievements = true;

   static const struct retro_variable vars[] =
   {
      { "catsfc_VideoMode", "Video Mode; auto|NTSC|PAL" },
      { NULL, NULL },
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   /* State that the core supports achievements. */
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

   rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");

   init_sfc_setting();
   S9xInitMemory();
   S9xInitAPU();
   S9xInitDisplay();
   S9xInitGFX();
#ifdef USE_BLARGG_APU
   S9xInitSound(1000, 0); /* just give it a 1 second buffer */
   S9xSetSamplesAvailableCallback(S9xAudioCallback);
#else
   S9xInitSound();
#endif
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
   CPU.SaveStateVersion = 0;
}

void retro_deinit(void)
{
   if (Settings.SPC7110)
      Del7110Gfx();

   S9xDeinitGFX();
   S9xDeinitDisplay();
   S9xDeinitAPU();
   S9xDeinitMemory();

#ifdef PERF_TEST
   perf_cb.perf_log();
#endif
}

uint32_t S9xReadJoypad(int32_t port)
{
   static const uint32_t snes_lut[] =
   {
      SNES_B_MASK,
      SNES_Y_MASK,
      SNES_SELECT_MASK,
      SNES_START_MASK,
      SNES_UP_MASK,
      SNES_DOWN_MASK,
      SNES_LEFT_MASK,
      SNES_RIGHT_MASK,
      SNES_A_MASK,
      SNES_X_MASK,
      SNES_TL_MASK,
      SNES_TR_MASK
   };

   int32_t i;
   uint32_t joypad = 0;

   for (i = RETRO_DEVICE_ID_JOYPAD_B; i <= RETRO_DEVICE_ID_JOYPAD_R; i++)
      if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, i))
         joypad |= snes_lut[i];

   return joypad;
}

static void check_variables(void)
{
   struct retro_variable var;

   var.key = "catsfc_VideoMode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      Settings.ForceNTSC = !strcmp(var.value, "NTSC");
      Settings.ForcePAL  = !strcmp(var.value, "PAL");
   }
}

#ifdef PSP
#define FRAMESKIP
#endif

static int32_t samples_to_play = 0;
void retro_run(void)
{
   bool updated = false;
#ifndef USE_BLARGG_APU
   static int16_t audio_buf[2048];
#endif

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

#ifdef NO_VIDEO_OUTPUT
   video_cb(NULL, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, GFX.Pitch);
   IPPU.RenderThisFrame = false;
#endif

   poll_cb();

   RETRO_PERFORMANCE_INIT(S9xMainLoop_func);
   RETRO_PERFORMANCE_START(S9xMainLoop_func);
   S9xMainLoop();
   RETRO_PERFORMANCE_STOP(S9xMainLoop_func);

#ifndef USE_BLARGG_APU
   samples_to_play += samples_per_frame;

   if (samples_to_play > 512)
   {
      S9xMixSamples(audio_buf, samples_to_play * 2);
      audio_batch_cb(audio_buf, samples_to_play);
      samples_to_play = 0;
   }
#endif

#ifdef NO_VIDEO_OUTPUT
   return;
#endif

#ifdef FRAMESKIP
   if (IPPU.RenderThisFrame)
   {
#endif

#ifdef PSP
      static unsigned int __attribute__((aligned(16))) d_list[32];
      void* const texture_vram_p = (void*)(0x44200000 - (512 * 512)); /* max VRAM address - frame size */
      sceKernelDcacheWritebackRange(GFX.Screen, GFX.Pitch * IPPU.RenderedScreenHeight);
      sceGuStart(GU_DIRECT, d_list);
      sceGuCopyImage(GU_PSM_4444, 0, 0, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, GFX.Pitch >> 1, GFX.Screen, 0, 0, 512, texture_vram_p);
      sceGuTexSync();
      sceGuTexImage(0, 512, 512, 512, texture_vram_p);
      sceGuTexMode(GU_PSM_5551, 0, 0, GU_FALSE);
      sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
      sceGuDisable(GU_BLEND);
      sceGuFinish();
      video_cb(texture_vram_p, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, GFX.Pitch);
#else
      video_cb(GFX.Screen, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, GFX.Pitch);
#endif

#ifdef FRAMESKIP
      IPPU.RenderThisFrame = false;
   }
   else
   {
      video_cb(NULL, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, GFX.Pitch);
      IPPU.RenderThisFrame = true;
   }
#endif
}

bool S9xReadMousePosition(int32_t which1, int32_t* x, int32_t* y, uint32_t* buttons)
{
   (void) which1;
   (void) x;
   (void) y;
   (void) buttons;
   return false;
}

bool S9xReadSuperScopePosition(int32_t* x, int32_t* y, uint32_t* buttons)
{
   (void) x;
   (void) y;
   (void) buttons;
   return true;
}

bool JustifierOffscreen(void)
{
   return false;
}

void JustifierButtons(uint32_t* justifiers)
{
   (void) justifiers;
}

unsigned retro_get_region(void)
{
   return Settings.PAL ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void retro_get_system_info(struct retro_system_info* info)
{
#ifdef LOAD_FROM_MEMORY_TEST
   info->need_fullpath    = false;
#else
   info->need_fullpath    = true;
#endif
   info->valid_extensions = "smc|fig|sfc|gd3|gd7|dx2|bsx|swc";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = "v1.36" GIT_VERSION;
#ifdef USE_BLARGG_APU
   info->library_name     = "Snes9x 2005 Plus";
#else
   info->library_name     = "Snes9x 2005";
#endif
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
   info->geometry.base_width = 256;
   info->geometry.base_height = 224;
   info->geometry.max_width = 512;
   info->geometry.max_height = 512;
   info->geometry.aspect_ratio = 4.0 / 3.0;

   if (!Settings.PAL)
      info->timing.fps = (SNES_CLOCK_SPEED * 6.0 / (SNES_CYCLES_PER_SCANLINE * SNES_MAX_NTSC_VCOUNTER));
   else
      info->timing.fps = (SNES_CLOCK_SPEED * 6.0 / (SNES_CYCLES_PER_SCANLINE * SNES_MAX_PAL_VCOUNTER));

   info->timing.sample_rate = samplerate;
}

void retro_reset(void)
{
   CPU.Flags = 0;
   S9xReset();
}

size_t retro_serialize_size(void)
{
   return sizeof(CPU) + sizeof(ICPU) + sizeof(PPU) + sizeof(DMA) +
          0x10000 + 0x20000 + 0x20000 + 0x8000 +
#ifndef USE_BLARGG_APU
          sizeof(APU) + sizeof(IAPU) + 0x10000 +
#else
          SPC_SAVE_STATE_BLOCK_SIZE +
#endif
          sizeof(SA1) + sizeof(s7r) + sizeof(rtc_f9);
}

bool retro_serialize(void* data, size_t size)
{
   uint8_t* buffer = data;
   (void) size;
#ifdef LAGFIX
   S9xPackStatus();
#ifndef USE_BLARGG_APU
   S9xAPUPackStatus();
#endif
#endif
   S9xUpdateRTC();
   S9xSRTCPreSaveState();
   memcpy(buffer, &CPU, sizeof(CPU));
   buffer += sizeof(CPU);
   memcpy(buffer, &ICPU, sizeof(ICPU));
   buffer += sizeof(ICPU);
   memcpy(buffer, &PPU, sizeof(PPU));
   buffer += sizeof(PPU);
   memcpy(buffer, &DMA, sizeof(DMA));
   buffer += sizeof(DMA);
   memcpy(buffer, Memory.VRAM, 0x10000);
   buffer += 0x10000;
   memcpy(buffer, Memory.RAM, 0x20000);
   buffer += 0x20000;
   memcpy(buffer, Memory.SRAM, 0x20000);
   buffer += 0x20000;
   memcpy(buffer, Memory.FillRAM, 0x8000);
   buffer += 0x8000;
#ifndef USE_BLARGG_APU
   memcpy(buffer, &APU, sizeof(APU));
   buffer += sizeof(APU);
   memcpy(buffer, &IAPU, sizeof(IAPU));
   buffer += sizeof(IAPU);
   memcpy(buffer, IAPU.RAM, 0x10000);
   buffer += 0x10000;
#else
   S9xAPUSaveState(buffer);
   buffer += SPC_SAVE_STATE_BLOCK_SIZE;
#endif

   SA1.Registers.PC = SA1.PC - SA1.PCBase;
   S9xSA1PackStatus();

   memcpy(buffer, &SA1, sizeof(SA1));
   buffer += sizeof(SA1);
   memcpy(buffer, &s7r, sizeof(s7r));
   buffer += sizeof(s7r);
   memcpy(buffer, &rtc_f9, sizeof(rtc_f9));

   return true;
}

bool retro_unserialize(const void* data, size_t size)
{
   const uint8_t* buffer = data;
#ifndef USE_BLARGG_APU
   uint8_t* IAPU_RAM_current = IAPU.RAM;
   uintptr_t IAPU_RAM_offset;
#endif

   if (size != retro_serialize_size())
      return false;

   S9xReset();
   memcpy(&CPU, buffer, sizeof(CPU));
   buffer += sizeof(CPU);
   memcpy(&ICPU, buffer, sizeof(ICPU));
   buffer += sizeof(ICPU);
   memcpy(&PPU, buffer, sizeof(PPU));
   buffer += sizeof(PPU);
   memcpy(&DMA, buffer, sizeof(DMA));
   buffer += sizeof(DMA);
   memcpy(Memory.VRAM, buffer, 0x10000);
   buffer += 0x10000;
   memcpy(Memory.RAM, buffer, 0x20000);
   buffer += 0x20000;
   memcpy(Memory.SRAM, buffer, 0x20000);
   buffer += 0x20000;
   memcpy(Memory.FillRAM, buffer, 0x8000);
   buffer += 0x8000;
#ifndef USE_BLARGG_APU
   memcpy(&APU, buffer, sizeof(APU));
   buffer += sizeof(APU);
   memcpy(&IAPU, buffer, sizeof(IAPU));
   buffer += sizeof(IAPU);
   IAPU_RAM_offset = IAPU_RAM_current - IAPU.RAM;
   IAPU.PC += IAPU_RAM_offset;
   IAPU.DirectPage += IAPU_RAM_offset;
   IAPU.WaitAddress1 += IAPU_RAM_offset;
   IAPU.WaitAddress2 += IAPU_RAM_offset;
   IAPU.RAM = IAPU_RAM_current;
   memcpy(IAPU.RAM, buffer, 0x10000);
   buffer += 0x10000;
#else
   S9xAPULoadState(buffer);
   buffer += SPC_SAVE_STATE_BLOCK_SIZE;
#endif

   memcpy(&SA1, buffer, sizeof(SA1));
   buffer += sizeof(SA1);
   memcpy(&s7r, buffer, sizeof(s7r));
   buffer += sizeof(s7r);
   memcpy(&rtc_f9, buffer, sizeof(rtc_f9));

   S9xFixSA1AfterSnapshotLoad();
   FixROMSpeed();
   IPPU.ColorsChanged = true;
   IPPU.OBJChanged = true;
   CPU.InDMA = false;
   S9xFixColourBrightness();
   S9xSA1UnpackStatus();
#ifndef USE_BLARGG_APU
   S9xAPUUnpackStatus();
   S9xFixSoundAfterSnapshotLoad();
#endif
   ICPU.ShiftedPB = ICPU.Registers.PB << 16;
   ICPU.ShiftedDB = ICPU.Registers.DB << 16;
   S9xSetPCBase(ICPU.ShiftedPB + ICPU.Registers.PC);
   S9xUnpackStatus();
   S9xFixCycles();
   S9xReschedule();
   return true;
}

void retro_cheat_reset(void)
{
   S9xDeleteCheats();
   S9xApplyCheats();
}

extern SCheatData Cheat;

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
   uint32_t address;
   uint8_t val;

   bool sram;
   uint8_t bytes[3];/* used only by GoldFinger, ignored for now */

   if (S9xGameGenieToRaw(code, &address, &val) && S9xProActionReplayToRaw(code, &address, &val) && S9xGoldFingerToRaw(code, &address, &sram, &val, bytes))
      return; /* bad code, ignore */
   if (index > Cheat.num_cheats)
      return; /* cheat added in weird order, ignore */
   if (index == Cheat.num_cheats)
      Cheat.num_cheats++;

   Cheat.c[index].address = address;
   Cheat.c[index].byte = val;
   Cheat.c[index].enabled = enabled;
   Cheat.c[index].saved = false; /* it'll be saved next time cheats run anyways */

   Settings.ApplyCheats = true;
   S9xApplyCheats();
}

static void init_descriptors(void)
{
   struct retro_input_descriptor desc[] =
   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

bool retro_load_game(const struct retro_game_info* game)
{
   struct retro_system_av_info av_info;
   if (!game)
      return false;

   CPU.Flags = 0;
   init_descriptors();
   check_variables();

#ifdef LOAD_FROM_MEMORY_TEST
   if (!LoadROM(game))
#else
   if (!LoadROM(game->path))
#endif
      return false;

   Settings.FrameTime = (Settings.PAL ? Settings.FrameTimePAL : Settings.FrameTimeNTSC);

   retro_get_system_av_info(&av_info);

#ifdef USE_BLARGG_APU
   Settings.SoundPlaybackRate = av_info.timing.sample_rate;
#else
   samples_per_frame = av_info.timing.sample_rate / av_info.timing.fps;
   S9xSetPlaybackRate(Settings.SoundPlaybackRate);
#endif
   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
   (void) game_type;
   (void) info;
   (void) num_info;
   return false;
}

void retro_unload_game(void)
{
}

void* retro_get_memory_data(unsigned type)
{
   uint8_t* data;

   switch(type)
   {
      case RETRO_MEMORY_SAVE_RAM:
         data = Memory.SRAM;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         data = Memory.RAM;
         break;
      case RETRO_MEMORY_VIDEO_RAM:
         data = Memory.VRAM;
         break;
      default:
         data = NULL;
         break;
   }

   return data;
}

size_t retro_get_memory_size(unsigned type)
{
   uint32_t size;

   switch(type)
   {
      case RETRO_MEMORY_SAVE_RAM:
         size = (uint32_t) (Memory.SRAMSize ? (1 << (Memory.SRAMSize + 3)) * 128 : 0);
         if (size > 0x20000)
            size = 0x20000;
         break;
      case RETRO_MEMORY_RTC:
         size = (Settings.SRTC || Settings.SPC7110RTC) ? 20 : 0;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         size = 128 * 1024;
         break;
      case RETRO_MEMORY_VIDEO_RAM:
         size = 64 * 1024;
         break;
      default:
         size = 0;
         break;
   }

   return size;
}
