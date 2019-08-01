/*
    C-Dogs SDL
    A port of the legendary (and fun) action/arcade cdogs.
    Copyright (C) 1995 Ronny Wester
    Copyright (C) 2003 Jeremy Chin
    Copyright (C) 2003-2007 Lucas Martin-King

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    This file incorporates work covered by the following copyright and
    permission notice:

    Copyright (c) 2013-2015, Cong Xu
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.
    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/
#include "grafx.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>

#include <SDL_events.h>
#include <SDL_mouse.h>

#include "blit.h"
#include "config.h"
#include "defs.h"
#include "grafx_bg.h"
#include "hqx/hqx.h"
#include "log.h"
#include "palette.h"
#include "files.h"
#include "utils.h"


GraphicsDevice gGraphicsDevice;

static void Gfx_ModeSet(const GraphicsMode *mode)
{
	ConfigGet(&gConfig, "Graphics.ResolutionWidth")->u.Int.Value = mode->Width;
	ConfigGet(&gConfig, "Graphics.ResolutionHeight")->u.Int.Value = mode->Height;
	ConfigGet(&gConfig, "Graphics.ScaleFactor")->u.Int.Value = mode->ScaleFactor;
}

void Gfx_ModePrev(void)
{
	GraphicsDevice *device = &gGraphicsDevice;
	device->modeIndex--;
	if (device->modeIndex < 0)
	{
		device->modeIndex = device->numValidModes - 1;
	}
	Gfx_ModeSet(&device->validModes[device->modeIndex]);
}

void Gfx_ModeNext(void)
{
	GraphicsDevice *device = &gGraphicsDevice;
	device->modeIndex++;
	if (device->modeIndex >= device->numValidModes)
	{
		device->modeIndex = 0;
	}
	Gfx_ModeSet(&device->validModes[device->modeIndex]);
}

static int FindValidMode(GraphicsDevice *device, int width, int height, int scaleFactor)
{
	int i;
	for (i = 0; i < device->numValidModes; i++)
	{
		GraphicsMode *mode = &device->validModes[i];
		if (mode->Width == width &&
			mode->Height == height &&
			mode->ScaleFactor == scaleFactor)
		{
			return i;
		}
	}
	return -1;
}

static void AddGraphicsMode(
	GraphicsDevice *device, int width, int height, int scaleFactor)
{
	int i = 0;
	int actualResolutionToAdd = width * height * scaleFactor * scaleFactor;
	GraphicsMode *mode = NULL;

	// Don't add if mode already exists
	if (FindValidMode(device, width, height, scaleFactor) != -1)
	{
		return;
	}

	for (; i < device->numValidModes; i++)
	{
		// Ordered by actual resolution ascending and scale descending
		int actualResolution;
		mode = &device->validModes[i];
		actualResolution =
			mode->Width * mode->Height * mode->ScaleFactor * mode->ScaleFactor;
		if (actualResolution > actualResolutionToAdd ||
			(actualResolution == actualResolutionToAdd &&
			mode->ScaleFactor < scaleFactor))
		{
			break;
		}
	}
	device->numValidModes++;
	CREALLOC(device->validModes, device->numValidModes * sizeof *device->validModes);
	// If inserting, move later modes one place further
	if (i < device->numValidModes - 1)
	{
		memmove(
			device->validModes + i + 1,
			device->validModes + i,
			(device->numValidModes - 1 - i) * sizeof *device->validModes);
	}
	mode = &device->validModes[i];
	mode->Width = width;
	mode->Height = height;
	mode->ScaleFactor = scaleFactor;
}

void GraphicsInit(GraphicsDevice *device, Config *c)
{
	device->IsInitialized = 0;
	device->IsWindowInitialized = 0;
	device->screen = NULL;
	memset(&device->cachedConfig, 0, sizeof device->cachedConfig);
	device->validModes = NULL;
	device->clipping.left = 0;
	device->clipping.top = 0;
	device->clipping.right = 0;
	device->clipping.bottom = 0;
	device->numValidModes = 0;
	device->modeIndex = 0;
	// Add default modes
	AddGraphicsMode(device, 320, 240, 1);
	AddGraphicsMode(device, 400, 300, 1);
	AddGraphicsMode(device, 640, 480, 1);
	AddGraphicsMode(device, 320, 240, 2);
	device->buf = NULL;
	device->bkg = NULL;
	hqxInit();
	GraphicsConfigSetFromConfig(&device->cachedConfig, c);
}

void AddSupportedModesForBPP(GraphicsDevice *device, int bpp)
{
	SDL_Rect** modes;
	SDL_PixelFormat fmt;
	int i;
	memset(&fmt, 0, sizeof fmt);
	fmt.BitsPerPixel = (Uint8)bpp;

	modes = SDL_ListModes(&fmt, SDL_FULLSCREEN);
	if (modes == (SDL_Rect**)0)
	{
		return;
	}
	if (modes == (SDL_Rect**)-1)
	{
		return;
	}

	for (i = 0; modes[i]; i++)
	{
		int validScaleFactors[] = { 1, 2, 3, 4 };
		int j;
		for (j = 0; j < 4; j++)
		{
			int scaleFactor = validScaleFactors[j];
			int w, h;
			if (modes[i]->w % scaleFactor || modes[i]->h % scaleFactor)
			{
				continue;
			}
			if (modes[i]->w % 4)
			{
				// TODO: why does width have to be divisible by 4? 1366x768 doesn't work
				continue;
			}
			w = modes[i]->w / scaleFactor;
			h = modes[i]->h / scaleFactor;
			if (w < 320 || h < 240)
			{
				break;
			}
			AddGraphicsMode(device, w, h, scaleFactor);
		}
	}
}

void AddSupportedGraphicsModes(GraphicsDevice *device)
{
	AddSupportedModesForBPP(device, 16);
	AddSupportedModesForBPP(device, 24);
	AddSupportedModesForBPP(device, 32);
}

// Initialises the video subsystem.
// To prevent needless screen flickering, config is compared with cache
// to see if anything changed. If not, don't recreate the screen.
void GraphicsInitialize(GraphicsDevice *g, const bool force)
{
#ifdef __GCWZERO__
	int sdl_flags = SDL_HWSURFACE | SDL_TRIPLEBUF;
#else
	int sdl_flags = SDL_SWSURFACE;
#endif
	unsigned int w, h = 0;
	unsigned int rw, rh;

	if (g->IsInitialized && !g->cachedConfig.needRestart)
	{
		return;
	}

	if (!g->IsWindowInitialized)
	{
		/* only do this the first time */
		char title[32];
		debug(D_NORMAL, "setting caption and icon...\n");
		sprintf(title, "C-Dogs SDL %s%s",
			g->cachedConfig.IsEditor ? "Editor " : "",
			CDOGS_SDL_VERSION);
		SDL_WM_SetCaption(title, NULL);
		char buf[CDOGS_PATH_MAX];
		GetDataFilePath(buf, "cdogs_icon.bmp");
		g->icon = SDL_LoadBMP(buf);
		SDL_WM_SetIcon(g->icon, NULL);
		AddSupportedGraphicsModes(g);
	}

	g->IsInitialized = false;

	sdl_flags |= g->cachedConfig.IsEditor ? SDL_RESIZABLE : 0;
	if (g->cachedConfig.Fullscreen)
	{
		sdl_flags |= SDL_FULLSCREEN;
	}

	rw = w = g->cachedConfig.Res.x;
	rh = h = g->cachedConfig.Res.y;

	if (g->cachedConfig.ScaleFactor > 1)
	{
		rw *= g->cachedConfig.ScaleFactor;
		rh *= g->cachedConfig.ScaleFactor;
	}

	if (!force && !g->cachedConfig.IsEditor)
	{
		g->modeIndex = FindValidMode(g, w, h, g->cachedConfig.ScaleFactor);
		if (g->modeIndex == -1)
		{
			g->modeIndex = 0;
			LOG(LM_MAIN, LL_ERROR, "invalid Video Mode %dx%d\n", w, h);
			return;
		}
	}

	LOG(LM_MAIN, LL_INFO, "graphics mode(%dx%d %dx) actual(%dx%d)",
		w, h, g->cachedConfig.ScaleFactor, rw, rh);
	SDL_FreeSurface(g->screen);
	//g->screen = SDL_SetVideoMode(rw, rh, 16, sdl_flags);
	g->ScreenSurface = SDL_SetVideoMode(320, 240, 16, SDL_HWSURFACE);
	g->screen = SDL_CreateRGBSurface(SDL_SWSURFACE, rw, rh, 32, 0, 0, 0, 0);
	if (g->screen == NULL)
	{
		printf("ERROR: InitVideo: %s\n", SDL_GetError());
		return;
	}
	SDL_PixelFormat *f = g->screen->format;
	g->Amask = -1 & ~(f->Rmask | f->Gmask | f->Bmask);
	Uint32 aMask = g->Amask;
	g->Ashift = 0;
	while (aMask != 0xff)
	{
		g->Ashift += 8;
		aMask >>= 8;
	}

	CFREE(g->buf);
	CCALLOC(g->buf, GraphicsGetMemSize(&g->cachedConfig));
	CFREE(g->bkg);
	CCALLOC(g->bkg, GraphicsGetMemSize(&g->cachedConfig));

	debug(D_NORMAL, "Changed video mode...\n");

	GraphicsSetBlitClip(
		g, 0, 0, g->cachedConfig.Res.x - 1, g->cachedConfig.Res.y - 1);
	debug(D_NORMAL, "Internal dimensions:\t%dx%d\n",
		g->cachedConfig.Res.x, g->cachedConfig.Res.y);

	g->IsInitialized = true;
	g->IsWindowInitialized = true;
	g->cachedConfig.Res.x = w;
	g->cachedConfig.Res.y = h;
	g->cachedConfig.needRestart = false;
}

void GraphicsTerminate(GraphicsDevice *device)
{
	debug(D_NORMAL, "Shutting down video...\n");
	SDL_FreeSurface(device->icon);
	SDL_FreeSurface(device->screen);
	SDL_VideoQuit();
	CFREE(device->buf);
	CFREE(device->bkg);
}

int GraphicsGetScreenSize(GraphicsConfig *config)
{
	return config->Res.x * config->Res.y;
}

int GraphicsGetMemSize(GraphicsConfig *config)
{
	return GraphicsGetScreenSize(config) * sizeof(Uint32);
}

void GraphicsConfigSet(
	GraphicsConfig *c,
	const Vec2i res, const bool fullscreen, const int scaleFactor)
{
	if (!Vec2iEqual(res, c->Res))
	{
		c->Res = res;
		c->needRestart = true;
	}
#define SET(_lhs, _rhs) \
	if ((_lhs) != (_rhs)) \
	{ \
		(_lhs) = (_rhs); \
		c->needRestart = true; \
	}
	SET(c->Fullscreen, fullscreen);
	SET(c->ScaleFactor, scaleFactor);
}

void GraphicsConfigSetFromConfig(GraphicsConfig *gc, Config *c)
{
	GraphicsConfigSet(
		gc,
		Vec2iNew(
			ConfigGetInt(c, "Graphics.ResolutionWidth"),
			ConfigGetInt(c, "Graphics.ResolutionHeight")),
		ConfigGetBool(c, "Graphics.Fullscreen"),
		ConfigGetInt(c, "Graphics.ScaleFactor"));
}

char *GrafxGetModeStr(void)
{
	static char buf[16];
	sprintf(buf, "%dx%d %dx",
		ConfigGetInt(&gConfig, "Graphics.ResolutionWidth"),
		ConfigGetInt(&gConfig, "Graphics.ResolutionHeight"),
		ConfigGetInt(&gConfig, "Graphics.ScaleFactor"));
	return buf;
}

void GraphicsSetBlitClip(
	GraphicsDevice *device, int left, int top, int right, int bottom)
{
	device->clipping.left = left;
	device->clipping.top = top;
	device->clipping.right = right;
	device->clipping.bottom = bottom;
}

void GraphicsResetBlitClip(GraphicsDevice *device)
{
	GraphicsSetBlitClip(
		device,
		0, 0,
		device->cachedConfig.Res.x - 1,
		device->cachedConfig.Res.y - 1);
}
