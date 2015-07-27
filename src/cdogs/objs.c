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
#include "objs.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "bullet_class.h"
#include "collision.h"
#include "config.h"
#include "damage.h"
#include "game_events.h"
#include "map.h"
#include "net_util.h"
#include "screen_shake.h"
#include "blit.h"
#include "pic_manager.h"
#include "pickup.h"
#include "defs.h"
#include "actors.h"
#include "gamedata.h"
#include "mission.h"
#include "game.h"
#include "utils.h"

#define SOUND_LOCK_MOBILE_OBJECT 12
#define SHOT_IMPULSE_DIVISOR 25

CArray gObjs;
CArray gMobObjs;
static unsigned int sObjUIDs = 0;


// Draw functions

static const Pic *GetObjectPic(const int id, Vec2i *offset)
{
	const TObject *obj = CArrayGet(&gObjs, id);
	return MapObjectGetPic(obj->Class, offset, obj->Health == 0);
}


static void DestroyObject(
	TObject *o, const int flags, const int playerUID, const int uid);
static void DamageObject(
	const int power, const int flags,
	const int playerUID, const int uid,
	TObject *o, const HitSounds *hitSounds)
{
	// Don't bother if object already destroyed
	if (o->Health <= 0)
	{
		return;
	}

	o->Health -= power;

	// Destroying objects and all the wonderful things that happen
	if (o->Health <= 0)
	{
		DestroyObject(o, flags, playerUID, uid);
	}

	if (hitSounds != NULL && power > 0)
	{
		GameEvent e = GameEventNew(GAME_EVENT_SOUND_AT);
		strcpy(e.u.SoundAt.Sound, hitSounds->Object);
		e.u.SoundAt.Pos = Vec2i2Net(Vec2iNew(o->tileItem.x, o->tileItem.y));
		e.u.SoundAt.IsHit = true;
		GameEventsEnqueue(&gGameEvents, e);
	}
}
static void DestroyObject(
	TObject *o, const int flags, const int playerUID, const int uid)
{
	o->Health = 0;

	// Update objective
	UpdateMissionObjective(&gMission, o->tileItem.flags, OBJECTIVE_DESTROY);
	// Extra score if objective
	if ((o->tileItem.flags & TILEITEM_OBJECTIVE) && playerUID >= 0)
	{
		GameEvent e = GameEventNew(GAME_EVENT_SCORE);
		e.u.Score.PlayerUID = playerUID;
		e.u.Score.Score = OBJECT_SCORE;
		GameEventsEnqueue(&gGameEvents, e);
	}

	// Weapons that go off when this object is destroyed
	const Vec2i realPos = Vec2iNew(o->tileItem.x, o->tileItem.y);
	const Vec2i fullPos = Vec2iReal2Full(realPos);
	for (int i = 0; i < (int)o->Class->DestroyGuns.size; i++)
	{
		const GunDescription **g = CArrayGet(&o->Class->DestroyGuns, i);
		GunAddBullets(*g, fullPos, 0, 0, flags, playerUID, uid, true);
	}

	// A wreck left after the destruction of this object
	GameEvent e = GameEventNew(GAME_EVENT_ADD_BULLET);
	strcpy(e.u.AddBullet.BulletClass, "fireball_wreck");
	e.u.AddBullet.MuzzlePos = Vec2i2Net(fullPos);
	e.u.AddBullet.MuzzleHeight = 0;
	e.u.AddBullet.Angle = 0;
	e.u.AddBullet.Elevation = 0;
	e.u.AddBullet.Flags = 0;
	e.u.AddBullet.PlayerUID = -1;
	e.u.AddBullet.UID = -1;
	GameEventsEnqueue(&gGameEvents, e);
	SoundPlayAt(&gSoundDevice, gSoundDevice.wreckSound, realPos);

	// Turn the object into a wreck, if available
	if (o->Class->Wreck.Pic)
	{
		o->tileItem.flags = TILEITEM_IS_WRECK;
	}
	else
	{
		ObjDestroy(o->tileItem.id);
	}

	// Update pathfinding cache since this object could have blocked a path
	// before
	PathCacheClear(&gPathCache);
}

bool CanHit(const int flags, const int uid, const TTileItem *target)
{
	switch (target->kind)
	{
	case KIND_CHARACTER:
		return CanHitCharacter(flags, uid, CArrayGet(&gActors, target->id));
	case KIND_OBJECT:
		return true;
	default:
		CASSERT(false, "cannot damage tile item kind");
		break;
	}
	return false;
}

static void DoDamageCharacter(
	const Vec2i hitVector,
	const int power,
	const int flags,
	const int playerUID,
	const int uid,
	TActor *actor,
	const special_damage_e special,
	const HitSounds *hitSounds,
	const bool allowFriendlyHitSound);
void Damage(
	const Vec2i hitVector,
	const int power,
	const int flags,
	const int playerUID,
	const int uid,
	const TileItemKind targetKind, const int targetUID,
	const special_damage_e special,
	const HitSounds *hitSounds,
	const bool allowFriendlyHitSound)
{
	switch (targetKind)
	{
	case KIND_CHARACTER:
		DoDamageCharacter(
			hitVector,
			power, flags, playerUID, uid,
			ActorGetByUID(targetUID), special,
			hitSounds, allowFriendlyHitSound);
		break;
	case KIND_OBJECT:
		DamageObject(
			power, flags, playerUID, uid, ObjGetByUID(targetUID),
			hitSounds);
		break;
	default:
		CASSERT(false, "cannot damage tile item kind");
		break;
	}
}
static void DoDamageCharacter(
	const Vec2i hitVector,
	const int power,
	const int flags,
	const int playerUID,
	const int uid,
	TActor *actor,
	const special_damage_e special,
	const HitSounds *hitSounds,
	const bool allowFriendlyHitSound)
{
	// Create events: hit, damage, score
	CASSERT(actor->isInUse, "Cannot damage nonexistent player");
	CASSERT(CanHitCharacter(flags, uid, actor), "damaging undamageable actor");
	const Vec2i realPos = Vec2iFull2Real(actor->Pos);
	GameEvent e = GameEventNew(GAME_EVENT_HIT_CHARACTER);
	e.u.HitCharacter.TargetId = actor->tileItem.id;
	e.u.HitCharacter.Special = special;
	GameEventsEnqueue(&gGameEvents, e);
	if (hitSounds != NULL &&
		!ActorIsImmune(actor, special) &&
		(allowFriendlyHitSound || !ActorIsInvulnerable(
		actor, flags, playerUID, gCampaign.Entry.Mode)))
	{
		e = GameEventNew(GAME_EVENT_SOUND_AT);
		strcpy(e.u.SoundAt.Sound, hitSounds->Flesh);
		e.u.SoundAt.Pos = Vec2i2Net(realPos);
		e.u.SoundAt.IsHit = true;
		GameEventsEnqueue(&gGameEvents, e);
	}
	if (ConfigGetBool(&gConfig, "Game.ShotsPushback"))
	{
		e = GameEventNew(GAME_EVENT_ACTOR_IMPULSE);
		e.u.ActorImpulse.Id = actor->tileItem.id;
		e.u.ActorImpulse.Vel = Vec2iScaleDiv(
			Vec2iScale(hitVector, power), SHOT_IMPULSE_DIVISOR);
		GameEventsEnqueue(&gGameEvents, e);
	}
	if (CanDamageCharacter(flags, playerUID, uid, actor, special))
	{
		e = GameEventNew(GAME_EVENT_DAMAGE_CHARACTER);
		e.u.ActorDamage.Power = power;
		e.u.ActorDamage.PlayerUID = playerUID;
		e.u.ActorDamage.TargetUID = actor->uid;
		e.u.ActorDamage.TargetPlayerUID = actor->PlayerUID;
		GameEventsEnqueue(&gGameEvents, e);

		if (ConfigGetEnum(&gConfig, "Game.Gore") != GORE_NONE)
		{
			e = GameEventNew(GAME_EVENT_ADD_PARTICLE);
			e.u.AddParticle.FullPos = actor->Pos;
			e.u.AddParticle.Z = 10 * Z_FACTOR;
			int bloodPower = power * 2;
			int bloodSize = 1;
			while (bloodPower > 0)
			{
				switch (bloodSize)
				{
				case 1:
					e.u.AddParticle.Class =
						StrParticleClass(&gParticleClasses, "blood1");
					break;
				case 2:
					e.u.AddParticle.Class =
						StrParticleClass(&gParticleClasses, "blood2");
					break;
				default:
					e.u.AddParticle.Class =
						StrParticleClass(&gParticleClasses, "blood3");
					break;
				}
				bloodSize++;
				if (bloodSize > 3)
				{
					bloodSize = 1;
				}
				if (ConfigGetBool(&gConfig, "Game.ShotsPushback"))
				{
					e.u.AddParticle.Vel = Vec2iScaleDiv(
						Vec2iScale(hitVector, (rand() % 8 + 8) * power),
						15 * SHOT_IMPULSE_DIVISOR);
				}
				else
				{
					e.u.AddParticle.Vel = Vec2iScaleDiv(
						Vec2iScale(hitVector, rand() % 8 + 8), 20);
				}
				e.u.AddParticle.Vel.x += (rand() % 128) - 64;
				e.u.AddParticle.Vel.y += (rand() % 128) - 64;
				e.u.AddParticle.Angle = RAND_DOUBLE(0, PI * 2);
				e.u.AddParticle.DZ = (rand() % 6) + 6;
				e.u.AddParticle.Spin = RAND_DOUBLE(-0.1, 0.1);
				GameEventsEnqueue(&gGameEvents, e);
				switch (ConfigGetEnum(&gConfig, "Game.Gore"))
				{
				case GORE_LOW:
					bloodPower /= 8;
					break;
				case GORE_MEDIUM:
					bloodPower /= 2;
					break;
				default:
					bloodPower = bloodPower * 7 / 8;
					break;
				}
			}
		}

		// Don't score for friendly or player hits
		const bool isFriendly =
			(actor->flags & FLAGS_GOOD_GUY) ||
			(!IsPVP(gCampaign.Entry.Mode) && actor->PlayerUID >= 0);
		if (playerUID >= 0 && power != 0 && !isFriendly)
		{
			// Calculate score based on
			// if they hit a penalty character
			e = GameEventNew(GAME_EVENT_SCORE);
			e.u.Score.PlayerUID = playerUID;
			if (actor->flags & FLAGS_PENALTY)
			{
				e.u.Score.Score = PENALTY_MULTIPLIER * power;
			}
			else
			{
				e.u.Score.Score = power;
			}
			GameEventsEnqueue(&gGameEvents, e);
		}
	}
}


void UpdateMobileObjects(int ticks)
{
	for (int i = 0; i < (int)gMobObjs.size; i++)
	{
		TMobileObject *obj = CArrayGet(&gMobObjs, i);
		if (!obj->isInUse)
		{
			continue;
		}
		if (!obj->updateFunc(obj, ticks))
		{
			GameEvent e = GameEventNew(GAME_EVENT_MOBILE_OBJECT_REMOVE);
			e.u.MobileObjectRemoveId = i;
			GameEventsEnqueue(&gGameEvents, e);
		}
		else
		{
			CPicUpdate(&obj->tileItem.CPic, ticks);
		}
	}
}


void ObjsInit(void)
{
	CArrayInit(&gObjs, sizeof(TObject));
	CArrayReserve(&gObjs, 1024);
	sObjUIDs = 0;
}
void ObjsTerminate(void)
{
	for (int i = 0; i < (int)gObjs.size; i++)
	{
		TObject *o = CArrayGet(&gObjs, i);
		if (o->isInUse)
		{
			ObjDestroy(i);
		}
	}
	CArrayTerminate(&gObjs);
}
int ObjsGetNextUID(void)
{
	return sObjUIDs++;
}

void ObjAdd(const NAddMapObject amo)
{
	// Find an empty slot in object list
	TObject *o = NULL;
	int i;
	for (i = 0; i < (int)gObjs.size; i++)
	{
		TObject *obj = CArrayGet(&gObjs, i);
		if (!obj->isInUse)
		{
			o = obj;
			break;
		}
	}
	if (o == NULL)
	{
		TObject obj;
		memset(&obj, 0, sizeof obj);
		CArrayPushBack(&gObjs, &obj);
		i = (int)gObjs.size - 1;
		o = CArrayGet(&gObjs, i);
	}
	memset(o, 0, sizeof *o);
	o->uid = amo.UID;
	o->Class = StrMapObject(amo.MapObjectClass);
	o->Health = amo.Health;
	o->tileItem.x = o->tileItem.y = -1;
	o->tileItem.flags = amo.TileItemFlags;
	o->tileItem.kind = KIND_OBJECT;
	o->tileItem.getPicFunc = GetObjectPic;
	o->tileItem.getActorPicsFunc = NULL;
	o->tileItem.size = o->Class->Size;
	o->tileItem.id = i;
	MapTryMoveTileItem(&gMap, &o->tileItem, Net2Vec2i(amo.Pos));
	o->isInUse = true;
}
void ObjDestroy(int id)
{
	TObject *o = CArrayGet(&gObjs, id);
	CASSERT(o->isInUse, "Destroying in-use object");
	MapRemoveTileItem(&gMap, &o->tileItem);
	o->isInUse = false;
}

bool ObjIsDangerous(const TObject *o)
{
	// TODO: something more sophisticated? Check if weapon is dangerous
	return o->Class->DestroyGuns.size > 0;
}

void UpdateObjects(const int ticks)
{
	for (int i = 0; i < (int)gObjs.size; i++)
	{
		TObject *obj = CArrayGet(&gObjs, i);
		if (!obj->isInUse)
		{
			continue;
		}
		switch (obj->Class->Type)
		{
		case MAP_OBJECT_TYPE_PICKUP_SPAWNER:
			if (gCampaign.IsClient) break;
			// If counter -1, it is inactive i.e. already spawned pickup
			if (obj->counter == -1)
			{
				break;
			}
			obj->counter -= ticks;
			if (obj->counter <= 0)
			{
				// Deactivate spawner by setting counter to -1
				// Spawner reactivated only when ammo taken
				obj->counter = -1;
				GameEvent e = GameEventNew(GAME_EVENT_ADD_PICKUP);
				e.u.AddPickup.UID = PickupsGetNextUID();
				strcpy(
					e.u.AddPickup.PickupClass,
					obj->Class->u.PickupClass->Name);
				e.u.AddPickup.IsRandomSpawned = false;
				e.u.AddPickup.SpawnerUID = obj->uid;
				e.u.AddPickup.TileItemFlags = 0;
				e.u.AddPickup.Pos =
					Vec2i2Net(Vec2iNew(obj->tileItem.x, obj->tileItem.y));
				GameEventsEnqueue(&gGameEvents, e);
			}
			break;
		default:
			// Do nothing
			break;
		}
	}
}

TObject *ObjGetByUID(const int uid)
{
	for (int i = 0; i < (int)gObjs.size; i++)
	{
		TObject *o = CArrayGet(&gObjs, i);
		if (o->uid == uid)
		{
			return o;
		}
	}
	CASSERT(false, "Cannot find object by UID");
	return NULL;
}


void MobObjsInit(void)
{
	CArrayInit(&gMobObjs, sizeof(TMobileObject));
	CArrayReserve(&gMobObjs, 1024);
}
void MobObjsTerminate(void)
{
	for (int i = 0; i < (int)gMobObjs.size; i++)
	{
		TMobileObject *m = CArrayGet(&gMobObjs, i);
		if (m->isInUse)
		{
			MobObjDestroy(i);
		}
	}
	CArrayTerminate(&gMobObjs);
}
void MobObjDestroy(int id)
{
	TMobileObject *m = CArrayGet(&gMobObjs, id);
	CASSERT(m->isInUse, "Destroying not-in-use mobobj");
	MapRemoveTileItem(&gMap, &m->tileItem);
	m->isInUse = false;
}

typedef struct
{
	bool HasHit;
	bool MultipleHits;
	TMobileObject *Obj;
} HitItemData;
static bool HitItemFunc(TTileItem *ti, void *data);
bool HitItem(TMobileObject *obj, const Vec2i pos, const bool multipleHits)
{
	// Don't hit if no damage dealt
	// This covers non-damaging debris explosions
	if (obj->bulletClass->Power <= 0 &&
		obj->bulletClass->Special == SPECIAL_NONE)
	{
		return 0;
	}

	// Get all items that collide
	HitItemData data;
	data.HasHit = false;
	data.MultipleHits = multipleHits;
	data.Obj = obj;
	CollideTileItems(
		&obj->tileItem, Vec2iFull2Real(pos),
		TILEITEM_CAN_BE_SHOT, COLLISIONTEAM_NONE,
		IsPVP(gCampaign.Entry.Mode),
		HitItemFunc, &data);
	return data.HasHit;
}
static bool HitItemFunc(TTileItem *ti, void *data)
{
	HitItemData *hData = data;
	hData->HasHit = CanHit(hData->Obj->flags, hData->Obj->uid, ti);
	if (hData->HasHit)
	{
		int targetUID = -1;
		switch (ti->kind)
		{
		case KIND_CHARACTER:
			targetUID = ((const TActor *)CArrayGet(&gActors, ti->id))->uid;
			break;
		case KIND_OBJECT:
			targetUID = ((const TObject *)CArrayGet(&gObjs, ti->id))->uid;
			break;
		default:
			CASSERT(false, "cannot damage target kind");
			break;
		}
		Damage(
			hData->Obj->vel, hData->Obj->bulletClass->Power,
			hData->Obj->flags, hData->Obj->PlayerUID, hData->Obj->uid,
			ti->kind, targetUID,
			hData->Obj->bulletClass->Special,
			hData->Obj->soundLock <= 0 ? &hData->Obj->bulletClass->HitSound : NULL,
			true);
		if (hData->Obj->soundLock <= 0)
		{
			hData->Obj->soundLock += SOUND_LOCK_MOBILE_OBJECT;
		}
	}
	// Whether to produce multiple hits from the same TMobileObject
	return hData->MultipleHits;
}
