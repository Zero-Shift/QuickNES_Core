#include "libretro.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include "Nes_Emu.h"
#include "Data_Reader.h"
#include "abstract_file.h"
#include "Nes_Effects_Buffer.h"

#ifdef PSP
#include "pspkernel.h"
#include "pspgu.h"
#endif

#define CORE_VERSION "1.0-WIP"

#define NES_4_3 (4.0 / 3.0)
#define NES_PAR (width * (8.0 / 7.0) / height)

static Nes_Emu *emu;

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static bool aspect_ratio_par;
#ifdef PSP
static bool use_overscan;
#else
static bool use_overscan_v;
static bool use_overscan_h;
#endif
static bool up_down_allowed = false;

const int videoBufferWidth = Nes_Emu::image_width + 16;
const int videoBufferHeight = Nes_Emu::image_height + 2;

Mono_Buffer mono_buffer;
Nes_Buffer nes_buffer;
Nes_Effects_Buffer effects_buffer;
Silent_Buffer silent_buffer;
Multi_Buffer *current_buffer = NULL;
bool use_silent_buffer = false;

bool is_fast_savestate();

void retro_init(void)
{
}

void retro_deinit(void)
{
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned, unsigned)
{
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "QuickNES";
#ifdef GIT_VERSION
   info->library_version  = CORE_VERSION GIT_VERSION;
#else
   info->library_version  = CORE_VERSION;
#endif
   info->need_fullpath    = false;
   info->valid_extensions = "nes"; // Anything is fine, we don't care.
}

float get_aspect_ratio(unsigned width, unsigned height)
{
   return (aspect_ratio_par ? NES_PAR : NES_4_3);
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
#ifdef PSP
   unsigned width  = Nes_Emu::image_width  - (use_overscan   ? 0 : 16);
   unsigned height = Nes_Emu::image_height - (use_overscan   ? 0 : 16);
#else
   unsigned width  = Nes_Emu::image_width  - (use_overscan_h ? 0 : 16);
   unsigned height = Nes_Emu::image_height - (use_overscan_v ? 0 : 16);
#endif

   const retro_system_timing timing = { Nes_Emu::frame_rate, 44100.0 };
   info->timing = timing;

   info->geometry.base_width   = width;
   info->geometry.base_height  = height;
   info->geometry.max_width    = width;
   info->geometry.max_height   = height;
   info->geometry.aspect_ratio = get_aspect_ratio(width, height);
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "quicknes_up_down_allowed", "Allow Opposing Directions; disabled|enabled" },
      { "quicknes_aspect_ratio_par", "Aspect ratio; PAR|4:3" },
#ifndef PSP
      { "quicknes_use_overscan_h", "Show horizontal overscan; enabled|disabled" },
      { "quicknes_use_overscan_v", "Show vertical overscan; disabled|enabled" },
#endif
      { "quicknes_no_sprite_limit", "No sprite limit; enabled|disabled" },
      { "quicknes_audio_nonlinear", "Audio mode; nonlinear|linear|stereo panning"},
      { "quicknes_audio_eq", "Audio equalizer preset; default|famicom|tv|flat|crisp|tinny"},
      { NULL, NULL },
   };

   environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   if (emu)
      emu->reset( false, false );
}

static void update_audio_mode(void)
{
	if (use_silent_buffer)
	{
		emu->set_sample_rate(44100, &silent_buffer);
		current_buffer = &silent_buffer;
		return;
	}
	struct retro_variable var = { 0 };

	var.key = "quicknes_audio_nonlinear";
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (0 == strcmp(var.value, "nonlinear"))
		{
			if (current_buffer != &nes_buffer)
			{
				emu->set_sample_rate(44100, &nes_buffer);
				current_buffer = &nes_buffer;
			}
		}
		else if (0 == strcmp(var.value, "stereo panning"))
		{
			if (current_buffer != &effects_buffer)
			{
				emu->set_sample_rate(44100, &effects_buffer);
				current_buffer = &effects_buffer;
			}

			Effects_Buffer::config_t c;
			c.pan_1             = -0.6f; // no full panning
			c.pan_2             =  0.6f;
			c.delay_variance    =  18.0f;
			c.reverb_delay      =  88.0f;
			c.echo_delay        =  61.0;
			c.reverb_level      =  0.2f; // adds a bit of "depth" instead of just being dry for each channel
			c.echo_level        =  0.2f;
			c.effects_enabled   =  1;
			effects_buffer.config( c );
		}
		else
		{
			if (current_buffer != &mono_buffer)
			{
				emu->set_sample_rate(44100, &mono_buffer);
				current_buffer = &mono_buffer;
			}
		}
	}
	else
	{
		//if the environment callback failed (won't happen), just set the nonlinear buffer
		if (current_buffer != &nes_buffer)
		{
			emu->set_sample_rate(44100, &nes_buffer);
			current_buffer = &nes_buffer;
		}
	}

	var.key   = "quicknes_audio_eq";
   var.value = NULL;

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (0 == strcmp(var.value, "default"))
			emu->set_equalizer(Nes_Emu::nes_eq);
		else if (0 == strcmp(var.value, "nes"))
			emu->set_equalizer(Nes_Emu::nes_eq);
		else if (0 == strcmp(var.value, "famicom"))
			emu->set_equalizer(Nes_Emu::famicom_eq);
		else if (0 == strcmp(var.value, "tv"))
			emu->set_equalizer(Nes_Emu::tv_eq);
		else if (0 == strcmp(var.value, "flat"))
			emu->set_equalizer(Nes_Emu::flat_eq);
		else if (0 == strcmp(var.value, "crisp"))
			emu->set_equalizer(Nes_Emu::crisp_eq);
		else if (0 == strcmp(var.value, "tinny"))
			emu->set_equalizer(Nes_Emu::tinny_eq);
		else
			emu->set_equalizer(Nes_Emu::nes_eq);
	}
	else
	{
		//if the environment callback failed (won't happen), just set the default NES equalizer
		emu->set_equalizer(Nes_Emu::nes_eq);
	}
}

static void check_variables(void)
{
   struct retro_variable var = {0};
   bool video_changed = false;

   var.key = "quicknes_no_sprite_limit";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         emu->set_sprite_mode( Nes_Emu::sprites_enhanced);
      else
         emu->set_sprite_mode( Nes_Emu::sprites_visible);
   }

   var.key = "quicknes_aspect_ratio_par";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      bool newval = (!strcmp(var.value, "PAR"));
      if (newval != aspect_ratio_par)
      {
         aspect_ratio_par = newval;
         video_changed = true;
      }
   }

   var.key = "quicknes_up_down_allowed";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      up_down_allowed = (!strcmp(var.value, "enabled")) ? true : false;
   }
   else
      up_down_allowed = false;


#ifndef PSP
   var.key = "quicknes_use_overscan_h";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      bool newval = (!strcmp(var.value, "enabled"));
      if (newval != use_overscan_h)
      {
         use_overscan_h = newval;
         video_changed = true;
      }
   }

   var.key = "quicknes_use_overscan_v";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      bool newval = (!strcmp(var.value, "enabled"));
      if (newval != use_overscan_v)
      {
         use_overscan_v = newval;
         video_changed = true;
      }
   }
#endif

   update_audio_mode();

   if (video_changed)
   {
      struct retro_system_av_info info;
      retro_get_system_av_info(&info);
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info);
   }
}

#define JOY_A           1
#define JOY_B           2
#define JOY_SELECT      4
#define JOY_START       8
#define JOY_UP       0x10
#define JOY_DOWN     0x20
#define JOY_LEFT     0x40
#define JOY_RIGHT    0x80

typedef struct
{
   unsigned retro;
   unsigned nes;
} keymap;

static const keymap bindmap[] = {
   { RETRO_DEVICE_ID_JOYPAD_A, JOY_A },
   { RETRO_DEVICE_ID_JOYPAD_B, JOY_B },
   { RETRO_DEVICE_ID_JOYPAD_SELECT, JOY_SELECT },
   { RETRO_DEVICE_ID_JOYPAD_START, JOY_START },
   { RETRO_DEVICE_ID_JOYPAD_UP, JOY_UP },
   { RETRO_DEVICE_ID_JOYPAD_DOWN, JOY_DOWN },
   { RETRO_DEVICE_ID_JOYPAD_LEFT, JOY_LEFT },
   { RETRO_DEVICE_ID_JOYPAD_RIGHT, JOY_RIGHT },
};

static void update_input(int pads[2])
{
   unsigned p;

   pads[0] = pads[1] = 0;
   input_poll_cb();

   for (p = 0; p < 2; p++)
   {
      unsigned bind;
      for (bind = 0; bind < sizeof(bindmap) / sizeof(bindmap[0]); bind++)
         pads[p] |= input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, bindmap[bind].retro) ? bindmap[bind].nes : 0;
   }

   if (!up_down_allowed)
   {
      for (p = 0; p < 2; p++)
      {
         if (pads[p] & JOY_UP)
            if (pads[p] & JOY_DOWN)
               pads[p] &= ~(JOY_UP | JOY_DOWN);

         if (pads[p] & JOY_LEFT)
            if (pads[p] & JOY_RIGHT)
               pads[p] &= ~(JOY_LEFT | JOY_RIGHT);
      }
   }
}

void retro_run(void)
{
   bool updated = false;
   int  pads[2] = {0};

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();
   
   bool audioDisabledForThisFrame = false;
   bool videoDisabledForThisFrame = false;
   bool hardDisableAudio = false;
   int flags;
   if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &flags))
   {
	   videoDisabledForThisFrame = !(flags & 1);
	   audioDisabledForThisFrame = !(flags & 2);
	   hardDisableAudio = !!(flags & 8);
   }

   if (hardDisableAudio != use_silent_buffer)
   {
      use_silent_buffer = hardDisableAudio;
      update_audio_mode();
   }

   update_input(pads);

   if (!videoDisabledForThisFrame)
   {
	   emu->emulate_frame(pads[0], pads[1]);
	   const Nes_Emu::frame_t &frame = emu->frame();
#ifdef PSP
	   static uint16_t     __attribute__((aligned(16))) retro_palette[256];
	   static unsigned int __attribute__((aligned(16))) d_list[128];
	   void* const texture_vram_p =
		   (void*)(0x44200000 - (Nes_Emu::image_width * Nes_Emu::image_height)); // max VRAM address - frame size


	   sceGuSync(0, 0);

	   for (unsigned i = 0; i < 256; i++)
	   {
		   const Nes_Emu::rgb_t& rgb = emu->nes_colors[frame.palette[i]];
		   retro_palette[i] = ((rgb.blue & 0xf8) << 8) | ((rgb.green & 0xfc) << 3) | ((rgb.red & 0xf8) >> 3);
	   }

	   sceKernelDcacheWritebackRange(retro_palette, sizeof(retro_palette));
	   sceKernelDcacheWritebackRange(frame.pixels, Nes_Emu::image_width * Nes_Emu::image_height);

	   sceGuStart(GU_DIRECT, d_list);

	   /* sceGuCopyImage doesnt seem to work correctly with GU_PSM_T8
		* so we use GU_PSM_4444 ( 2 Bytes per pixel ) instead
		* with half the values for pitch / width / x offset
		*/

	   sceGuCopyImage(GU_PSM_4444,
		   (use_overscan ? 0 : 4) + ((u32)frame.pixels & 0xF) / 2,
		   (use_overscan ? 0 : 4),
		   Nes_Emu::image_width / 2 - (use_overscan ? 0 : 8),
		   Nes_Emu::image_height - (use_overscan ? 0 : 16),
		   Nes_Emu::image_width / 2, (void*)((u32)frame.pixels & ~0xF), 0, 0,
		   Nes_Emu::image_width / 2, texture_vram_p);

	   sceGuTexSync();
	   sceGuTexImage(0, 256, 256, 256, texture_vram_p);
	   sceGuTexMode(GU_PSM_T8, 0, 0, GU_FALSE);
	   sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	   sceGuDisable(GU_BLEND);
	   sceGuClutMode(GU_PSM_5650, 0, 0xFF, 0);
	   sceGuClutLoad(32, retro_palette);

	   sceGuFinish();

	   video_cb(texture_vram_p,
		   Nes_Emu::image_width - (use_overscan ? 0 : 16),
		   Nes_Emu::image_height - (use_overscan ? 0 : 16),
		   256);
#else

	   static uint16_t video_buffer[Nes_Emu::image_width * Nes_Emu::image_height];
	   static uint16_t retro_palette[256];

	   for (unsigned i = 0; i < 256; i++)
	   {
		   const Nes_Emu::rgb_t& rgb = emu->nes_colors[frame.palette[i]];
		   retro_palette[i] = ((rgb.red & 0xf8) << 8) | ((rgb.green & 0xfc) << 3) | ((rgb.blue & 0xf8) >> 3);
	   }

	   for (int y = 0; y < Nes_Emu::image_height; y++)
	   {
		   uint16_t *out_scanline = video_buffer + Nes_Emu::image_width * y;
		   uint8_t *in_scanline = frame.pixels + videoBufferWidth * y;
		   for (int x = 0; x < Nes_Emu::image_width; x++)
			   out_scanline[x] = retro_palette[in_scanline[x]];
	   }

	   video_cb(video_buffer + (use_overscan_v ? (use_overscan_h ? 0 : 8) : ((use_overscan_h ? 0 : 8) + 256 * 8)),
		   Nes_Emu::image_width - (use_overscan_h ? 0 : 16),
		   Nes_Emu::image_height - (use_overscan_v ? 0 : 16),
		   Nes_Emu::image_width * sizeof(uint16_t));
#endif
   }
   else
   {
	   emu->emulate_skip_frame(pads[0], pads[1]);
   }

   if (!audioDisabledForThisFrame)
   {
	   // Mono -> Stereo.
	   int16_t samples[2048];
	   long read_samples = emu->read_samples(samples, 2048);
	   int16_t out_samples[4096];

	   if ( current_buffer != &effects_buffer)
	   {
			for (long i = 0; i < read_samples; i++)
				out_samples[(i << 1)] = out_samples[(i << 1) + 1] = samples[i];
			audio_batch_cb(out_samples, read_samples);
	   }
	   else
			audio_batch_cb(samples, read_samples >> 1);
   }
   else
   {
	   emu->read_samples(NULL, 2048);
   }
}

bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      fprintf(stderr, "RGB565 is not supported.\n");
      return false;
   }

   emu = new Nes_Emu;
   register_optional_mappers();
   register_extra_mappers();

   check_variables();

   update_audio_mode(); //calls set_sample_rate and set_equalizer
   emu->set_palette_range(0);

#ifdef PSP
   // TODO: fix overscan setting for PSP
   use_overscan = false;
   static uint8_t video_buffer[Nes_Emu::image_width * (Nes_Emu::image_height + 16)];
   emu->set_pixels(video_buffer + (8 * Nes_Emu::image_width), Nes_Emu::image_width);
#else
   static uint8_t video_buffer[videoBufferWidth * videoBufferHeight];
   emu->set_pixels(video_buffer, videoBufferWidth);
#endif

   struct retro_memory_descriptor descs[2];
   struct retro_memory_map retromap;

   memset(descs, 0, sizeof(descs));

   descs[0].ptr    = emu->low_mem();         // System RAM
   descs[0].start  = 0x00000000;
   descs[0].len    = Nes_Emu::low_mem_size;
   descs[0].select = 0;

   descs[1].ptr    = emu->high_mem();        // WRAM
   descs[1].start  = 0x00006000;
   descs[1].len    = Nes_Emu::high_mem_size;
   descs[1].select = 0;

   retromap.descriptors       = descs;
   retromap.num_descriptors   = sizeof(descs) / sizeof(*descs);

   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &retromap);

   Mem_File_Reader reader(info->data, info->size);
   return !emu->load_ines(reader);
}

void retro_unload_game(void)
{
   if (emu)
      emu->close();
   delete emu;
   emu = 0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

size_t retro_serialize_size(void)
{
   Mem_Writer writer;
   if (emu->save_state(writer))
      return 0;

   return writer.size();
}

bool is_fast_savestate()
{
	int value;
	bool okay = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &value);
	if (okay)
	{
		if (value & 4)
		{
			return true;
		}
	}
	return false;
}

bool retro_serialize(void *data, size_t size)
{
   bool isFastSavestate = is_fast_savestate();
   Mem_Writer writer(data, size);
   bool okay = !emu->save_state(writer);
   if (isFastSavestate)
   {
      emu->SaveAudioBufferState();
   }
   return okay;
}

bool retro_unserialize(const void *data, size_t size)
{
   bool isFastSavestate = is_fast_savestate();
   Mem_File_Reader reader(data, size);
   bool okay = !emu->load_state(reader);
   if (isFastSavestate)
   {
      emu->RestoreAudioBufferState();
   }
   return okay;
}

void *retro_get_memory_data(unsigned id)
{
   switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:
         if (emu->has_battery_ram())
             return emu->high_mem();
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         return emu->low_mem();
      default:
         break;
   }

   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:
         if (emu->has_battery_ram())
            return Nes_Emu::high_mem_size;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         return Nes_Emu::low_mem_size;
      default:
         break;
   }

   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned, bool, const char *)
{}
