#include "weapon.h"
#include "player.h"
#include "pickup.h"
#include "weaponFireFunc.h"
#include <TFE_System/system.h>
#include <TFE_Jedi/Level/rtexture.h>
#include <TFE_Jedi/Level/robject.h>
#include <TFE_Jedi/Level/rsector.h>
#include <TFE_Jedi/InfSystem/message.h>
#include <TFE_Jedi/Renderer/RClassic_Fixed/rlightingFixed.h>
#include <TFE_Jedi/Renderer/RClassic_Fixed/robj3d_fixed/robj3dFixed.h>
#include <TFE_Jedi/Renderer/RClassic_Float/robj3d_float/robj3dFloat.h>
#include <TFE_Jedi/Renderer/virtualFramebuffer.h>
#include <TFE_Jedi/Renderer/jediRenderer.h>
#include <TFE_Jedi/Renderer/screenDraw.h>
#include <TFE_Jedi/Serialization/serialization.h>
#include <TFE_ExternalData/weaponExternal.h>
#include <TFE_Asset/voxelAsset.h>
#include <TFE_Asset/modelAsset_jedi.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_Jedi/Renderer/RClassic_GPU/modelGPU.h>
#include <TFE_Asset/imageAsset.h>
#include <TFE_FileSystem/fileutil.h>
#include "mission.h"
#include <TFE_FrontEndUI/console.h>

namespace TFE_VoxDbg
{
	bool noCull = true;
	bool noPortalClip = true;
	f32  wpnOffX = 1.0f;
	f32  wpnOffY = 1.245f;
	f32  wpnOffZ = 2.563f;
	f32  wpnYawOff = 173.924f;
	f32  wpnPitchScale = 0.1f;
	f32  wpnFwdPitchScale = 1.0f;
	f32  wpnRollScale = 0.0f;
}

namespace TFE_DarkForces
{
	static bool s_voxDbgRegistered = false;

	static void voxDbgRegisterCVars()
	{
		if (s_voxDbgRegistered) return;
		s_voxDbgRegistered = true;
		CVAR_FLOAT(TFE_VoxDbg::wpnOffX,  "v_wpnOffX",  CVFLAG_DO_NOT_SERIALIZE, "Voxel weapon X offset (right).");
		CVAR_FLOAT(TFE_VoxDbg::wpnOffY,  "v_wpnOffY",  CVFLAG_DO_NOT_SERIALIZE, "Voxel weapon Y offset (down).");
		CVAR_FLOAT(TFE_VoxDbg::wpnOffZ,  "v_wpnOffZ",  CVFLAG_DO_NOT_SERIALIZE, "Voxel weapon Z offset (forward).");
		CVAR_FLOAT(TFE_VoxDbg::wpnYawOff, "v_wpnYawOff", CVFLAG_DO_NOT_SERIALIZE, "Voxel weapon yaw offset in degrees.");
		CVAR_FLOAT(TFE_VoxDbg::wpnPitchScale, "v_wpnPitchScale", CVFLAG_DO_NOT_SERIALIZE, "Voxel weapon pitch multiplier.");
		CVAR_FLOAT(TFE_VoxDbg::wpnFwdPitchScale, "v_wpnFwdPitchScale", CVFLAG_DO_NOT_SERIALIZE, "Forward offset per unit of pitch.");
		CVAR_FLOAT(TFE_VoxDbg::wpnRollScale, "v_wpnRollScale", CVFLAG_DO_NOT_SERIALIZE, "Voxel weapon roll multiplier.");
		CVAR_BOOL(TFE_VoxDbg::noCull,  "v_noCull",   CVFLAG_DO_NOT_SERIALIZE, "Disable backface culling for all models.");
		CVAR_BOOL(TFE_VoxDbg::noPortalClip, "v_noPortalClip", CVFLAG_DO_NOT_SERIALIZE, "Disable portal clipping for all models.");
	}

	///////////////////////////////////////////
	// Internal State
	///////////////////////////////////////////
	static JBool s_weaponTexturesLoaded = JFALSE;
	static JBool s_switchWeapons = JFALSE;
	static JBool s_queWeaponSwitch = JFALSE;
	
	static TextureData* s_rhand1 = nullptr;
	static TextureData* s_gasmaskTexture = nullptr;
	static s32 s_gasMaskXpos;
	static s32 s_gasMaskYpos;
	static PlayerWeapon s_playerWeaponList[WPN_COUNT];
			
	// Voxel HUD weapon models.
	static JediModel* s_weaponVoxelModels[WPN_COUNT] = {};
	static bool s_weaponVoxelsLoaded = false;

	static const char* s_weaponVoxelNames[WPN_COUNT] =
	{
		nullptr,       // WPN_FIST
		"IPISTOL",     // WPN_PISTOL (bryar)
		"IAUTOGUN",    // WPN_RIFLE
		"IDETS",       // WPN_THERMAL_DET
		"IST-GUNU",    // WPN_REPEATER
		"IFUSION",     // WPN_FUSION
		"IMORTAR",     // WPN_MORTAR
		"IMINE",       // WPN_MINE
		"ICONCUS",     // WPN_CONCUSSION
		"ICANNON",     // WPN_CANNON
	};

	static bool s_weaponVoxelGpuDirty = false;
	static bool s_weaponSpritesDumped = false;

	static const char* s_weaponIdNames[WPN_COUNT] = {
		"FIST", "PISTOL", "RIFLE", "THERMAL_DET", "REPEATER",
		"FUSION", "MORTAR", "MINE", "CONCUSSION", "CANNON"
	};

	static void dumpWeaponSprites()
	{
		if (s_weaponSpritesDumped) { return; }
		s_weaponSpritesDumped = true;

		FileUtil::makeDirectory("weapon_sprites");

		const u8* pal = s_levelPalette;
		for (s32 w = 0; w < WPN_COUNT; w++)
		{
			PlayerWeapon& wpn = s_playerWeaponList[w];
			for (s32 f = 0; f < wpn.frameCount; f++)
			{
				TextureData* tex = wpn.frames[f];
				if (!tex || !tex->image) { continue; }

				const u32 width = tex->width;
				const u32 height = tex->height;
				u32* rgba = (u32*)malloc(width * height * 4);
				if (!rgba) { continue; }

				// BM textures are column-major: image[col * height + row], bottom-up.
				for (u32 y = 0; y < height; y++)
				{
					for (u32 x = 0; x < width; x++)
					{
						u8 idx = tex->image[x * height + (height - 1 - y)];
						u8 r = CONV_6bitTo8bit(pal[idx * 3 + 0]);
						u8 g = CONV_6bitTo8bit(pal[idx * 3 + 1]);
						u8 b = CONV_6bitTo8bit(pal[idx * 3 + 2]);
						u8 a = (idx == 0 && (tex->flags & OPACITY_TRANS)) ? 0 : 255;
						rgba[y * width + x] = (a << 24) | (b << 16) | (g << 8) | r;
					}
				}

				char path[256];
				snprintf(path, sizeof(path), "weapon_sprites/%s_frame%d.png", s_weaponIdNames[w], f);
				TFE_Image::writeImage(path, width, height, rgba);
				free(rgba);
			}
		}
	}

	static void loadWeaponVoxels()
	{
		if (s_weaponVoxelsLoaded) { return; }
		s_weaponVoxelsLoaded = true;

		bool anyLoaded = false;
		for (s32 i = 0; i < WPN_COUNT; i++)
		{
			if (s_weaponVoxelNames[i])
			{
				s_weaponVoxelModels[i] = TFE_Voxel::getModelForName(s_weaponVoxelNames[i], POOL_LEVEL);
				if (s_weaponVoxelModels[i]) { anyLoaded = true; }
			}
		}
		if (anyLoaded) { s_weaponVoxelGpuDirty = true; }
	}

	// Called when a new level loads so weapon voxels rebuild with the correct palette.
	void weapon_resetVoxels()
	{
		s_weaponVoxelsLoaded = false;
		s_weaponVoxelGpuDirty = false;
		for (s32 i = 0; i < WPN_COUNT; i++)
		{
			s_weaponVoxelModels[i] = nullptr;
		}
	}

	static Tick s_weaponDelayPrimary;
	static Tick s_weaponDelaySeconary;
	static s32* s_canFirePrimPtr;
	static s32* s_canFireSecPtr;

	WeaponAnimState s_weaponAnimState;

	s32 s_prevWeapon;
	s32 s_curWeapon;
	s32 s_nextWeapon;
	s32 s_lastWeapon;

	JBool s_weaponAutoMount2 = JFALSE;
	JBool s_secondaryFire = JFALSE;
	JBool s_weaponOffAnim = JFALSE;
	JBool s_isShooting = JFALSE;
	s32 s_canFireWeaponSec;
	s32 s_canFireWeaponPrim;
	u32 s_fireFrame = 0;
		
	SoundSourceId s_punchSwingSndSrc;
	SoundSourceId s_pistolSndSrc;
	SoundSourceId s_pistolEmptySndSrc;
	SoundSourceId s_rifleSndSrc;
	SoundSourceId s_rifleEmptySndSrc;
	SoundSourceId s_fusion1SndSrc;
	SoundSourceId s_fusion2SndSrc;
	SoundSourceId s_repeaterSndSrc;
	SoundSourceId s_repeater1SndSrc;
	SoundSourceId s_repeaterEmptySndSrc;
	SoundSourceId s_mortarFireSndSrc;
	SoundSourceId s_mortarFireSndSrc2;
	SoundSourceId s_mortarEmptySndSrc;
	SoundSourceId s_mineSndSrc;
	SoundSourceId s_concussion6SndSrc;
	SoundSourceId s_concussion5SndSrc;
	SoundSourceId s_concussion1SndSrc;
	SoundSourceId s_plasma4SndSrc;
	SoundSourceId s_plasmaEmptySndSrc;
	SoundSourceId s_missile1SndSrc;
	SoundSourceId s_weaponChangeSnd;

	SoundEffectId s_repeaterFireSndID = 0;

	///////////////////////////////////////////
	// Shared State
	///////////////////////////////////////////
	PlayerWeapon* s_curPlayerWeapon = nullptr;
	SoundSourceId s_superchargeCountdownSound;
	Task* s_playerWeaponTask = nullptr;

	///////////////////////////////////////////
	// Forward Declarations
	///////////////////////////////////////////
	TextureData* loadWeaponTexture(const char* texName);
	void weapon_setFireRateInternal(WeaponFireMode mode, Tick delay, s32* canFire);
	void weapon_playerWeaponTaskFunc(MessageType msg);
	void weapon_handleOnAnimation(MessageType msg);
	void weapon_setIdle();
	
	static WeaponFireFunc s_weaponFireFunc[WPN_COUNT] =
	{
		weaponFire_fist,			// WPN_FIST
		weaponFire_pistol,			// WPN_PISTOL
		weaponFire_rifle,			// WPN_RIFLE
		weaponFire_thermalDetonator,// WPN_THERMAL_DET
		weaponFire_repeater,		// WPN_REPEATER
		weaponFire_fusion,			// WPN_FUSION
		weaponFire_mortar,			// WPN_MORTAR
		weaponFire_mine,			// WPN_MINE
		weaponFire_concussion,		// WPN_CONCUSSION
		weaponFire_cannon			// WPN_CANNON
	};

	///////////////////////////////////////////
	// API Implementation
	///////////////////////////////////////////
	void weapon_resetState()
	{
		s_weaponTexturesLoaded = JFALSE;
		s_switchWeapons = JFALSE;
		s_queWeaponSwitch = JFALSE;

		s_rhand1 = nullptr;
		s_gasmaskTexture = nullptr;

		s_weaponAutoMount2 = JFALSE;
		s_secondaryFire = JFALSE;
		s_weaponOffAnim = JFALSE;
		s_isShooting = JFALSE;

		s_fireFrame = 0;
		s_curPlayerWeapon = nullptr;
		s_playerWeaponTask = nullptr;
	}

	void weapon_serialize(Stream* stream)
	{
		SERIALIZE(SaveVersionInit, s_weaponAnimState, { 0 });
		SERIALIZE(SaveVersionInit, s_prevWeapon, 0);
		SERIALIZE(SaveVersionInit, s_curWeapon, 0);
		SERIALIZE(SaveVersionInit, s_nextWeapon, 0);
		SERIALIZE(SaveVersionInit, s_lastWeapon, 0);

		SERIALIZE(SaveVersionInit, s_switchWeapons, JFALSE);
		SERIALIZE(SaveVersionInit, s_queWeaponSwitch, JFALSE);
		SERIALIZE(SaveVersionInit, s_weaponAutoMount2, JFALSE);
		SERIALIZE(SaveVersionInit, s_secondaryFire, JFALSE);
		SERIALIZE(SaveVersionInit, s_weaponOffAnim, JFALSE);
		SERIALIZE(SaveVersionInit, s_isShooting, JFALSE);
		SERIALIZE(SaveVersionInit, s_fireFrame, 0);

		s32 curWpnIndex = -1;
		if (serialization_getMode() == SMODE_WRITE && s_curPlayerWeapon)
		{
			curWpnIndex = PTR_INDEX_S32(s_curPlayerWeapon, s_playerWeaponList, sizeof(PlayerWeapon));
		}
		SERIALIZE(SaveVersionInit, curWpnIndex, -1);
		if (serialization_getMode() == SMODE_READ)
		{
			s_curPlayerWeapon = nullptr;
			if (curWpnIndex >= 0)
			{
				s_curPlayerWeapon = &s_playerWeaponList[curWpnIndex];
			}
		}

		if (s_curPlayerWeapon)
		{
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->frame, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->flags, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->rollOffset, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->pchOffset, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->xWaveOffset, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->yWaveOffset, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->xOffset, 0);
			SERIALIZE(SaveVersionInit, s_curPlayerWeapon->yOffset, 0);

			// Reset the frame for now.
			if (serialization_getMode() == SMODE_READ)
			{
				s_curPlayerWeapon->frame = 0;
			}
		}
	}

	// TFE: Set weapon data from external data
	void setWeaponData(WeaponID id, TFE_ExternalData::ExternalWeapon* extWeapons)
	{
		s_playerWeaponList[id].frameCount = min(extWeapons[id].frameCount, WEAPON_NUM_TEXTURES);
		s_playerWeaponList[id].frame = 0;
		
		for (s32 f = 0; f < WEAPON_NUM_TEXTURES; f++)
		{
			const char* tex = extWeapons[id].textures[f];
			s_playerWeaponList[id].frames[f] = tex ? loadWeaponTexture(tex) : nullptr;
			s_playerWeaponList[id].xPos[f] = extWeapons[id].xPos[f];
			s_playerWeaponList[id].yPos[f] = extWeapons[id].yPos[f];
		}

		s_playerWeaponList[id].flags = 7;
		s_playerWeaponList[id].rollOffset = 0;
		s_playerWeaponList[id].pchOffset = 0;
		s_playerWeaponList[id].xWaveOffset = 0;
		s_playerWeaponList[id].yWaveOffset = 0;
		s_playerWeaponList[id].xOffset = 0;
		s_playerWeaponList[id].yOffset = 0;
		s_playerWeaponList[id].ammo = extWeapons[id].ammo;
		s_playerWeaponList[id].secondaryAmmo = extWeapons[id].secondaryAmmo;
		s_playerWeaponList[id].u8c = 0;
		s_playerWeaponList[id].u90 = 0;
		s_playerWeaponList[id].wakeupRange = FIXED(extWeapons[id].wakeupRange);
		s_playerWeaponList[id].variation = extWeapons[id].variation;
		s_playerWeaponList[id].primaryFireConsumption = extWeapons[id].primaryFireConsumption;
		s_playerWeaponList[id].secondaryFireConsumption = extWeapons[id].secondaryFireConsumption;

		setupAnimationFrames(id, extWeapons[id].numAnimFrames, extWeapons[id].animFrames, extWeapons[id].numSecondaryAnimFrames, extWeapons[id].animFramesSecondary);
	}
	
	void weapon_startup()
	{
		// TFE: Weapon data is set from external data instead of being hardcoded as in vanilla DF
		TFE_ExternalData::ExternalWeapon* externalWeapons = TFE_ExternalData::getExternalWeapons();
		setWeaponData(WPN_FIST, externalWeapons);
		setWeaponData(WPN_PISTOL, externalWeapons);
		setWeaponData(WPN_RIFLE, externalWeapons);
		setWeaponData(WPN_THERMAL_DET, externalWeapons);
		setWeaponData(WPN_REPEATER, externalWeapons);
		setWeaponData(WPN_FUSION, externalWeapons);
		setWeaponData(WPN_MORTAR, externalWeapons);
		setWeaponData(WPN_MINE, externalWeapons);
		setWeaponData(WPN_CONCUSSION, externalWeapons);
		setWeaponData(WPN_CANNON, externalWeapons);

		s_gasmaskTexture = loadWeaponTexture(TFE_ExternalData::getExternalGasmask()->texture);
		s_gasMaskXpos = TFE_ExternalData::getExternalGasmask()->xPos;
		s_gasMaskYpos = TFE_ExternalData::getExternalGasmask()->yPos;

		// Load Sounds.
		s_punchSwingSndSrc    = sound_load("swing.voc",    SOUND_PRIORITY_HIGH3);
		s_pistolSndSrc        = sound_load("pistol-1.voc", SOUND_PRIORITY_HIGH0);
		s_pistolEmptySndSrc   = sound_load("pistout1.voc", SOUND_PRIORITY_HIGH0);
		s_rifleSndSrc         = sound_load("rifle-1.voc",  SOUND_PRIORITY_HIGH0);
		s_rifleEmptySndSrc    = sound_load("riflout.voc",  SOUND_PRIORITY_HIGH0);
		s_fusion1SndSrc       = sound_load("fusion1.voc",  SOUND_PRIORITY_HIGH3);
		s_fusion2SndSrc       = sound_load("fusion2.voc",  SOUND_PRIORITY_HIGH3);
		s_repeaterSndSrc      = sound_load("repeater.voc", SOUND_PRIORITY_HIGH3);
		s_repeater1SndSrc     = sound_load("repeat-1.voc", SOUND_PRIORITY_HIGH3);
		s_repeaterEmptySndSrc = sound_load("rep-emp.voc",  SOUND_PRIORITY_HIGH3);
		s_mortarFireSndSrc    = sound_load("mortar4.voc",  SOUND_PRIORITY_HIGH3);
		s_mortarFireSndSrc2   = sound_load("mortar2.voc",  SOUND_PRIORITY_HIGH3);
		s_mortarEmptySndSrc   = sound_load("mortar9.voc",  SOUND_PRIORITY_HIGH3);
		s_mineSndSrc          = sound_load("claymor1.voc", SOUND_PRIORITY_HIGH3);
		s_concussion6SndSrc   = sound_load("concuss6.voc", SOUND_PRIORITY_HIGH3);
		s_concussion5SndSrc   = sound_load("concuss5.voc", SOUND_PRIORITY_HIGH3);
		s_concussion1SndSrc   = sound_load("concuss1.voc", SOUND_PRIORITY_HIGH3);
		s_plasma4SndSrc       = sound_load("plasma4.voc",  SOUND_PRIORITY_HIGH3);
		s_plasmaEmptySndSrc   = sound_load("plas-emp.voc", SOUND_PRIORITY_HIGH3);
		s_missile1SndSrc      = sound_load("missile1.voc", SOUND_PRIORITY_HIGH3);
		s_weaponChangeSnd     = sound_load("weapon1.voc",  SOUND_PRIORITY_LOW4);
		s_superchargeCountdownSound = sound_load("quarter.voc", SOUND_PRIORITY_HIGH4);

		s_isShooting       = JFALSE;
		s_secondaryFire    = JFALSE;
		s_superCharge      = JFALSE;
		s_superChargeHud   = JFALSE;
		s_superchargeTask  = nullptr;
		s_weaponAutoMount2 = JFALSE;
		s_prevWeapon = WPN_PISTOL;
		s_curWeapon  = WPN_PISTOL;
		s_lastWeapon = WPN_PISTOL;
		s_curPlayerWeapon = &s_playerWeaponList[WPN_PISTOL];
	}

	void weapon_enableAutomount(JBool enable)
	{
		s_weaponAutoMount2 = enable;
	}

	void player_cycleWeapons(s32 change)
	{
		s32 dir = (change < 0) ? -1 : 1;
		s32 nextWeapon = s_curWeapon + change;
		JBool hasWeapon = JFALSE;
		while (!hasWeapon)
		{
			if (nextWeapon < 0)
			{
				nextWeapon = WPN_COUNT - 1;
			}
			else if (nextWeapon == WPN_COUNT)
			{
				nextWeapon = WPN_FIST;
			}

			switch (nextWeapon)
			{
				case WPN_FIST:
				{
					hasWeapon = JTRUE;
				} break;
				case WPN_PISTOL:
				{
					if (!s_playerInfo.itemPistol) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_RIFLE:
				{
					if (!s_playerInfo.itemRifle) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_THERMAL_DET:
				{
					// TODO(Core Game Loop Release): Check this.
					if (!s_playerInfo.ammoDetonator) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_REPEATER:
				{
					if (!s_playerInfo.itemAutogun) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_FUSION:
				{
					if (!s_playerInfo.itemFusion) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_MORTAR:
				{
					if (!s_playerInfo.itemMortar) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_MINE:
				{
					// TODO(Core Game Loop Release): Check this.
					if (!s_playerInfo.ammoMine) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_CONCUSSION:
				{
					if (!s_playerInfo.itemConcussion) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
				case WPN_CANNON:
				{
					if (!s_playerInfo.itemCannon) { nextWeapon += dir; }
					else { hasWeapon = JTRUE; }
				} break;
			}
		}

		if (nextWeapon != s_playerInfo.curWeapon && s_playerWeaponTask)
		{
			s_msgArg1 = nextWeapon;
			task_runAndReturn(s_playerWeaponTask, MSG_SWITCH_WPN);

			if (s_playerInfo.curWeapon > s_playerInfo.maxWeapon)
			{
				s_playerInfo.maxWeapon = s_playerInfo.curWeapon;
			}
		}
		else
		{
			s_playerInfo.saveWeapon = nextWeapon;
		}
	}

	void weapon_setNext(s32 wpnIndex)
	{
		PlayerWeapon* prevWeapon = s_curPlayerWeapon;
		s_curWeapon = wpnIndex;

		s_prevWeapon = wpnIndex;
		PlayerWeapon* nextWeapon = &s_playerWeaponList[wpnIndex];
		s_playerInfo.curWeapon = wpnIndex;

		if (wpnIndex == WPN_THERMAL_DET && !(*nextWeapon->ammo))
		{
			nextWeapon->frame = 3;
		}
		else if (wpnIndex == WPN_MINE && !(*nextWeapon->ammo))
		{
			nextWeapon->frame = 2;
		}
		else
		{
			nextWeapon->frame = 0;
		}

		s_curPlayerWeapon = nextWeapon;
		weapon_setFireRate();

		PlayerWeapon* weapon = s_curPlayerWeapon;
		weapon->rollOffset  = prevWeapon->rollOffset;
		weapon->pchOffset   = prevWeapon->pchOffset;
		weapon->xWaveOffset = prevWeapon->xWaveOffset;
		weapon->yWaveOffset = prevWeapon->yWaveOffset;
		weapon->xOffset     = prevWeapon->xOffset;
		weapon->yOffset     = prevWeapon->yOffset;
	}

	void weapon_setFireRate()
	{
		s_canFireWeaponPrim = 1;
		s_canFireWeaponSec = 1;

		// Clear both fire modes.
		weapon_setFireRateInternal(WFIRE_PRIMARY,   0, nullptr);
		weapon_setFireRateInternal(WFIRE_SECONDARY, 0, nullptr);

		// These values are set but never read. Actual fire rate is determined by the animation frames durations.
		switch (s_prevWeapon)
		{
			case WPN_PISTOL:
			{
				Tick fireDelay = s_superCharge ? 35 : 71;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);
			} break;
			case WPN_RIFLE:
			{
				Tick fireDelay = s_superCharge ? 10 : 21;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);
			} break;
			case WPN_THERMAL_DET:
			case WPN_MINE:
			{
				weapon_setFireRateInternal(WFIRE_PRIMARY, 1, &s_canFireWeaponPrim);
			} break;
			case WPN_REPEATER:
			{
				Tick fireDelay = s_superCharge ? 14 : 30;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);

				fireDelay = s_superCharge ? 18 : 37;
				weapon_setFireRateInternal(WFIRE_SECONDARY, fireDelay, &s_canFireWeaponSec);
			} break;
			case WPN_FUSION:
			{
				Tick fireDelay = s_superCharge ? 17 : 35;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);

				fireDelay = s_superCharge ? 33 : 68;
				weapon_setFireRateInternal(WFIRE_SECONDARY, fireDelay, &s_canFireWeaponSec);
			} break;
			case WPN_MORTAR:
			{
				Tick fireDelay = s_superCharge ? 50 : 100;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);
			} break;
			case WPN_CONCUSSION:
			{
				Tick fireDelay = s_superCharge ? 57 : 115;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);
			} break;
			case WPN_CANNON:
			{
				Tick fireDelay = s_superCharge ? 17 : 35;
				weapon_setFireRateInternal(WFIRE_PRIMARY, fireDelay, &s_canFireWeaponPrim);

				fireDelay = s_superCharge ? 86 : 87;
				weapon_setFireRateInternal(WFIRE_SECONDARY, fireDelay, &s_canFireWeaponSec);
			} break;
		}
	}

	void weapon_clearFireRate()
	{
		s_weaponDelayPrimary = 0;
		s_weaponDelaySeconary = 0;
	}
	
	void weapon_createPlayerWeaponTask()
	{
		//s_playerWeaponTask = createSubTask("player weapon", weapon_playerWeaponTaskFunc);
		// TODO: In the original code, createTask() was called - but then the firing is a frame behind.
		// Making it a "createTask" instead makes it happen before the main task, which is what we want.
		s_playerWeaponTask = createTask("player weapon", weapon_playerWeaponTaskFunc);

		s_superCharge = JFALSE;
		s_superChargeHud = JFALSE;
		s_queWeaponSwitch = JFALSE;
		s_superchargeTask = nullptr;
	}

	// If using TDs or mines and no ammo, show empty right hand
	void weapon_emptyAnim()
	{
		PlayerWeapon* weapon = s_curPlayerWeapon;
		s32 ammo = weapon->ammo ? *weapon->ammo : 0;

		if (s_prevWeapon == WPN_THERMAL_DET && !ammo)
		{
			weapon->frame = 3;
		}
		else if (s_prevWeapon == WPN_MINE && !ammo)
		{
			weapon->frame = 2;
		}
	}

	void weapon_computeMatrix(fixed16_16* mtx, angle14_32 pitch, angle14_32 yaw)
	{
		fixed16_16 cosPch, sinPch;
		sinCosFixed(pitch, &sinPch, &cosPch);

		fixed16_16 cosYaw, sinYaw;
		sinCosFixed(yaw, &sinYaw, &cosYaw);

		mtx[0] = cosYaw;
		mtx[1] = 0;
		mtx[2] = sinYaw;
		mtx[3] = mul16(sinYaw, sinPch);
		mtx[4] = cosPch;
		mtx[5] = -mul16(cosYaw, sinPch);
		mtx[6] = -mul16(sinYaw, cosPch);
		mtx[7] = sinPch;
		mtx[8] = mul16(cosPch, cosYaw);
	}
		
	///////////////////////////////////////////
	// Internal Implementation
	///////////////////////////////////////////
	void weapon_setFireRateInternal(WeaponFireMode mode, Tick delay, s32* canFire)
	{
		if (mode == WFIRE_PRIMARY)
		{
			s_weaponDelayPrimary = delay;
			s_canFirePrimPtr = canFire;
		}
		else
		{
			s_weaponDelaySeconary = delay;
			s_canFireSecPtr = canFire;
		}
	}

	// Added for the early GPU renderer. No longer used
	void weapon_addTexture(TextureInfoList& texList, TextureData* tex)
	{
		TextureInfo texInfo = {};
		texInfo.type = TEXINFO_DF_TEXTURE_DATA;
		texInfo.texData = tex;
		texList.push_back(texInfo);
	}

	// Added for the early GPU renderer. No longer used
	bool weapon_getTextures(TextureInfoList& texList)
	{
		// Get weapon textures.
		weapon_addTexture(texList, s_rhand1);
		weapon_addTexture(texList, s_gasmaskTexture);
		for (s32 i = 0; i < WPN_COUNT; i++)
		{
			for (s32 f = 0; f < s_playerWeaponList[i].frameCount; f++)
			{
				weapon_addTexture(texList, s_playerWeaponList[i].frames[f]);
			}
		}
		return true;
	}

	TextureData* loadWeaponTexture(const char* texName)
	{
		TextureData* weaponTex = bitmap_load(texName, 1, POOL_GAME);
		if (!weaponTex)
		{
			TFE_System::logWrite(LOG_ERROR, "Weapon", "Weapon_Startup: %s NOT FOUND.", texName);
		}
		return weaponTex;
	}

	void weapon_setShooting(u32 secondaryFire)
	{
		s_isShooting = JTRUE;
		s_secondaryFire = secondaryFire ? JTRUE : JFALSE;

		if (s_weaponOffAnim)
		{
			// TODO(Core Game Loop Release)
		}

		PlayerWeapon* weapon = s_curPlayerWeapon;
		weapon->flags &= ~2;
		s32 wpnIndex = s_prevWeapon;

		if (wpnIndex == 4)
		{
			// TODO(Core Game Loop Release)
		}
		else if (wpnIndex == 9)
		{
			// TODO(Core Game Loop Release)
		}

		weapon_setFireRate();
	}

	void weapon_queueWeaponSwitch(s32 wpnId)
	{
		s_queWeaponSwitch = JTRUE;
		s_msgArg1 = wpnId;
		task_makeActive(s_playerWeaponTask);
	}

	void weapon_handleState(MessageType msg)
	{
		task_begin;
		if (msg == MSG_STOP_FIRING)
		{
			s_isShooting = JFALSE;
			weapon_setIdle();
			s_curPlayerWeapon->flags |= 2;
		}
		else if (msg == MSG_START_FIRING)
		{
			s_secondaryFire = s_msgArg1 ? JTRUE : JFALSE;
			s_isShooting = JTRUE;
			if (s_weaponOffAnim)
			{
				task_callTaskFunc(weapon_handleOnAnimation);
			}
			s_curPlayerWeapon->flags &= ~2;
		}
		else if (msg == MSG_SWITCH_WPN)
		{
			s_switchWeapons = JTRUE;
			s_nextWeapon = s_msgArg1;
		}
		task_end;
	}

	void weapon_stopFiring()
	{
		s_isShooting = JFALSE;
		if (s_prevWeapon == WPN_REPEATER)
		{
			if (s_repeaterFireSndID)
			{
				sound_stop(s_repeaterFireSndID);
				s_repeaterFireSndID = 0;
			}
			s_curPlayerWeapon->frame = 0;
		}
		else if (s_prevWeapon == WPN_CANNON)
		{
			if (s_secondaryFire)
			{
				s_curPlayerWeapon->frame = 0;
			}
		}
		PlayerWeapon* weapon = s_curPlayerWeapon;
		weapon->flags |= 2;
	}

	void weapon_handleState2(MessageType msg)
	{
		task_begin;
		task_makeActive(s_playerWeaponTask);
		task_yield(TASK_NO_DELAY);

		if (msg == MSG_STOP_FIRING)
		{
			weapon_stopFiring();
		}
		else if (msg == MSG_START_FIRING)
		{
			weapon_setShooting(s_msgArg1);
		}
		else if (msg == MSG_SWITCH_WPN)
		{
			s_switchWeapons = JTRUE;
			s_nextWeapon = s_msgArg1;
		}
		task_end;
	}
		
	// This task function handles animating a weapon on or off screen, based on the inputs.
	void weapon_animateOnOrOffscreen(MessageType msg)
	{
		struct LocalContext
		{
			Tick startTick;
		};
		task_begin_ctx;

		s_curPlayerWeapon->xOffset = s_weaponAnimState.startOffsetX;
		s_curPlayerWeapon->yOffset = s_weaponAnimState.startOffsetY;
		task_makeActive(s_playerWeaponTask);

		task_yield(TASK_NO_DELAY);
		task_callTaskFunc(weapon_handleState);

		s_curPlayerWeapon->frame = s_weaponAnimState.frame;
		while (s_weaponAnimState.frameCount)
		{
			taskCtx->startTick = s_curTick;
			// We may get calls to the weapon task while animating, so handle them as we get them.
			do
			{
				task_yield((msg != MSG_RUN_TASK) ? TASK_NO_DELAY : s_weaponAnimState.ticksPerFrame);

				if (msg == MSG_STOP_FIRING)
				{
					weapon_stopFiring();
				}
				else if (msg == MSG_START_FIRING)
				{
					weapon_setShooting(s_msgArg1/*secondaryFire*/);
				}
				else if (msg == MSG_SWITCH_WPN)
				{
					s_switchWeapons = JTRUE;
					s_nextWeapon = s_msgArg1;
				}
			} while (msg != MSG_RUN_TASK);

			// Calculate the number of elapsed frames.
			Tick elapsed = s_curTick - taskCtx->startTick;
			if (elapsed <= s_weaponAnimState.ticksPerFrame + 1 || s_weaponAnimState.frameCount < 2)
			{
				s_curPlayerWeapon->xOffset += s_weaponAnimState.xSpeed;
				s_curPlayerWeapon->yOffset += s_weaponAnimState.ySpeed;
				s_weaponAnimState.frameCount--;
			}
			else
			{
				s32 elapsedFrames = min((s32)s_weaponAnimState.frameCount, (s32)(elapsed / s_weaponAnimState.ticksPerFrame));
				s_curPlayerWeapon->xOffset += s_weaponAnimState.xSpeed * elapsedFrames;
				s_curPlayerWeapon->yOffset += s_weaponAnimState.ySpeed * elapsedFrames;
				s_weaponAnimState.frameCount -= elapsedFrames;
			}
		}

		task_end;
	}

	void weapon_handleOffAnimation(MessageType msg)
	{
		task_begin;
		// s_prevWeapon is the weapon we are switching away from.
		if (s_prevWeapon == WPN_REPEATER)
		{
			if (s_repeaterFireSndID)
			{
				sound_stop(s_repeaterFireSndID);
				s_repeaterFireSndID = 0;
			}
			s_curPlayerWeapon->frame = 0;
		}
		else if (s_prevWeapon == WPN_CANNON && !s_secondaryFire)
		{
			s_curPlayerWeapon->frame = 0;
		}

		s_weaponAnimState =
		{
			s_curPlayerWeapon->frame,	// frame
			s_curPlayerWeapon->xOffset, s_curPlayerWeapon->yOffset,
			3, 4,						// xSpeed, ySpeed
			10, s_superCharge ? 1u : 2u	// frameCount, ticksPerFrame
		};
		task_callTaskFunc(weapon_animateOnOrOffscreen);
		s_weaponOffAnim = JTRUE;

		task_end;
	}

	void weapon_handleOnAnimation(MessageType msg)
	{
		task_begin;
		s_weaponOffAnim = JFALSE;
		if (s_curWeapon != WPN_FIST && s_curWeapon != WPN_THERMAL_DET && s_curWeapon != WPN_MINE)
		{
			sound_play(s_weaponChangeSnd);
		}
				
		if (s_prevWeapon == WPN_THERMAL_DET)
		{
			// TODO(Core Game Loop Release)
		}
		else if (s_prevWeapon == WPN_MINE)
		{
			// TODO(Core Game Loop Release)
		}

		// s_prevWeapon is the weapon we are switching away from.
		s_weaponAnimState =
		{
			0,							// frame
			30, 40,						// startOffsetX, startOffsetY: was 14, 40
			-3, -4,						// xSpeed, ySpeed
			10, s_superCharge ? 1u : 2u	// frameCount, ticksPerFrame
		};
		task_callTaskFunc(weapon_animateOnOrOffscreen);
		task_end;
	}

	void weapon_holster()
	{
		if (s_playerWeaponTask)
		{
			task_runAndReturn(s_playerWeaponTask, MSG_HOLSTER);
		}
	}

	// Set weapons back to their idle frame; stop looping sound
	void weapon_setIdle()
	{
		PlayerWeapon* weapon = s_curPlayerWeapon;
		if (s_prevWeapon == WPN_REPEATER)
		{
			if (s_repeaterFireSndID)
			{
				sound_stop(s_repeaterFireSndID);
				s_repeaterFireSndID = 0;
			}
			weapon->frame = 0;
		}
		else if (s_prevWeapon == WPN_CANNON)
		{
			if (!s_secondaryFire)
			{
				weapon->frame = 0;
			}
		}
	}

	void weapon_playerWeaponTaskFunc(MessageType msg)
	{
		struct LocalContext
		{
			JBool secondaryFire;
		};
		task_begin_ctx;
		while (1)
		{
			// If the weapon task is called with a non-zero id, handle it here.
			if (msg == MSG_FREE_TASK)
			{
				task_free(s_playerWeaponTask);
				s_playerWeaponTask = nullptr;
				return;
			}
			else if (msg == MSG_SWITCH_WPN || s_queWeaponSwitch)
			{
				s_queWeaponSwitch = JFALSE;

				task_makeActive(s_playerWeaponTask);
				s_nextWeapon = s_msgArg1;
				task_makeActive(s_playerWeaponTask);
				task_yield(TASK_NO_DELAY);
				task_callTaskFunc(weapon_handleState);

				if (s_nextWeapon == -1)
				{
					swap(s_curWeapon, s_lastWeapon);
				}
				else
				{
					s32 lastWeapon = s_curWeapon;
					s_curWeapon = s_nextWeapon;
					s_lastWeapon = lastWeapon;
				}
				s_playerInfo.curWeapon = s_prevWeapon;

				if (!s_weaponOffAnim)
				{
					task_callTaskFunc(weapon_handleOffAnimation);
				}
				weapon_setNext(s_curWeapon);
				task_callTaskFunc(weapon_handleOnAnimation);
			}
			else if (msg == MSG_START_FIRING)
			{
				taskCtx->secondaryFire = s_msgArg1;
				task_makeActive(s_playerWeaponTask);
				task_makeActive(s_playerWeaponTask);
				task_yield(TASK_NO_DELAY);

				task_callTaskFunc(weapon_handleState);
				s_isShooting = JTRUE;
				s_secondaryFire = taskCtx->secondaryFire ? JTRUE : JFALSE;
				if (s_weaponOffAnim)
				{
					task_callTaskFunc(weapon_handleOnAnimation);
				}
				s_curPlayerWeapon->flags &= ~2;

				weapon_setIdle();
				weapon_setFireRate();
			}
			else if (msg == MSG_STOP_FIRING)
			{
				s_isShooting = JFALSE;
				weapon_setIdle();
				s_curPlayerWeapon->flags |= 2;
			}
			else if (msg == MSG_HOLSTER)
			{
				task_makeActive(s_playerWeaponTask);
				task_yield(TASK_NO_DELAY);
				task_callTaskFunc(weapon_handleState);

				if (!s_weaponOffAnim)
				{
					// Remove weapon from screen
					weapon_setIdle();
					
					s_weaponAnimState =
					{
						s_curPlayerWeapon->frame,   // frame
						 0,  0,                     // startOffsetX, startOffsetY
						 3,  4,                     // xSpeed, ySpeed
						10, s_superCharge ? 1u : 2u // frameCount, ticksPerFrame
					};
					task_callTaskFunc(weapon_animateOnOrOffscreen);
					s_weaponOffAnim = JTRUE;
				}
				else
				{
					// Return weapon to screen
					s_weaponOffAnim = JFALSE;
					if (s_curWeapon == WPN_PISTOL || s_curWeapon == WPN_RIFLE || s_curWeapon == WPN_REPEATER || s_curWeapon == WPN_FUSION || s_curWeapon == WPN_MORTAR ||
						s_curWeapon == WPN_CONCUSSION || s_curWeapon == WPN_CANNON)
					{
						sound_play(s_weaponChangeSnd);
					}

					weapon_emptyAnim();

					s_weaponAnimState =
					{
						s_curPlayerWeapon->frame,   // frame
						30, 40,                     // startOffsetX, startOffsetY
						-3, -4,                     // xSpeed, ySpeed
						10, s_superCharge ? 1u : 2u // frameCount, ticksPerFrame
					};
					task_callTaskFunc(weapon_animateOnOrOffscreen);
				}
			}

			// Handle shooting.
			while (s_isShooting)
			{
				s_switchWeapons = JFALSE;
				s_queWeaponSwitch = JFALSE;
				s_fireFrame++;

				while (msg != MSG_RUN_TASK)
				{
					task_makeActive(s_playerWeaponTask);
					task_yield(TASK_NO_DELAY);
					task_callTaskFunc(weapon_handleState);
				}

				if ((s_secondaryFire && s_canFireWeaponSec) || s_canFireWeaponPrim)
				{
					// Fire the weapon.
					task_callTaskFunc(s_weaponFireFunc[s_prevWeapon]);
				}
				else
				{
					task_makeActive(s_playerWeaponTask);
					task_yield(TASK_NO_DELAY);
					task_callTaskFunc(weapon_handleState);
				}

				if (s_switchWeapons)
				{
					if (s_nextWeapon == -1)
					{
						swap(s_curWeapon, s_lastWeapon);
					}
					else
					{
						s_lastWeapon = s_curWeapon;
						s_curWeapon = s_nextWeapon;
					}
					s_playerInfo.curWeapon = s_prevWeapon;

					// Move the next weapon off screen, if it is not already off.
					// Then move the next one on screen.
					if (!s_weaponOffAnim)
					{
						task_callTaskFunc(weapon_handleOffAnimation);
					}
					weapon_setNext(s_curWeapon);
					task_callTaskFunc(weapon_handleOnAnimation);
				}
			}

			// Go to sleep until needed.
			task_yield(TASK_SLEEP);
		}
		task_end;
	}

	// Voxel HUD weapon: rendered as an overlay (no depth test) so it never clips into world geometry.
	static SecObject* s_voxelHudObj = nullptr;
	static RSector* s_voxelHudSector = nullptr;

	void weapon_preRender()
	{
		loadWeaponVoxels();
		dumpWeaponSprites();

		// If we just loaded weapon voxels, the GPU renderer needs to rebuild model data.
		if (s_weaponVoxelGpuDirty && TFE_Jedi::getSubRenderer() == TSR_CLASSIC_GPU)
		{
			TFE_Jedi::model_loadGpuModels();
			s_weaponVoxelGpuDirty = false;
		}

		JediModel* voxModel = s_weaponVoxelModels[s_curWeapon];
		{
			static bool s_loggedOnce = false;
			if (voxModel && !s_loggedOnce)
			{
				s_loggedOnce = true;
				FILE* f = fopen("tfe_wpnvox_diag.log", "w");
				if (f)
				{
					fprintf(f, "weapon=%d model=%p\n", s_curWeapon, voxModel);
					fprintf(f, "  polyCount=%d vertexCount=%d flags=0x%x\n", voxModel->polygonCount, voxModel->vertexCount, voxModel->flags);
					fprintf(f, "  drawId=%p radius=%d\n", voxModel->drawId, voxModel->radius);
					fprintf(f, "  textureCount=%d\n", voxModel->textureCount);
					if (voxModel->polygonCount > 0)
					{
						JmPolygon* p = &voxModel->polygons[0];
						fprintf(f, "  poly[0]: shading=%d color=%d vertexCount=%d texture=%p\n", p->shading, p->color, p->vertexCount, p->texture);
						fprintf(f, "  poly[0] indices: %d %d %d %d\n", p->indices[0], p->indices[1], p->indices[2], p->indices[3]);
					}
					// Sample a few vertex positions
					for (s32 i = 0; i < std::min(6, voxModel->vertexCount); i++)
					{
						fprintf(f, "  vtx[%d]: %d %d %d\n", i, voxModel->vertices[i].x, voxModel->vertices[i].y, voxModel->vertices[i].z);
					}
					fclose(f);
				}
			}
		}
		if (!voxModel || !s_curPlayerWeapon || s_weaponOffAnim || s_externalCameraMode || !s_playerObject)
		{
			if (s_voxelHudObj && s_voxelHudSector)
			{
				TFE_Jedi::sector_removeObject(s_voxelHudObj);
				s_voxelHudSector = nullptr;
			}
			return;
		}

		SecObject* player = s_playerObject;

		voxDbgRegisterCVars();

		// Position the weapon using yaw only (no pitch) so it doesn't swing when looking up/down.
		fixed16_16 mtx[9];
		weapon_computeMatrix(mtx, 0, -s_eyeYaw);

		// Apply a subtle forward offset when looking down, controlled by CVar.
		const fixed16_16 pitchFwd = mul16(s_eyePitch, floatToFixed16(TFE_VoxDbg::wpnFwdPitchScale));

		// Local-space offset from CVars: (right, down, forward).
		vec3_fixed localOffset = {
			floatToFixed16(TFE_VoxDbg::wpnOffX),
			floatToFixed16(TFE_VoxDbg::wpnOffY),
			floatToFixed16(TFE_VoxDbg::wpnOffZ) + pitchFwd
		};
		vec3_fixed worldOffset;
		TFE_Jedi::rotateVectorM3x3(&localOffset, &worldOffset, mtx);

		fixed16_16 posX = s_eyePos.x + worldOffset.x;
		fixed16_16 posY = s_eyePos.y + worldOffset.y;
		fixed16_16 posZ = s_eyePos.z + worldOffset.z;

		if (TFE_Jedi::getSubRenderer() == TSR_CLASSIC_GPU)
		{
			// GPU path: add directly to the overlay draw list (renders on top of world).
			if (!s_voxelHudObj)
			{
				s_voxelHudObj = TFE_Jedi::allocateObject();
				if (!s_voxelHudObj) { return; }
				s_voxelHudObj->type = OBJ_TYPE_3D;
				s_voxelHudObj->flags = OBJ_FLAG_NEEDS_TRANSFORM;
				s_voxelHudObj->worldWidth = FIXED(1);
				s_voxelHudObj->worldHeight = FIXED(1);
				s_voxelHudObj->entityFlags = 0;
			}
			s_voxelHudObj->model = voxModel;
			s_voxelHudObj->posWS = { posX, posY, posZ };
			// Yaw offset from CVar (converted from degrees to 14-bit angle: degrees * 16384/360).
			s_voxelHudObj->yaw = s_eyeYaw + (angle14_32)(TFE_VoxDbg::wpnYawOff * 16384.0f / 360.0f);
			// Pitch from CVar: scale factor applied to eye pitch.
			s_voxelHudObj->pitch = (angle14_32)(s_eyePitch * TFE_VoxDbg::wpnPitchScale);
			s_voxelHudObj->roll = (angle14_32)(s_eyePitch * TFE_VoxDbg::wpnRollScale);
			TFE_Jedi::obj3d_computeTransform(s_voxelHudObj);

			Vec3f posWS = { fixed16ToFloat(posX), fixed16ToFloat(posY), fixed16ToFloat(posZ) };
			TFE_Jedi::model_addOverlay(s_voxelHudObj, voxModel, posWS, s_voxelHudObj->transform, 31.0f);
		}
		else
		{
			// Software renderer: voxel HUD weapon not supported (no overlay pass).
			// Fall back to the 2D sprite in weapon_draw().
		}
	}

	void weapon_postRender()
	{
		if (s_voxelHudObj && s_voxelHudSector)
		{
			TFE_Jedi::sector_removeObject(s_voxelHudObj);
			s_voxelHudSector = nullptr;
		}
	}

	// In DOS, this was part of drawWorld() - 
	// for TFE I split it out to limit the amount of game code in the renderer.
	static FILE* s_weaponDbgLog = nullptr;
	static int s_weaponDbgCount = 0;

	void weapon_draw(u8* display, DrawRect* rect)
	{
		// TFE - don't draw weapon in external camera mode
		if (s_externalCameraMode)
		{
			return;
		}

		if (!s_weaponDbgLog)
		{
			s_weaponDbgLog = fopen("E:/Github/TheForceEngine/x64/Release/tfe_weapon_debug.log", "w");
		}
		if (s_weaponDbgLog && s_weaponDbgCount < 120)
		{
			s_weaponDbgCount++;
			PlayerWeapon* w = s_curPlayerWeapon;
			fprintf(s_weaponDbgLog, "frame %d: weapon=%p offAnim=%d extCam=%d", s_weaponDbgCount, (void*)w, (int)s_weaponOffAnim, (int)s_externalCameraMode);
			if (w) { fprintf(s_weaponDbgLog, " wframe=%d frameCount=%d tex=%p flags=%u", w->frame, w->frameCount, (void*)w->frames[w->frame], w->flags); }
			fprintf(s_weaponDbgLog, "\n");
			fflush(s_weaponDbgLog);
		}

		const fixed16_16 weaponLightingZDist  = FIXED(6);
		const fixed16_16 gasmaskLightingZDist = FIXED(2);

		PlayerWeapon* weapon = s_curPlayerWeapon;
		if (weapon && !s_weaponOffAnim)
		{
			// TFE: Skip the 2D sprite when the voxel overlay is active (GPU renderer only).
			JediModel* voxModel = s_weaponVoxelModels[s_curWeapon];
			TFE_SubRenderer subRenderer = TFE_Jedi::getSubRenderer();
			if (voxModel && subRenderer == TSR_CLASSIC_GPU)
			{
				// Voxel weapon rendered via model_addOverlay in weapon_preRender.
			}
			else
			{
			s32 x = weapon->xPos[weapon->frame];
			s32 y = weapon->yPos[weapon->frame];
			if (weapon->flags & 1)
			{
				x += weapon->rollOffset;
				y += weapon->pchOffset;
			}
			if (weapon->flags & 2)
			{
				x += weapon->xWaveOffset;
				y += weapon->yWaveOffset;
			}
			if (weapon->flags & 4)
			{
				x += weapon->xOffset;
				y += weapon->yOffset;
			}

			const u8* atten = RClassic_Fixed::computeLighting(weaponLightingZDist, 0);
			TextureData* tex = weapon->frames[weapon->frame];
			if (weapon->ammo && *weapon->ammo == 0 && (weapon->ammo == &s_playerInfo.ammoDetonator || weapon->ammo == &s_playerInfo.ammoMine))
			{
				tex = s_playerWeaponList[WPN_FIST].frames[0];
			}

			u32 dispWidth, dispHeight;
			vfb_getResolution(&dispWidth, &dispHeight);

			if (dispWidth == 320 && dispHeight == 200 && subRenderer != TSR_CLASSIC_GPU)
			{
				if (atten && !s_weaponLight)
				{
					blitTextureToScreenLit(tex, rect, x, y, atten, display, JTRUE);
				}
				else
				{
					blitTextureToScreen(tex, rect, x, y, display, JTRUE);
				}
			}
			else
			{
				// HUD scaling.
				fixed16_16 xScale = vfb_getXScale();
				fixed16_16 yScale = vfb_getYScale();
				x = floor16(mul16(xScale, intToFixed16(x))) + vfb_getWidescreenOffset();
				y = floor16(mul16(yScale, intToFixed16(y)));

				// Hack to handle the Dark Trooper rifle.
				if (weapon->frames[0]->width == 169)
				{
					x += vfb_getWidescreenOffset();
				}

				// TFE: Handle extended pitch limits.
				s32 texHeight = floor16(mul16(intToFixed16(tex->height), yScale));
				s32 y1 = y + texHeight;
				if (y1 < s32(dispHeight) - 3)
				{
					y = dispHeight + 2 - texHeight;
				}

				if (atten && !s_weaponLight)
				{
					blitTextureToScreenLitScaled(tex, rect, x, y, xScale, yScale, atten, display, JTRUE);
				}
				else
				{
					blitTextureToScreenScaled(tex, rect, x, y, xScale, yScale, display, JTRUE);
				}
			}
			} // end else (no voxel model)
		}

		if (s_wearingGasmask)
		{
			s32 x = s_gasMaskXpos;
			s32 y = s_gasMaskYpos;
			if (weapon)
			{
				x -= (weapon->xWaveOffset >> 3);
				y += (weapon->yWaveOffset >> 2);
			}
			const u8* atten = RClassic_Fixed::computeLighting(gasmaskLightingZDist, 0);
			TextureData* tex = s_gasmaskTexture;

			u32 dispWidth, dispHeight;
			vfb_getResolution(&dispWidth, &dispHeight);

			if (dispWidth == 320 && dispHeight == 200)
			{
				if (atten)
				{
					blitTextureToScreenLit(tex, rect, x, y, atten, display, JTRUE);
				}
				else
				{
					blitTextureToScreen(tex, rect, x, y, display, JTRUE);
				}
			}
			else
			{
				// HUD scaling.
				fixed16_16 xScale = vfb_getXScale();
				fixed16_16 yScale = vfb_getYScale();
				x = floor16(mul16(xScale, intToFixed16(x))) + vfb_getWidescreenOffset();
				y = floor16(yScale + mul16(yScale, intToFixed16(y)));

				if (atten && !s_weaponLight)
				{
					blitTextureToScreenLitScaled(tex, rect, x, y, xScale, yScale, atten, display, JTRUE);
				}
				else
				{
					blitTextureToScreenScaled(tex, rect, x, y, xScale, yScale, display, JTRUE);
				}
			}
		}
	}
}  // namespace TFE_DarkForces
