/*
 *  Copyright (C) 2002-2004  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* $Id: sdlmain.cpp,v 1.66 2004-06-10 07:18:19 harekiet Exp $ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>

#include "SDL.h"

#include "dosbox.h"
#include "video.h"
#include "mouse.h"
#include "pic.h"
#include "timer.h"
#include "setup.h"
#include "support.h"
#include "debug.h"
#include "mapper.h"

//#define DISABLE_JOYSTICK

#if C_OPENGL
#include "SDL_opengl.h"

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#ifdef __WIN32__
#define NVIDIA_PixelDataRange 1

#ifndef WGL_NV_allocate_memory
#define WGL_NV_allocate_memory 1
typedef void * (APIENTRY * PFNWGLALLOCATEMEMORYNVPROC) (int size, float readfreq, float writefreq, float priority);
typedef void (APIENTRY * PFNWGLFREEMEMORYNVPROC) (void *pointer);
#endif

PFNWGLALLOCATEMEMORYNVPROC db_glAllocateMemoryNV = NULL;
PFNWGLFREEMEMORYNVPROC db_glFreeMemoryNV = NULL;

#else 

#endif

#if defined(NVIDIA_PixelDataRange)

#ifndef GL_NV_pixel_data_range
#define GL_NV_pixel_data_range 1
#define GL_WRITE_PIXEL_DATA_RANGE_NV      0x8878
typedef void (APIENTRYP PFNGLPIXELDATARANGENVPROC) (GLenum target, GLsizei length, GLvoid *pointer);
typedef void (APIENTRYP PFNGLFLUSHPIXELDATARANGENVPROC) (GLenum target);
#endif

PFNGLPIXELDATARANGENVPROC glPixelDataRangeNV = NULL;

#endif

#endif //C_OPENGL


#if !(ENVIRON_INCLUDED)
extern char** environ;
#endif


#ifdef WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define STDOUT_FILE	TEXT("stdout.txt")
#define STDERR_FILE	TEXT("stderr.txt")
#define DEFAULT_CONFIG_FILE "/dosbox.conf"
#elif defined(MACOSX)
#define DEFAULT_CONFIG_FILE "/Library/Preferences/DOSBox Preferences"
#else /*linux freebsd*/
#define DEFAULT_CONFIG_FILE "/.dosboxrc"
#endif

void MAPPER_Init(void);
void MAPPER_StartUp(Section * sec);

enum SCREEN_TYPES	{ 
	SCREEN_SURFACE,
	SCREEN_OVERLAY,
	SCREEN_OPENGL
};


struct SDL_Block {
	bool active;							//If this isn't set don't draw
	bool updating;
	struct {
		Bit32u width;
		Bit32u height;
		Bitu flags;
		GFX_Modes mode;
		double scalex,scaley;
		GFX_ResetCallBack reset;
	} draw;
	bool wait_on_error;
	struct {
		Bit32u width,height,bpp;
		bool fixed;
		bool fullscreen;
		bool doublebuf;
		SCREEN_TYPES type;
		SCREEN_TYPES want_type;
		double hwscale;
	} desktop;
#if C_OPENGL
	struct {
		Bitu pitch;
		void * framebuf;
		GLuint texture;
		GLuint displaylist;
		GLint max_texsize;
		bool bilinear;
		bool packed_pixel;
		bool paletted_texture;
#if defined(NVIDIA_PixelDataRange)
		bool pixel_data_range;
#endif
	} opengl;
#endif
	SDL_Rect clip;
	SDL_Surface * surface;
	SDL_Overlay * overlay;
	SDL_cond *cond;
	struct {
		bool autolock;
		bool autoenable;
		bool requestlock;
		bool locked;
		Bitu sensitivity;
	} mouse;
};

static SDL_Block sdl;
static void CaptureMouse(void);

extern const char * RunningProgram;
void GFX_SetTitle(Bits cycles,Bits frameskip){
	char title[200]={0};
	static Bits internal_cycles=0;
	static Bits internal_frameskip=0;
	if(cycles != -1) internal_cycles = cycles;
	if(frameskip != -1) internal_frameskip = frameskip;
	sprintf(title,"DOSBox %s,Cpu Cycles: %8d, Frameskip %2d, Program: %s",VERSION,internal_cycles,internal_frameskip,RunningProgram);
	SDL_WM_SetCaption(title,VERSION);
}

/* Reset the screen with current values in the sdl structure */
Bitu GFX_GetBestMode(Bitu flags) {
	Bitu testbpp,gotbpp,setflags;
	switch (sdl.desktop.want_type) {
	case SCREEN_SURFACE:
check_surface:
		/* Check if we can satisfy the depth it loves */
		if (flags & LOVE_8) testbpp=8;
		else if (flags & LOVE_16) testbpp=16;
		else if (flags & LOVE_32) testbpp=32;
		if (sdl.desktop.fullscreen) gotbpp=SDL_VideoModeOK(640,480,testbpp,SDL_FULLSCREEN|SDL_HWSURFACE|SDL_HWPALETTE);
		else gotbpp=sdl.desktop.bpp;
		/* If we can't get our favorite mode check for another working one */
		switch (gotbpp) {
		case 8:
			if (flags & CAN_8) flags&=~(CAN_16|CAN_32);
			break;
		case 15:
		case 16:
			if (flags & CAN_16) flags&=~(CAN_8|CAN_32);
			break;
		case 24:
		case 32:
			if (flags & CAN_32) flags&=~(CAN_8|CAN_16);
			break;
		}
		/* Not a valid display depth found? Let's just hope sdl provides conversions */
		break;
	case SCREEN_OVERLAY:
		if (flags & NEED_RGB || !(flags&CAN_32)) goto check_surface;
		flags|=HAVE_SCALING;
		flags&=~(CAN_8,CAN_16);
		break;
#if C_OPENGL
	case SCREEN_OPENGL:
		if (flags & NEED_RGB || !(flags&CAN_32)) goto check_surface;
		flags|=HAVE_SCALING;
		flags&=~(CAN_8,CAN_16);
		break;
#endif
	}
	return flags;
}


void GFX_ResetScreen(void) {
	GFX_Stop();
	if (sdl.draw.reset) (sdl.draw.reset)();
	GFX_Start();
}

static int int_log2 (int val) {
    int log = 0;
    while ((val >>= 1) != 0)
	log++;
    return log;
}

GFX_Modes GFX_SetSize(Bitu width,Bitu height,Bitu flags,double scalex,double scaley,GFX_ResetCallBack reset) {
	if (sdl.updating) GFX_EndUpdate();
	sdl.draw.width=width;
	sdl.draw.height=height;
	sdl.draw.flags=flags;
	sdl.draw.mode=GFX_NONE;
	sdl.draw.reset=reset;
	sdl.draw.scalex=scalex;
	sdl.draw.scaley=scaley;

	Bitu bpp;
	switch (sdl.desktop.want_type) {
	case SCREEN_SURFACE:
dosurface:
		if (flags & CAN_8) bpp=8;
		if (flags & CAN_16) bpp=16;
		if (flags & CAN_32) bpp=32;
		sdl.desktop.type=SCREEN_SURFACE;
		sdl.clip.w=width;
		sdl.clip.h=height;
		if (sdl.desktop.fullscreen) {
			if (sdl.desktop.fixed) {
				sdl.clip.x=(Sint16)((sdl.desktop.width-width)/2);
				sdl.clip.y=(Sint16)((sdl.desktop.height-height)/2);
				sdl.surface=SDL_SetVideoMode(sdl.desktop.width,sdl.desktop.height,bpp,
					SDL_FULLSCREEN|SDL_HWSURFACE|(sdl.desktop.doublebuf ? SDL_DOUBLEBUF|SDL_ASYNCBLIT  : 0)|SDL_HWPALETTE);
			} else {
				sdl.clip.x=0;sdl.clip.y=0;
				sdl.surface=SDL_SetVideoMode(width,height,bpp,
					SDL_FULLSCREEN|SDL_HWSURFACE|(sdl.desktop.doublebuf ? SDL_DOUBLEBUF|SDL_ASYNCBLIT  : 0)|SDL_HWPALETTE);
			}
		} else {
			sdl.clip.x=0;sdl.clip.y=0;
			sdl.surface=SDL_SetVideoMode(width,height,bpp,SDL_HWSURFACE);
		}
		if (sdl.surface) switch (sdl.surface->format->BitsPerPixel) {
			case 8:sdl.draw.mode=GFX_8;break;
			case 15:sdl.draw.mode=GFX_15;break;
			case 16:sdl.draw.mode=GFX_16;break;
			case 32:sdl.draw.mode=GFX_32;break;
			default:
				break;
		}
		break;
	case SCREEN_OVERLAY:
		if (sdl.overlay) SDL_FreeYUVOverlay(sdl.overlay);
		sdl.overlay=0;
		if (!(flags&CAN_32) || (flags & NEED_RGB)) goto dosurface;
		if (sdl.desktop.fullscreen) {
			if (sdl.desktop.fixed) {
				double ratio_w=(double)sdl.desktop.width/(width*scalex);
				double ratio_h=(double)sdl.desktop.height/(height*scaley);
				if ( ratio_w < ratio_h) {
					sdl.clip.w=(Bit16u)sdl.desktop.width;
					sdl.clip.h=(Bit16u)(height*scaley*ratio_w);
				} else {
					sdl.clip.w=(Bit16u)(width*scalex*ratio_h);
					sdl.clip.h=(Bit16u)sdl.desktop.height;
				}
				sdl.clip.x=(Sint16)((sdl.desktop.width-sdl.clip.w)/2);
				sdl.clip.y=(Sint16)((sdl.desktop.height-sdl.clip.h)/2);
				sdl.surface=SDL_SetVideoMode(sdl.desktop.width,sdl.desktop.height,0,
					SDL_FULLSCREEN|SDL_HWSURFACE);
			} else {
				sdl.clip.x=0;sdl.clip.y=0;
				sdl.clip.w=(Bit16u)(width*scalex);
				sdl.clip.h=(Bit16u)(height*scaley);
				sdl.surface=SDL_SetVideoMode(sdl.clip.w,sdl.clip.h,0,
					SDL_FULLSCREEN|SDL_HWSURFACE);
			}
		} else {
			sdl.clip.x=0;sdl.clip.y=0;
			sdl.clip.w=(Bit16u)(width*scalex*sdl.desktop.hwscale);
			sdl.clip.h=(Bit16u)(height*scaley*sdl.desktop.hwscale);
			sdl.surface=SDL_SetVideoMode(sdl.clip.w,sdl.clip.h,0,SDL_HWSURFACE);
		}
		sdl.overlay=SDL_CreateYUVOverlay(width*2,height,SDL_UYVY_OVERLAY,sdl.surface);
		if (!sdl.overlay) {
			LOG_MSG("SDL:Failed to create overlay, switching back to surface");
			goto dosurface;
		}
		sdl.desktop.type=SCREEN_OVERLAY;
		sdl.draw.mode=GFX_32;
		break;
#if C_OPENGL
	case SCREEN_OPENGL:
	{
		if (sdl.opengl.framebuf) {
#if defined(NVIDIA_PixelDataRange)
			if (sdl.opengl.pixel_data_range) db_glFreeMemoryNV(sdl.opengl.framebuf);
			else
#endif
			free(sdl.opengl.framebuf);
		}
		sdl.opengl.framebuf=0;
		if (!(flags&CAN_32) || (flags & NEED_RGB)) goto dosurface;
		int texsize=2 << int_log2(width > height ? width : height);
		if (texsize>sdl.opengl.max_texsize) {
			LOG_MSG("SDL:OPENGL:No support for texturesize of %d, falling back to surface",texsize);
			goto dosurface;
		}
		SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
		if (sdl.desktop.fullscreen) {
			if (sdl.desktop.fixed) {
				double ratio_w=(double)sdl.desktop.width/(width*scalex);
				double ratio_h=(double)sdl.desktop.height/(height*scaley);
				if ( ratio_w < ratio_h) {
					sdl.clip.w=(Bit16u)sdl.desktop.width;
					sdl.clip.h=(Bit16u)(height*scaley*ratio_w);
				} else {
					sdl.clip.w=(Bit16u)(width*scalex*ratio_h);
					sdl.clip.h=(Bit16u)sdl.desktop.height;
				}
				sdl.clip.x=(Sint16)((sdl.desktop.width-sdl.clip.w)/2);
				sdl.clip.y=(Sint16)((sdl.desktop.height-sdl.clip.h)/2);
				sdl.surface=SDL_SetVideoMode(sdl.desktop.width,sdl.desktop.height,0,
					SDL_OPENGL|SDL_FULLSCREEN|SDL_HWSURFACE);
			} else {
				sdl.clip.x=0;sdl.clip.y=0;
				sdl.clip.w=(Bit16u)(width*scalex);
				sdl.clip.h=(Bit16u)(height*scaley);
				sdl.surface=SDL_SetVideoMode(sdl.clip.w,sdl.clip.h,0,
					SDL_OPENGL|SDL_FULLSCREEN|SDL_HWSURFACE);
			}
		} else {
			sdl.clip.x=0;sdl.clip.y=0;
			sdl.clip.w=(Bit16u)(width*scalex*sdl.desktop.hwscale);
			sdl.clip.h=(Bit16u)(height*scaley*sdl.desktop.hwscale);
			sdl.surface=SDL_SetVideoMode(sdl.clip.w,sdl.clip.h,0,
				SDL_OPENGL|SDL_HWSURFACE);
		}
		if (!sdl.surface || sdl.surface->format->BitsPerPixel<15) {
			LOG_MSG("SDL:OPENGL:Can't open drawing surface, are you running in 16bpp(or higher) mode?");
			goto dosurface;
		}
		/* Create the texture and display list */
#if defined(NVIDIA_PixelDataRange)
		if (sdl.opengl.pixel_data_range) {
			sdl.opengl.framebuf=db_glAllocateMemoryNV(width*height*4,0.0,1.0,1.0);
			glPixelDataRangeNV(GL_WRITE_PIXEL_DATA_RANGE_NV,width*height*4,sdl.opengl.framebuf);
			glEnableClientState(GL_WRITE_PIXEL_DATA_RANGE_NV);
		} else {
#else 
		{
#endif
			sdl.opengl.framebuf=malloc(width*height*4);		//32 bit color
		}
		sdl.opengl.pitch=width*4;
		glViewport(sdl.clip.x,sdl.clip.y,sdl.clip.w,sdl.clip.h);
		glMatrixMode (GL_PROJECTION);
		glDeleteTextures(1,&sdl.opengl.texture);
 		glGenTextures(1,&sdl.opengl.texture);
		glBindTexture(GL_TEXTURE_2D,sdl.opengl.texture);
		// No borders
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
		if (sdl.opengl.bilinear) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texsize, texsize, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, 0);

		glClearColor (0.0, 0.0, 0.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);
		SDL_GL_SwapBuffers();
		glClear(GL_COLOR_BUFFER_BIT);
		glShadeModel (GL_FLAT); 
		glDisable (GL_DEPTH_TEST);
		glDisable (GL_LIGHTING);
		glDisable(GL_CULL_FACE);
		glEnable(GL_TEXTURE_2D);
		glMatrixMode (GL_MODELVIEW);
		glLoadIdentity ();

		GLfloat tex_width=((GLfloat)(width)/(GLfloat)texsize);
		GLfloat tex_height=((GLfloat)(height)/(GLfloat)texsize);

		if (glIsList(sdl.opengl.displaylist)) glDeleteLists(sdl.opengl.displaylist, 1);
		sdl.opengl.displaylist = glGenLists(1);
		glNewList(sdl.opengl.displaylist, GL_COMPILE);
		glBindTexture(GL_TEXTURE_2D, sdl.opengl.texture);
		glBegin(GL_QUADS);
		// lower left
		glTexCoord2f(0,tex_height); glVertex2f(-1.0f,-1.0f);
		// lower right
		glTexCoord2f(tex_width,tex_height); glVertex2f(1.0f, -1.0f);
		// upper right
		glTexCoord2f(tex_width,0); glVertex2f(1.0f, 1.0f);
		// upper left
		glTexCoord2f(0,0); glVertex2f(-1.0f, 1.0f);
		glEnd();
		glEndList();
		sdl.desktop.type=SCREEN_OPENGL;
		sdl.draw.mode=GFX_32;
		break;
		}//OPENGL
#endif	//C_OPENGL
	}//CASE
	if (sdl.draw.mode!=GFX_NONE) GFX_Start();
	return sdl.draw.mode;
}


static void CaptureMouse(void) {
	sdl.mouse.locked=!sdl.mouse.locked;
	if (sdl.mouse.locked) {
		SDL_WM_GrabInput(SDL_GRAB_ON);
		SDL_ShowCursor(SDL_DISABLE);
	} else {
		SDL_WM_GrabInput(SDL_GRAB_OFF);
		SDL_ShowCursor(SDL_ENABLE);
	}
}

static void SwitchFullScreen(void) {
	sdl.desktop.fullscreen=!sdl.desktop.fullscreen;
	if (sdl.desktop.fullscreen) {
		if (!sdl.mouse.locked) CaptureMouse();
	} else {
		if (sdl.mouse.locked) CaptureMouse();
	}
	GFX_ResetScreen();
}

void GFX_SwitchFullScreen(void) {
    SwitchFullScreen();
}

bool GFX_StartUpdate(Bit8u * & pixels,Bitu & pitch) {
	if (!sdl.active || sdl.updating) return false;
	sdl.updating=true;
	switch (sdl.desktop.type) {
	case SCREEN_SURFACE:
		if (SDL_MUSTLOCK(sdl.surface)) {
			if (SDL_LockSurface(sdl.surface)) {
				LOG_MSG("SDL Lock failed");
				sdl.updating=false;
				return false;
			}
		}
		pixels=(Bit8u *)sdl.surface->pixels;
		pixels+=sdl.clip.y*sdl.surface->pitch;
		pixels+=sdl.clip.x*sdl.surface->format->BytesPerPixel;
		pitch=sdl.surface->pitch;
		return true;
	case SCREEN_OVERLAY:
		SDL_LockYUVOverlay(sdl.overlay);
		pixels=(Bit8u *)*(sdl.overlay->pixels);
		pitch=*(sdl.overlay->pitches);
		return true;
#if C_OPENGL
	case SCREEN_OPENGL:
		pixels=(Bit8u *)sdl.opengl.framebuf;
		pitch=sdl.opengl.pitch;
		return true;
#endif
	}
	return false;
}

void GFX_EndUpdate(void) {
	if (!sdl.updating) return;
	sdl.updating=false;
	switch (sdl.desktop.type) {
	case SCREEN_SURFACE:
		if (SDL_MUSTLOCK(sdl.surface)) {
			SDL_UnlockSurface(sdl.surface);
		}
		SDL_Flip(sdl.surface);
		break;
	case SCREEN_OVERLAY:
		SDL_UnlockYUVOverlay(sdl.overlay);
		SDL_DisplayYUVOverlay(sdl.overlay,&sdl.clip);
		break;
#if C_OPENGL
	case SCREEN_OPENGL:
		glBindTexture(GL_TEXTURE_2D, sdl.opengl.texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 
				sdl.draw.width, sdl.draw.height, GL_BGRA_EXT,
				GL_UNSIGNED_INT_8_8_8_8_REV, sdl.opengl.framebuf);
		glCallList(sdl.opengl.displaylist);
		SDL_GL_SwapBuffers();
		break;
#endif

	}
}


void GFX_SetPalette(Bitu start,Bitu count,GFX_PalEntry * entries) {
	/* I should probably not change the GFX_PalEntry :) */
	if (sdl.surface->flags & SDL_HWPALETTE) {
		if (!SDL_SetPalette(sdl.surface,SDL_PHYSPAL,(SDL_Color *)entries,start,count)) {
			E_Exit("SDL:Can't set palette");
		}
	} else {
		if (!SDL_SetPalette(sdl.surface,SDL_LOGPAL,(SDL_Color *)entries,start,count)) {
			E_Exit("SDL:Can't set palette");
		}
	}
}

Bitu GFX_GetRGB(Bit8u red,Bit8u green,Bit8u blue) {
	switch (sdl.desktop.type) {
	case SCREEN_SURFACE:
		return SDL_MapRGB(sdl.surface->format,red,green,blue);
	case SCREEN_OVERLAY:
		{
			Bit8u y =  ( 9797*(red) + 19237*(green) +  3734*(blue) ) >> 15;
			Bit8u u =  (18492*((blue)-(y)) >> 15) + 128;
			Bit8u v =  (23372*((red)-(y)) >> 15) + 128;
#ifdef WORDS_BIGENDIAN
			return (y << 0) | (v << 8) | (y << 16) | (u << 24);
#else
			return (u << 0) | (y << 8) | (v << 16) | (y << 24);
#endif
		}
	case SCREEN_OPENGL:
//		return ((red << 0) | (green << 8) | (blue << 16)) | (255 << 24);
		//USE BGRA
		return ((blue << 0) | (green << 8) | (red << 16)) | (255 << 24);
	}
	return 0;
}

void GFX_Stop() {
	sdl.active=false;
}

void GFX_Start() {
	sdl.active=true;
}

static void GUI_ShutDown(Section * sec) {
	GFX_Stop();
	if (sdl.mouse.locked) CaptureMouse();
	if (sdl.desktop.fullscreen) SwitchFullScreen();
	SDL_Quit();   //Becareful this should be removed if on the fly renderchanges are allowed
}

static void KillSwitch(void){
	throw 1;
}

static void GUI_StartUp(Section * sec) {
	sec->AddDestroyFunction(&GUI_ShutDown);
	Section_prop * section=static_cast<Section_prop *>(sec);
	sdl.active=false;
	sdl.updating=false;
	sdl.desktop.fullscreen=section->Get_bool("fullscreen");
	sdl.wait_on_error=section->Get_bool("waitonerror");
	sdl.mouse.locked=false;
	sdl.mouse.requestlock=false;
	sdl.desktop.fixed=section->Get_bool("fullfixed");
	sdl.desktop.width=section->Get_int("fullwidth");
	sdl.desktop.height=section->Get_int("fullheight");
	sdl.desktop.doublebuf=section->Get_bool("fulldouble");
	sdl.desktop.hwscale=section->Get_float("hwscale");
	if (sdl.desktop.hwscale<0.1f) {
		LOG_MSG("SDL:Can't hwscale lower than 0.1");
		sdl.desktop.hwscale=0.1f;
	}
	if (!sdl.desktop.width) {
#ifdef WIN32
		sdl.desktop.width=GetSystemMetrics(SM_CXSCREEN);
#else	
		sdl.desktop.width=1024;
#endif
	}
	if (!sdl.desktop.height) {
#ifdef WIN32
		sdl.desktop.height=GetSystemMetrics(SM_CYSCREEN);
#else	
		sdl.desktop.height=768;
#endif
	}
    sdl.mouse.autoenable=section->Get_bool("autolock");
	sdl.mouse.autolock=false;
	sdl.mouse.sensitivity=section->Get_int("sensitivity");
	const char * output=section->Get_string("output");
	if (!strcasecmp(output,"surface")) {
		sdl.desktop.want_type=SCREEN_SURFACE;
	} else if (!strcasecmp(output,"overlay")) {
		sdl.desktop.want_type=SCREEN_OVERLAY;
#if C_OPENGL
	} else if (!strcasecmp(output,"opengl")) {
		sdl.desktop.want_type=SCREEN_OPENGL;
		sdl.opengl.bilinear=true;
	} else if (!strcasecmp(output,"openglnb")) {
		sdl.desktop.want_type=SCREEN_OPENGL;
		sdl.opengl.bilinear=false;
#endif
	} else {
		LOG_MSG("SDL:Unsupported output device %s, switching back to surface",output);
		sdl.desktop.want_type=SCREEN_SURFACE;
	}

	sdl.overlay=0;
#if C_OPENGL
   if(sdl.desktop.want_type==SCREEN_OPENGL){ /* OPENGL is requested */
	sdl.surface=SDL_SetVideoMode(640,400,0,SDL_OPENGL);
	sdl.opengl.framebuf=0;
	sdl.opengl.texture=0;
	sdl.opengl.displaylist=0;
	glGetIntegerv (GL_MAX_TEXTURE_SIZE, &sdl.opengl.max_texsize);
#if defined(__WIN32__) && defined(NVIDIA_PixelDataRange)
	glPixelDataRangeNV = (PFNGLPIXELDATARANGENVPROC) wglGetProcAddress("glPixelDataRangeNV");
	db_glAllocateMemoryNV = (PFNWGLALLOCATEMEMORYNVPROC) wglGetProcAddress("wglAllocateMemoryNV");
	db_glFreeMemoryNV = (PFNWGLFREEMEMORYNVPROC) wglGetProcAddress("wglFreeMemoryNV");
#endif
	const char * gl_ext = (const char *)glGetString (GL_EXTENSIONS);
	if(gl_ext && *gl_ext){
		sdl.opengl.packed_pixel=(strstr(gl_ext,"EXT_packed_pixels") > 0);
		sdl.opengl.paletted_texture=(strstr(gl_ext,"EXT_paletted_texture") > 0);
#if defined(NVIDIA_PixelDataRange)
		sdl.opengl.pixel_data_range=(strstr(gl_ext,"GL_NV_pixel_data_range") >0 ) &&
			glPixelDataRangeNV && db_glAllocateMemoryNV && db_glFreeMemoryNV;
#endif
    	} else {
		sdl.opengl.packed_pixel=sdl.opengl.paletted_texture=false;
	}
	} /* OPENGL is requested end */
   
#endif	//OPENGL
	/* Initialize screen for first time */
	sdl.surface=SDL_SetVideoMode(640,400,0,0);
	sdl.desktop.bpp=sdl.surface->format->BitsPerPixel;
	if (sdl.desktop.bpp==24) {
		LOG_MSG("SDL:You are running in 24 bpp mode, this will slow down things!");
	}
	GFX_Stop();
/* Get some Event handlers */
	MAPPER_AddHandler(KillSwitch,MK_f9,MMOD1,"shutdown","ShutDown");
	MAPPER_AddHandler(CaptureMouse,MK_f10,MMOD1,"capmouse","Cap Mouse");
	MAPPER_AddHandler(SwitchFullScreen,MK_return,MMOD2,"fullscr","Fullscreen");
}

void Mouse_AutoLock(bool enable) {
	sdl.mouse.autolock=enable;
	if (enable && sdl.mouse.autoenable) sdl.mouse.requestlock=true;
	else sdl.mouse.requestlock=false;
}

static void HandleMouseMotion(SDL_MouseMotionEvent * motion) {
	if (sdl.mouse.locked) 
		Mouse_CursorMoved((float)motion->xrel*sdl.mouse.sensitivity/100,(float)motion->yrel*sdl.mouse.sensitivity/100);
}

static void HandleMouseButton(SDL_MouseButtonEvent * button) {
	switch (button->state) {
	case SDL_PRESSED:
		if (sdl.mouse.requestlock && !sdl.mouse.locked) {
			CaptureMouse();
			// Dont pass klick to mouse handler
			break;
		}
		switch (button->button) {
		case SDL_BUTTON_LEFT:
			Mouse_ButtonPressed(0);
			break;
		case SDL_BUTTON_RIGHT:
			Mouse_ButtonPressed(1);
			break;
		case SDL_BUTTON_MIDDLE:
			Mouse_ButtonPressed(2);
			break;
		}
		break;
	case SDL_RELEASED:
		switch (button->button) {
		case SDL_BUTTON_LEFT:
			Mouse_ButtonReleased(0);
			break;
		case SDL_BUTTON_RIGHT:
			Mouse_ButtonReleased(1);
			break;
		case SDL_BUTTON_MIDDLE:
			Mouse_ButtonReleased(2);
			break;
		}
		break;
	}
}

void GFX_Events() {
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
	    switch (event.type) {
		case SDL_ACTIVEEVENT:
			if (event.active.state & SDL_APPINPUTFOCUS) {
				if (!event.active.gain && sdl.mouse.locked) {
					CaptureMouse();	
				}
			}
			break;
		case SDL_MOUSEMOTION:
			HandleMouseMotion(&event.motion);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			HandleMouseButton(&event.button);
			break;
		case SDL_VIDEORESIZE:
//			HandleVideoResize(&event.resize);
			break;
		case SDL_QUIT:
			throw(0);
			break;
		default:
			void MAPPER_CheckEvent(SDL_Event * event);
			MAPPER_CheckEvent(&event);
		}
    }
}

void GFX_ShowMsg(char * format,...) {
	char buf[512];
	va_list msg;
	va_start(msg,format);
	vsprintf(buf,format,msg);
        strcat(buf,"\n");
	va_end(msg);
	printf(buf);       
};

int main(int argc, char* argv[]) {
	try {
		CommandLine com_line(argc,argv);
		Config myconf(&com_line);
		control=&myconf;
		if (control->cmdline->FindExist("-version") || 
		    control->cmdline->FindExist("--version") ) {
			printf(VERSION "\n");
			return 0;
		}
	   

		/* Can't disable the console with debugger enabled */
#if defined(WIN32) && !(C_DEBUG)
		if (control->cmdline->FindExist("-noconsole")) {
			FreeConsole();
			/* Redirect standard input and standard output */
			freopen(STDOUT_FILE, "w", stdout);
			freopen(STDERR_FILE, "w", stderr);
			setvbuf(stdout, NULL, _IOLBF, BUFSIZ);	/* Line buffered */
			setbuf(stderr, NULL);					/* No buffering */
		} else {
			if (AllocConsole()) {
				fclose(stdin);
				fclose(stdout);
				fclose(stderr);
				freopen("CONIN$","w",stdin);
				freopen("CONOUT$","w",stdout);
				freopen("CONOUT$","w",stderr);
			}
		}
#endif  //defined(WIN32) && !(C_DEBUG)
#if C_DEBUG
		DEBUG_SetupConsole();
#endif
	if ( SDL_Init( SDL_INIT_AUDIO|SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_CDROM
#ifndef DISABLE_JOYSTICK
		|SDL_INIT_JOYSTICK
	
#endif
		) < 0 ) E_Exit("Can't init SDL %s",SDL_GetError());
		Section_prop * sdl_sec=control->AddSection_prop("sdl",&GUI_StartUp);
		sdl_sec->AddInitFunction(&MAPPER_StartUp);
		sdl_sec->Add_bool("fullscreen",false);
		sdl_sec->Add_bool("fulldouble",false);
		sdl_sec->Add_bool("fullfixed",false);
		sdl_sec->Add_int("fullwidth",0);
		sdl_sec->Add_int("fullheight",0);
		sdl_sec->Add_string("output","surface");
		sdl_sec->Add_float("hwscale",1.0);
		sdl_sec->Add_bool("autolock",true);
		sdl_sec->Add_int("sensitivity",100);
		sdl_sec->Add_bool("waitonerror",true);
		sdl_sec->Add_string("mapperfile","mapper.txt");

		MSG_Add("SDL_CONFIGFILE_HELP",
			"fullscreen -- Start dosbox directly in fullscreen.\n"
			"fulldouble -- Use double buffering in fullscreen.\n"
			"fullfixed -- Don't resize the screen when in fullscreen.\n"
			"fullwidth/height -- What resolution to use for fullscreen, use together with fullfixed.\n"
			"output -- What to use for output: surface,overlay"
#if C_OPENGL
			",opengl,openglnb"
#endif
			".\n"
			"hwscale -- Extra scaling of window if the output device supports hardware scaling.\n"
			"autolock -- Mouse will automatically lock, if you click on the screen.\n"
			"sensitiviy -- Mouse sensitivity.\n"
			"waitonerror -- Wait before closing the console if dosbox has an error.\n"
		);
		/* Init all the dosbox subsystems */
		DOSBOX_Init();
		std::string config_file;
		if (control->cmdline->FindString("-conf",config_file,true)) {
			
		} else {
			config_file="dosbox.conf";
		}
		/* Parse the config file
		 * try open config file in $HOME if can't open dosbox.conf or specified file
		 */
		if (control->ParseConfigFile(config_file.c_str()) == false)  {
			if ((getenv("HOME") != NULL)) {
				config_file = (std::string)getenv("HOME") + 
					      (std::string)DEFAULT_CONFIG_FILE;
				if (control->ParseConfigFile(config_file.c_str()) == false) {
					LOG_MSG("CONFIG: Using default settings. Create a configfile to change them");
				}
			   
			}
		}
#if (ENVIRON_LINKED)
		control->ParseEnv(environ);
#endif
		/* Init all the sections */
		control->Init();
		/* Some extra SDL Functions */
		if (control->cmdline->FindExist("-fullscreen") || sdl_sec->Get_bool("fullscreen")) {
			if(!sdl.desktop.fullscreen) { //only switch if not allready in fullscreen
				SwitchFullScreen();
			}
		}
		MAPPER_Init();
		/* Start up main machine */
		control->StartUp();
		/* Shutdown everything */
	} catch (char * error) {
		LOG_MSG("Exit to error: %s",error);
		if(sdl.wait_on_error) {
			//TODO Maybe look for some way to show message in linux?
#if (C_DEBUG)
			LOG_MSG("Press enter to continue");
			fgetc(stdin);
#elif defined(WIN32)
			Sleep(5000);
#endif 
		}

	}
	catch (int){ 
		;//nothing pressed killswitch
	}
	catch(...){   
		throw;//dunno what happened. rethrow for sdl to catch
	}
	return 0;
};
