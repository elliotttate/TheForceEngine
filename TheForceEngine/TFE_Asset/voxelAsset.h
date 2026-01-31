#pragma once
//////////////////////////////////////////////////////////////////////
// MagicaVoxel (.vox) asset support for Dark Forces voxel replacements.
//////////////////////////////////////////////////////////////////////
#include <TFE_System/types.h>

struct JediModel;

namespace TFE_Voxel
{
	void addVoxelRootPath(const char* path);
	void clearLevelData();

	// Base name lookup (no extension). Example: "OFFCFIN".
	JediModel* getModelForName(const char* baseName, AssetPool pool = POOL_LEVEL);
	// Asset name lookup (may include extension/path). Example: "OFFCFIN.WAX".
	JediModel* getModelForAssetName(const char* assetName, AssetPool pool = POOL_LEVEL);

	bool isVoxelModel(const JediModel* model);
}
