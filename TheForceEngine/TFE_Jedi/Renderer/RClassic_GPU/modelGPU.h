#pragma once
//////////////////////////////////////////////////////////////////////
// Manage and render 3DO models.
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>
#include <TFE_System/memoryPool.h>
#include <TFE_Jedi/Math/fixedPoint.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_Asset/spriteAsset_Jedi.h>
#include "sbuffer.h"

namespace TFE_Jedi
{
	bool model_init();
	void model_destroy();
	// This needs to be called *after* texture packing is complete so that textureIds are already set.
	void model_loadGpuModels();

	void model_drawListClear();
	void model_drawListFinish();

	void model_add(void* obj, JediModel* model, Vec3f posWS, fixed16_16* transform, f32 ambient, Vec2f floorOffset, Vec2f ceilOffset, u32 portalInfo);
	void model_addOverlay(void* obj, JediModel* model, Vec3f posWS, fixed16_16* transform, f32 ambient);
	void model_drawList();
	void model_drawOverlayList();

}  // TFE_Jedi

// Voxel debug/tuning variables shared across weapon.cpp, modelGPU.cpp, frontEndUi.cpp.
namespace TFE_VoxDbg
{
	extern bool noCull;
	extern bool noPortalClip;
	extern f32  wpnOffX;
	extern f32  wpnOffY;
	extern f32  wpnOffZ;
	extern f32  wpnYawOff;
	extern f32  wpnPitchScale;
	extern f32  wpnFwdPitchScale;
	extern f32  wpnRollScale;
}
