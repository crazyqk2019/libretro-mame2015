#include <unistd.h>
#include <stdint.h>
#include <string.h>

#include "osdepend.h"

#include "emu.h"
#include "clifront.h"
#include "render.h"
#include "ui/ui.h"
#include "uiinput.h"
#include "drivenum.h"

#include "libretro.h"
#include "libretro_shared.h"

/* Empirically, i found that Mame savegames sizes are uniform for a given game,
that is, they're always the same size as the game progress. Tested on a dozen 
of different games both news and olds.
Problem is: looking at the code i can't exclude that at some point a game
can return a bigger state than the previous (i tried to follow the code but
it goes too deep into the innards of Mame).
To account for this, i report a bigger size than the one i got from mame,
this multiplier tells how much extra space to reserve (currently 
one and a half). */
#define SAVE_SIZE_MULTIPLIER 1.5

/* forward decls / externs / prototypes */
bool retro_load_ok    = false;
int retro_pause       = 0;

int fb_width          = 320;
int fb_height         = 240;
int fb_pitch          = 1600;
float retro_aspect    = 0;
float retro_fps       = 60.0;
int SHIFTON           = -1;
int NEWGAME_FROM_OSD  = 0;
char RPATH[512];

int serialize_size = 0; // memorize size of serialized savestate

static char option_mouse[50];
static char option_cheats[50];
static char option_nag[50];
static char option_info[50];
static char option_renderer[50];
static char option_warnings[50];
static char option_osd[50];
static char option_cli[50];
static char option_bios[50];
static char option_softlist[50];
static char option_softlist_media[50];
static char option_media[50];
static char option_read_config[50];
static char option_write_config[50];
static char option_auto_save[50];
static char option_throttle[50];
static char option_nobuffer[50];
static char option_saves[50];

const char *retro_save_directory;
const char *retro_system_directory;
const char *retro_content_directory;

retro_log_printf_t log_cb;

static bool draw_this_frame;

#ifdef M16B
uint16_t videoBuffer[1600*1200];
#define LOG_PIXEL_BYTES 1
#else
unsigned int videoBuffer[1600*1200];
#define LOG_PIXEL_BYTES 2*1
#endif

retro_video_refresh_t video_cb = NULL;
retro_environment_t environ_cb = NULL;

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include "retroogl.c"
#endif

static void extract_basename(char *buf, const char *path, size_t size)
{
   char *ext = NULL;
   const char *base = strrchr(path, '/');

   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base = NULL;

   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');

   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

retro_input_state_t input_state_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
static retro_input_poll_t input_poll_cb;

void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { }

void retro_set_environment(retro_environment_t cb)
{
   sprintf(option_mouse, "%s_%s", core, "mouse_mode");
   sprintf(option_cheats, "%s_%s", core, "cheats_enable");
   sprintf(option_nag, "%s_%s",core,"hide_nagscreen");
   sprintf(option_info, "%s_%s",core,"hide_infoscreen");
   sprintf(option_warnings,"%s_%s",core,"hide_warnings");
   sprintf(option_renderer,"%s_%s",core,"alternate_renderer");
   sprintf(option_osd,"%s_%s",core,"boot_to_osd");
   sprintf(option_bios,"%s_%s",core,"boot_to_bios");
   sprintf(option_cli,"%s_%s",core,"boot_from_cli");
   sprintf(option_softlist,"%s_%s",core,"softlists_enable");
   sprintf(option_softlist_media,"%s_%s",core,"softlists_auto_media");
   sprintf(option_media,"%s_%s",core,"media_type");
   sprintf(option_read_config,"%s_%s",core,"read_config");
   sprintf(option_write_config,"%s_%s",core,"write_config");
   sprintf(option_auto_save,"%s_%s",core,"auto_save");
   sprintf(option_saves,"%s_%s",core,"saves");
   sprintf(option_throttle,"%s_%s",core,"throttle");
  sprintf(option_nobuffer,"%s_%s",core,"nobuffer");

   static const struct retro_variable vars[] = {
    /* some ifdefs are redundant but I wanted 
     * to have these options in a logical order
     * common for MAME/MESS/UME. */

    { option_read_config, "Read configuration; disabled|enabled" },

#if !defined(WANT_PHILIPS_CDI)
    /* ONLY FOR MESS/UME */
#if !defined(WANT_MAME)
    { option_write_config, "Write configuration; disabled|enabled" },
    { option_saves, "Save state naming; game|system" },
#endif
#endif
    /* common for MAME/MESS/UME */
    { option_auto_save, "Auto save/load states; disabled|enabled" },
    { option_mouse, "XY device (Restart); none|lightgun|mouse" },
    { option_throttle, "Enable throttle; disabled|enabled" },
    { option_cheats, "Enable cheats; disabled|enabled" },
//  { option_nobuffer, "Nobuffer patch; disabled|enabled" },
    { option_nag, "Hide nag screen; disabled|enabled" },
    { option_info, "Hide gameinfo screen; disabled|enabled" },
    { option_warnings, "Hide warnings screen; disabled|enabled" },
    { option_renderer, "Alternate render method; disabled|enabled" },

#if !defined(WANT_PHILIPS_CDI)
    /* ONLY FOR MESS/UME */
#if !defined(WANT_MAME)
    { option_softlist, "Enable softlists; enabled|disabled" },
    { option_softlist_media, "Softlist automatic media type; enabled|disabled" },
#if defined(WANT_MESS)
    { option_media, "Media type; cart|flop|cdrm|cass|hard|serl|prin" },
#elif defined(WANT_UME)
    { option_media, "Media type; rom|cart|flop|cdrm|cass|hard|serl|prin" },
#endif
    { option_bios, "Boot to BIOS; disabled|enabled" },
#endif

    /* common for MAME/MESS/UME */
    { option_osd, "Boot to OSD; disabled|enabled" },
    { option_cli, "Boot from CLI; disabled|enabled" },
#endif
    { NULL, NULL },

   };

   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

static void check_variables(void)
{
   struct retro_variable var = {0};

   var.key   = option_cli;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         experimental_cmdline = true;
      if (!strcmp(var.value, "disabled"))
         experimental_cmdline = false;
   }

   var.key   = option_mouse;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "none"))
         mouse_mode = 0;
      if (!strcmp(var.value, "mouse"))
         mouse_mode = 1;
      if (!strcmp(var.value, "lightgun"))
         mouse_mode = 2;
   }

   var.key   = option_throttle;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         throttle_enable = false;
      if (!strcmp(var.value, "enabled"))
         throttle_enable = true;
   }

   var.key   = option_nobuffer;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         nobuffer_enable = false;
      if (!strcmp(var.value, "enabled"))
         nobuffer_enable = true;
   }

   var.key   = option_cheats;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         cheats_enable = false;
      if (!strcmp(var.value, "enabled"))
         cheats_enable = true;
   }

   var.key   = option_nag;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         hide_nagscreen = false;
      if (!strcmp(var.value, "enabled"))
         hide_nagscreen = true;
   }

   var.key   = option_info;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         hide_gameinfo = false;
      if (!strcmp(var.value, "enabled"))
         hide_gameinfo = true;
   }

   var.key   = option_warnings;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         hide_warnings = false;
      if (!strcmp(var.value, "enabled"))
         hide_warnings = true;
   }

   var.key   = option_renderer;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         alternate_renderer = false;
      if (!strcmp(var.value, "enabled"))
         alternate_renderer = true;
   }

   var.key   = option_osd;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         boot_to_osd_enable = true;
      if (!strcmp(var.value, "disabled"))
         boot_to_osd_enable = false;
   }

   var.key = option_read_config;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         read_config_enable = false;
      if (!strcmp(var.value, "enabled"))
         read_config_enable = true;
   }

   var.key   = option_auto_save;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         auto_save_enable = false;
      if (!strcmp(var.value, "enabled"))
         auto_save_enable = true;
   }

   var.key   = option_saves;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "game"))
         game_specific_saves_enable = true;
      if (!strcmp(var.value, "system"))
         game_specific_saves_enable = false;
   }

#if !defined(WANT_MAME)

   var.key   = option_media;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      sprintf(mediaType,"-%s",var.value);
   }

   var.key   = option_softlist;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         softlist_enable = true;
      if (!strcmp(var.value, "disabled"))
         softlist_enable = false;
   }

   var.key   = option_softlist_media;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         softlist_auto = true;
      if (!strcmp(var.value, "disabled"))
         softlist_auto = false;
   }

   var.key = option_bios;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         boot_to_bios_enable = true;
      if (!strcmp(var.value, "disabled"))
         boot_to_bios_enable = false;
   }

   var.key = option_write_config;
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         write_config_enable = false;
      if (!strcmp(var.value, "enabled"))
         write_config_enable = true;
   }

#endif
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));

#if defined(WANT_MAME)
   info->library_name     = "MAME 2015";
#elif defined(WANT_MESS)
   info->library_name     = "MESS 2015";
#elif defined(WANT_UME)
   info->library_name     = "UME 2015";
#elif defined(WANT_PHILIPS_CDI)
   info->library_name     = "Philips CD-i 2015";
#else
   info->library_name     = "MAME 2015";
#endif

#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = "0.160" GIT_VERSION;
#if !defined(WANT_PHILIPS_CDI)
   info->valid_extensions = "chd|cmd|zip|7z";
#else
   info->valid_extensions = "chd";
#endif
   info->need_fullpath    = true;
   info->block_extract    = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   check_variables();

   info->geometry.base_width  = fb_width;
   info->geometry.base_height = fb_height;

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "AV_INFO: width=%d height=%d\n",info->geometry.base_width,info->geometry.base_height);

   info->geometry.max_width   = 1600;
   info->geometry.max_height  = 1200;

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "AV_INFO: max_width=%d max_height=%d\n",info->geometry.max_width,info->geometry.max_height);

   info->geometry.aspect_ratio = retro_aspect;

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "AV_INFO: aspect_ratio = %f\n",info->geometry.aspect_ratio);

   info->timing.fps            = retro_fps;
   info->timing.sample_rate    = 48000.0;

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "AV_INFO: fps = %f sample_rate = %f\n",info->timing.fps,info->timing.sample_rate);

}


void retro_init (void)
{
   struct retro_log_callback log;
   const char *system_dir  = NULL;
   const char *content_dir = NULL;
   const char *save_dir    = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
   {
      /* if defined, use the system directory */
      retro_system_directory = system_dir;
   }

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "SYSTEM_DIRECTORY: %s", retro_system_directory);

   if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir)
   {
      // if defined, use the system directory
      retro_content_directory=content_dir;
   }

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "CONTENT_DIRECTORY: %s", retro_content_directory);


   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
   {
      /* If save directory is defined use it, 
       * otherwise use system directory. */
      retro_save_directory = *save_dir ? save_dir : retro_system_directory;

   }
   else
   {
      /* make retro_save_directory the same,
       * in case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 
       * is not implemented by the frontend. */
      retro_save_directory=retro_system_directory;
   }
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "SAVE_DIRECTORY: %s", retro_save_directory);

}

extern void retro_finish();

void retro_deinit(void)
{
   printf("RETRO DEINIT\n");
   if(retro_load_ok)retro_finish();
}

void retro_reset (void)
{
   mame_reset = 1;
}

int RLOOP=1;
extern void retro_main_loop();

// save state functions defined in mame.c
extern void retro_save_state(retro_buffer_writer &buf);
extern bool retro_load_state(retro_buffer_reader &buf);

void retro_run (void)
{
   static int mfirst=1;
   bool updated = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

   if(mfirst==1)
   {
      mfirst++;
      int res=mmain(1,RPATH);
      if(res!=1)exit(0);
      if (log_cb)log_cb(RETRO_LOG_INFO,"MAIN FIRST\n");
      retro_load_ok=true;
      return;
   }

   if (NEWGAME_FROM_OSD == 1)
   {
      serialize_size = 0; // reset stored serial size
      struct retro_system_av_info ninfo;

      retro_get_system_av_info(&ninfo);

      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &ninfo);

      if (log_cb)
         log_cb(RETRO_LOG_INFO, "ChangeAV: w:%d h:%d ra:%f.\n",
               ninfo.geometry.base_width, ninfo.geometry.base_height, ninfo.geometry.aspect_ratio);

      NEWGAME_FROM_OSD=0;
   }

   input_poll_cb();

   process_mouse_state();
   process_lightgun_state();
   process_keyboard_state();
   process_joypad_state();

   if(retro_pause==0)retro_main_loop();

   RLOOP=1;

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   do_glflush();
#else
   if (draw_this_frame)
      video_cb(videoBuffer, fb_width, fb_height, fb_pitch << LOG_PIXEL_BYTES);
   else
      video_cb(NULL, fb_width, fb_height, fb_pitch << LOG_PIXEL_BYTES);
#endif
}

bool retro_load_game(const struct retro_game_info *info)
{
   char basename[256];
#ifdef M16B
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
#endif

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "pixel format not supported");
      return false;
   }

   check_variables();

#ifdef M16B
   memset(videoBuffer, 0, 1600*1200*2);
#else
   memset(videoBuffer, 0, 1600*1200*2*2);
#endif

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#if defined(HAVE_OPENGLES)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   /*
      hw_render.depth = true;
      hw_render.stencil = true;
      hw_render.bottom_left_origin = true;
      */
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;
#endif

   extract_basename(basename, info->path, sizeof(basename));
   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));
   strcpy(RPATH,info->path);

   return true;
}

void retro_unload_game(void)
{
   serialize_size = 0; // reset stored serialized savestate size
   if (retro_pause == 0)
      retro_pause = -1;
}

size_t retro_serialize_size(void) 
{ 
	log_cb(RETRO_LOG_INFO, "RETRO_SERIALIZE_SIZE CALLED\n");
	// serialize_size is memorized per-game, if we already have it
	// we send the old value
	if(serialize_size == 0)
	{
		// to calculate the size we must perform an actual savestate
		// and measure it
		retro_buffer_writer saveBuffer;
		retro_save_state(saveBuffer);
		// reserve some extra space (see comment on define)
		serialize_size  = saveBuffer.size() * SAVE_SIZE_MULTIPLIER;
		log_cb(RETRO_LOG_INFO, "RETRO_SERIALIZE_SIZE IS: %d, ALLOCATED: %d\n", saveBuffer.size(), serialize_size);
	}

	return serialize_size; 
}
bool retro_serialize(void *data, size_t size) 
{
	retro_buffer_writer saveBuffer;
	retro_save_state(saveBuffer);
	log_cb(RETRO_LOG_INFO, "RETRO_SERIALIZE ACTUAL SIZE IS: %d\n",saveBuffer.size());
	// check if the save size is within both the buffer size and the precalculated serialize_size.
	if( (saveBuffer.size() > size) || (size>serialize_size) ) 
	{
		log_cb(RETRO_LOG_ERROR, "RETRO_SERIALIZE too big. Got %d buffer size: %d stored size: %d\n",saveBuffer.size(), size, serialize_size);
		return false;
	}
	memcpy(data, saveBuffer.data(), saveBuffer.size()); 
	return true;
}
bool retro_unserialize(const void * data, size_t size) 
{ 
	log_cb(RETRO_LOG_INFO, "RETRO_UNSERIALIZE. SIZE: %d\n",size);
	retro_buffer_reader readBuffer(data, size);
	bool ret = retro_load_state(readBuffer);
	if(!ret) {
		log_cb(RETRO_LOG_ERROR, "RETRO_UNSERIALIZE. ERROR!\n");
	}
	return ret;

}

unsigned retro_get_region (void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned type) { return 0; }
size_t retro_get_memory_size(unsigned type) { return 0; }
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) {return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2) {}
void retro_set_controller_port_device(unsigned in_port, unsigned device) {}

void *retro_get_fb_ptr(void)
{
   return videoBuffer;
}

void retro_frame_draw_enable(bool enable)
{
   draw_this_frame = enable;
}
