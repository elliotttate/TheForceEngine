#include <cmath>
#include <climits>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "voxelAsset.h"
#include "modelAsset_jedi.h"
#include <TFE_DarkForces/mission.h>
#include <TFE_FileSystem/fileutil.h>
#include <TFE_FileSystem/filestream.h>
#include <TFE_Jedi/Level/rtexture.h>
#include <TFE_Jedi/Level/robjData.h>
#include <TFE_Jedi/Math/core_math.h>
#include <TFE_Game/igame.h>
#include <TFE_System/system.h>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TFE_Voxel
{
	// Maximum recursion depth for directory scanning to prevent stack overflow.
	static const s32 MAX_SCAN_DEPTH = 16;

	struct VoxVoxel
	{
		u8 x;
		u8 y;
		u8 z;
		u8 color;
	};

	struct VoxData
	{
		s32 sizeX = 0;
		s32 sizeY = 0;
		s32 sizeZ = 0;
		std::vector<u8> palette; // 256 * 4 (RGBA)
		std::vector<VoxVoxel> voxels;
	};

	static std::vector<std::string> s_voxelRoots;
	static std::unordered_map<std::string, std::string> s_voxelFiles;
	static std::unordered_map<std::string, f32> s_voxelScales;  // per-model scale from .cfg sidecar files
	static std::unordered_map<std::string, JediModel*> s_voxelModels[POOL_COUNT];
	static std::unordered_set<const JediModel*> s_voxelModelSet;
	static bool s_voxelFilesDirty = true;

	static u32 readU32(const u8*& ptr, const u8* end)
	{
		if (ptr + 4 > end) { return 0; }
		const u32 v = (u32)ptr[0] | ((u32)ptr[1] << 8u) | ((u32)ptr[2] << 16u) | ((u32)ptr[3] << 24u);
		ptr += 4;
		return v;
	}

	static void toUpperInPlace(std::string& value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)toupper(c); });
	}

	static std::string normalizeBaseName(const char* name)
	{
		if (!name || !name[0]) { return std::string(); }

		char base[TFE_MAX_PATH] = {};
		FileUtil::getFileNameFromPath(name, base, false);
		if (!base[0])
		{
			strncpy(base, name, TFE_MAX_PATH - 1);
			base[TFE_MAX_PATH - 1] = 0;
			FileUtil::stripExtension(base, base);
		}
		std::string key = base;
		toUpperInPlace(key);
		return key;
	}

	static void ensureTrailingSlash(std::string& path)
	{
		if (path.empty()) { return; }
		char last = path.back();
		if (last != '/' && last != '\\')
		{
			path.push_back('/');
		}
	}

	static void toLowerInPlace(std::string& value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)tolower(c); });
	}

	static bool endsWithVoxelsDir(const std::string& path)
	{
		if (path.empty()) { return false; }
		std::string tmp = path;
		toLowerInPlace(tmp);
		// Trim trailing slashes.
		while (!tmp.empty() && (tmp.back() == '/' || tmp.back() == '\\'))
		{
			tmp.pop_back();
		}
		if (tmp.size() < 6) { return false; }
		const size_t pos = tmp.find_last_of("/\\");
		const std::string leaf = (pos == std::string::npos) ? tmp : tmp.substr(pos + 1);
		return leaf == "voxels";
	}

	static bool hasVoxFiles(const std::string& dir)
	{
		FileList files;
		FileUtil::readDirectory(dir.c_str(), "vox", files);
		return !files.empty();
	}

	static void parseCfgFile(const std::string& path, const std::string& key)
	{
		FILE* f = fopen(path.c_str(), "r");
		if (!f) { return; }
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			f32 val = 0.0f;
			if (sscanf(line, " scale %f", &val) == 1 && val > 0.0f)
			{
				s_voxelScales[key] = val;
			}
		}
		fclose(f);
	}

	static void scanVoxelDirectory(const std::string& dir, s32 depth = 0)
	{
		if (depth >= MAX_SCAN_DEPTH)
		{
			TFE_System::logWrite(LOG_WARNING, "Voxel", "Max directory scan depth reached at '%s'.", dir.c_str());
			return;
		}

		FileList files;
		FileUtil::readDirectory(dir.c_str(), "vox", files);
		for (const std::string& file : files)
		{
			char name[TFE_MAX_PATH] = {};
			FileUtil::stripExtension(file.c_str(), name);
			std::string key = name;
			toUpperInPlace(key);

			if (s_voxelFiles.find(key) == s_voxelFiles.end())
			{
				s_voxelFiles[key] = dir + file;
			}
		}

		// Scan for .cfg sidecar files (e.g., STORMFIN.cfg alongside STORMFIN.vox).
		FileList cfgFiles;
		FileUtil::readDirectory(dir.c_str(), "cfg", cfgFiles);
		for (const std::string& file : cfgFiles)
		{
			char name[TFE_MAX_PATH] = {};
			FileUtil::stripExtension(file.c_str(), name);
			std::string key = name;
			toUpperInPlace(key);
			parseCfgFile(dir + file, key);
		}

		FileList subdirs;
		FileUtil::readSubdirectories(dir.c_str(), subdirs);
		for (const std::string& subdir : subdirs)
		{
			scanVoxelDirectory(subdir, depth + 1);
		}
	}

	static void scanVoxelFiles()
	{
		if (!s_voxelFilesDirty)
		{
			return;
		}

		s_voxelFiles.clear();
		s_voxelScales.clear();
		for (const std::string& root : s_voxelRoots)
		{
			std::string base = root;
			ensureTrailingSlash(base);

			bool scanned = false;
			if (FileUtil::directoryExits(base.c_str()))
			{
				// If the root is already the Voxels directory (or contains .vox files), scan it directly.
				if (endsWithVoxelsDir(base) || hasVoxFiles(base))
				{
					scanVoxelDirectory(base);
					scanned = true;
				}
			}

			if (!scanned)
			{
				std::string voxRoot = base + "Voxels/";
				if (FileUtil::directoryExits(voxRoot.c_str()))
				{
					scanVoxelDirectory(voxRoot);
				}
			}
		}

		s_voxelFilesDirty = false;
	}

	// Helper to read a VOX dictionary string.
	static std::string readVoxString(const u8*& p, const u8* end)
	{
		if (p + 4 > end) { p = end; return {}; }
		u32 len = readU32(p, end);
		if (p + len > end) { p = end; return {}; }
		std::string s((const char*)p, len);
		p += len;
		return s;
	}

	// Helper to read a VOX dictionary (DICT).
	static std::unordered_map<std::string, std::string> readVoxDict(const u8*& p, const u8* end)
	{
		std::unordered_map<std::string, std::string> dict;
		if (p + 4 > end) return dict;
		u32 count = readU32(p, end);
		for (u32 i = 0; i < count && p < end; i++)
		{
			std::string key = readVoxString(p, end);
			std::string val = readVoxString(p, end);
			dict[key] = val;
		}
		return dict;
	}

	struct VoxModelData
	{
		std::vector<VoxVoxel> voxels;
	};

	struct VoxTransformNode
	{
		s32 childId = -1;
		s32 tx = 0, ty = 0, tz = 0;
	};

	struct VoxShapeNode
	{
		s32 modelId = -1;
	};

	struct VoxGroupNode
	{
		std::vector<s32> children;
	};

	// Recursively accumulate translation through the scene graph.
	static void resolveTransforms(
		s32 nodeId, s32 accTx, s32 accTy, s32 accTz,
		const std::unordered_map<s32, VoxTransformNode>& transforms,
		const std::unordered_map<s32, VoxShapeNode>& shapes,
		const std::unordered_map<s32, VoxGroupNode>& groups,
		std::unordered_map<s32, std::array<s32, 3>>& modelTranslations)
	{
		auto tIt = transforms.find(nodeId);
		if (tIt != transforms.end())
		{
			const auto& t = tIt->second;
			s32 tx = accTx + t.tx;
			s32 ty = accTy + t.ty;
			s32 tz = accTz + t.tz;
			resolveTransforms(t.childId, tx, ty, tz, transforms, shapes, groups, modelTranslations);
			return;
		}
		auto gIt = groups.find(nodeId);
		if (gIt != groups.end())
		{
			for (s32 child : gIt->second.children)
			{
				resolveTransforms(child, accTx, accTy, accTz, transforms, shapes, groups, modelTranslations);
			}
			return;
		}
		auto sIt = shapes.find(nodeId);
		if (sIt != shapes.end())
		{
			modelTranslations[sIt->second.modelId] = { accTx, accTy, accTz };
		}
	}

	static bool loadVoxFile(const char* path, VoxData& outData)
	{
		FileStream file;
		if (!file.open(path, Stream::MODE_READ))
		{
			return false;
		}

		const size_t fileSize = file.getSize();
		std::vector<u8> buffer(fileSize);
		file.readBuffer(buffer.data(), (u32)fileSize);
		file.close();

		const u8* ptr = buffer.data();
		const u8* end = buffer.data() + buffer.size();
		if (end - ptr < 8 || memcmp(ptr, "VOX ", 4) != 0)
		{
			return false;
		}
		ptr += 4;
		const u32 version = readU32(ptr, end);
		if (version < 150)
		{
			TFE_System::logWrite(LOG_WARNING, "Voxel", "Unsupported VOX version %u in '%s'.", version, path);
		}

		if (end - ptr < 12)
		{
			return false;
		}
		char chunkId[5] = {};
		memcpy(chunkId, ptr, 4);
		ptr += 4;
		const u32 contentSize = readU32(ptr, end);
		const u32 childrenSize = readU32(ptr, end);
		if (memcmp(chunkId, "MAIN", 4) != 0)
		{
			return false;
		}
		ptr += contentSize;
		const u8* childEnd = ptr + childrenSize;

		outData.palette.clear();
		outData.palette.resize(256 * 4, 0);
		bool hasPalette = false;

		// Collect per-model voxel data and scene graph nodes.
		std::vector<VoxModelData> models;
		std::unordered_map<s32, VoxTransformNode> transformNodes;
		std::unordered_map<s32, VoxShapeNode> shapeNodes;
		std::unordered_map<s32, VoxGroupNode> groupNodes;

		while (ptr + 12 <= childEnd)
		{
			memcpy(chunkId, ptr, 4);
			ptr += 4;
			const u32 chunkContentSize = readU32(ptr, end);
			const u32 chunkChildrenSize = readU32(ptr, end);
			const u8* chunkContent = ptr;
			const u8* chunkEnd = ptr + chunkContentSize;

			if (memcmp(chunkId, "SIZE", 4) == 0)
			{
				// SIZE always precedes XYZI; we just note a new model is coming.
			}
			else if (memcmp(chunkId, "XYZI", 4) == 0 && chunkContent + 4 <= chunkEnd)
			{
				const u32 count = readU32(chunkContent, chunkEnd);
				VoxModelData md;
				md.voxels.reserve(count);
				for (u32 i = 0; i < count && (chunkContent + 4) <= chunkEnd; i++)
				{
					VoxVoxel voxel = {};
					voxel.x = *chunkContent++;
					voxel.y = *chunkContent++;
					voxel.z = *chunkContent++;
					voxel.color = *chunkContent++;
					md.voxels.push_back(voxel);
				}
				models.push_back(std::move(md));
			}
			else if (memcmp(chunkId, "RGBA", 4) == 0 && chunkContent + 256 * 4 <= chunkEnd)
			{
				memcpy(outData.palette.data(), chunkContent, 256 * 4);
				hasPalette = true;
			}
			else if (memcmp(chunkId, "nTRN", 4) == 0 && chunkContent + 4 <= chunkEnd)
			{
				s32 nodeId = (s32)readU32(chunkContent, chunkEnd);
				auto attrs = readVoxDict(chunkContent, chunkEnd);
				s32 childId = (chunkContent + 4 <= chunkEnd) ? (s32)readU32(chunkContent, chunkEnd) : -1;
				// reserved
				if (chunkContent + 4 <= chunkEnd) readU32(chunkContent, chunkEnd);
				// layer id
				if (chunkContent + 4 <= chunkEnd) readU32(chunkContent, chunkEnd);
				// num frames
				s32 numFrames = (chunkContent + 4 <= chunkEnd) ? (s32)readU32(chunkContent, chunkEnd) : 0;
				VoxTransformNode tn;
				tn.childId = childId;
				for (s32 f = 0; f < numFrames && chunkContent < chunkEnd; f++)
				{
					auto frameDict = readVoxDict(chunkContent, chunkEnd);
					if (f == 0)
					{
						auto tIt = frameDict.find("_t");
						if (tIt != frameDict.end())
						{
							sscanf(tIt->second.c_str(), "%d %d %d", &tn.tx, &tn.ty, &tn.tz);
						}
					}
				}
				transformNodes[nodeId] = tn;
			}
			else if (memcmp(chunkId, "nSHP", 4) == 0 && chunkContent + 4 <= chunkEnd)
			{
				s32 nodeId = (s32)readU32(chunkContent, chunkEnd);
				auto attrs = readVoxDict(chunkContent, chunkEnd);
				s32 numModels = (chunkContent + 4 <= chunkEnd) ? (s32)readU32(chunkContent, chunkEnd) : 0;
				if (numModels > 0 && chunkContent + 4 <= chunkEnd)
				{
					VoxShapeNode sn;
					sn.modelId = (s32)readU32(chunkContent, chunkEnd);
					shapeNodes[nodeId] = sn;
				}
			}
			else if (memcmp(chunkId, "nGRP", 4) == 0 && chunkContent + 4 <= chunkEnd)
			{
				s32 nodeId = (s32)readU32(chunkContent, chunkEnd);
				auto attrs = readVoxDict(chunkContent, chunkEnd);
				s32 numChildren = (chunkContent + 4 <= chunkEnd) ? (s32)readU32(chunkContent, chunkEnd) : 0;
				VoxGroupNode gn;
				for (s32 c = 0; c < numChildren && chunkContent + 4 <= chunkEnd; c++)
				{
					gn.children.push_back((s32)readU32(chunkContent, chunkEnd));
				}
				groupNodes[nodeId] = gn;
			}

			ptr = chunkEnd + chunkChildrenSize;
		}

		if (!hasPalette)
		{
			for (u32 i = 0; i < 256; i++)
			{
				outData.palette[i * 4 + 0] = (u8)i;
				outData.palette[i * 4 + 1] = (u8)i;
				outData.palette[i * 4 + 2] = (u8)i;
				outData.palette[i * 4 + 3] = 255;
			}
		}

		// Resolve per-model translations from the scene graph.
		std::unordered_map<s32, std::array<s32, 3>> modelTranslations;
		if (!transformNodes.empty())
		{
			resolveTransforms(0, 0, 0, 0, transformNodes, shapeNodes, groupNodes, modelTranslations);
		}

		// Merge all models with their translations. Use s32 coords first, then shift to u8.
		struct VoxelS32 { s32 x, y, z; u8 color; };
		std::vector<VoxelS32> allVoxels;
		for (s32 mi = 0; mi < (s32)models.size(); mi++)
		{
			s32 tx = 0, ty = 0, tz = 0;
			auto tIt = modelTranslations.find(mi);
			if (tIt != modelTranslations.end())
			{
				tx = tIt->second[0];
				ty = tIt->second[1];
				tz = tIt->second[2];
			}
			for (const VoxVoxel& v : models[mi].voxels)
			{
				allVoxels.push_back({ (s32)v.x + tx, (s32)v.y + ty, (s32)v.z + tz, v.color });
			}
		}

		if (allVoxels.empty())
		{
			return false;
		}

		// Compute bounding box and shift to non-negative.
		s32 minX = allVoxels[0].x, minY = allVoxels[0].y, minZ = allVoxels[0].z;
		s32 maxX = minX, maxY = minY, maxZ = minZ;
		for (const auto& v : allVoxels)
		{
			if (v.x < minX) minX = v.x; if (v.x > maxX) maxX = v.x;
			if (v.y < minY) minY = v.y; if (v.y > maxY) maxY = v.y;
			if (v.z < minZ) minZ = v.z; if (v.z > maxZ) maxZ = v.z;
		}

		outData.sizeX = maxX - minX + 1;
		outData.sizeY = maxY - minY + 1;
		outData.sizeZ = maxZ - minZ + 1;

		outData.voxels.reserve(allVoxels.size());
		for (const auto& v : allVoxels)
		{
			VoxVoxel voxel;
			voxel.x = (u8)(v.x - minX);
			voxel.y = (u8)(v.y - minY);
			voxel.z = (u8)(v.z - minZ);
			voxel.color = v.color;
			outData.voxels.push_back(voxel);
		}

		return outData.sizeX > 0 && outData.sizeY > 0 && outData.sizeZ > 0;
	}

	static u8 findClosestPaletteIndex(u8 r, u8 g, u8 b, const u8* dfPalette)
	{
		s32 bestIdx = 1;
		s32 bestDist = INT_MAX;
		for (s32 i = 1; i < 256; i++)
		{
			const s32 pr = CONV_6bitTo8bit(dfPalette[i * 3 + 0]);
			const s32 pg = CONV_6bitTo8bit(dfPalette[i * 3 + 1]);
			const s32 pb = CONV_6bitTo8bit(dfPalette[i * 3 + 2]);

			const s32 dr = pr - r;
			const s32 dg = pg - g;
			const s32 db = pb - b;
			const s32 dist = dr * dr + dg * dg + db * db;
			if (dist < bestDist)
			{
				bestDist = dist;
				bestIdx = i;
			}
		}
		return (u8)bestIdx;
	}

	static TextureData* createPaletteTexture(const std::string& key, const u8* paletteMap, AssetPool pool)
	{
		u8 image[256];
		for (s32 i = 0; i < 256; i++)
		{
			image[i] = paletteMap[i];
		}

		char texName[TFE_MAX_PATH] = {};
		snprintf(texName, TFE_MAX_PATH, "voxpal_%s", key.c_str());
		return TFE_Jedi::bitmap_createIndexedTexture(texName, 16, 16, image, pool, true);
	}

	// Match the 3DO polygon normal computation exactly.
	// Cross product: (v1-v0) x (v2-v0), result stored as v0 + normalized(cross).
	// Called with (v1, v2, v0) vertex order to match modelAsset_jedi.cpp convention.
	static void computePolygonNormal(const vec3* v0, const vec3* v1, const vec3* v2, vec3* out)
	{
		const fixed16_16 dx10 = v1->x - v0->x;
		const fixed16_16 dy10 = v1->y - v0->y;
		const fixed16_16 dz10 = v1->z - v0->z;
		const fixed16_16 dx20 = v2->x - v0->x;
		const fixed16_16 dy20 = v2->y - v0->y;
		const fixed16_16 dz20 = v2->z - v0->z;

		fixed16_16 cx = mul16(dz10, dy20) - mul16(dy10, dz20);
		fixed16_16 cy = mul16(dx10, dz20) - mul16(dz10, dx20);
		fixed16_16 cz = mul16(dy10, dx20) - mul16(dx10, dy20);

		// Normalize using float to avoid fixed-point overflow.
		const f32 fx = fixed16ToFloat(cx);
		const f32 fy = fixed16ToFloat(cy);
		const f32 fz = fixed16ToFloat(cz);
		const f32 len = sqrtf(fx * fx + fy * fy + fz * fz);
		if (len > 0.0f)
		{
			out->x = v0->x + floatToFixed16(fx / len);
			out->y = v0->y + floatToFixed16(fy / len);
			out->z = v0->z + floatToFixed16(fz / len);
		}
		else
		{
			*out = *v0;
		}
	}

	static f32 getVoxelScale(const std::string& key)
	{
		auto it = s_voxelScales.find(key);
		return (it != s_voxelScales.end()) ? it->second : 1.0f;
	}

	// Pack a vertex into a 64-bit key for deduplication: 3 x fixed16_16 values.
	static u64 vertexKey(const vec3& v)
	{
		// Use the lower 21 bits of each component to fit in 64 bits.
		const u64 kx = (u64)(u32)v.x;
		const u64 ky = (u64)(u32)v.y;
		const u64 kz = (u64)(u32)v.z;
		return (kx) | (ky << 21) | (kz << 42);
	}

	static JediModel* buildVoxelModel(const std::string& key, const VoxData& data, AssetPool pool)
	{
		if (data.sizeX <= 0 || data.sizeY <= 0 || data.sizeZ <= 0 || data.voxels.empty())
		{
			return nullptr;
		}

		const u8* dfPalette = TFE_DarkForces::s_levelPalette;
		u8 paletteMap[256] = {};
		paletteMap[0] = 0;
		for (s32 i = 1; i < 256; i++)
		{
			// Treat all palette entries as opaque (like GZDoom) to avoid holes.
			paletteMap[i] = findClosestPaletteIndex(
				data.palette[i * 4 + 0],
				data.palette[i * 4 + 1],
				data.palette[i * 4 + 2],
				dfPalette);
		}

		// Validate grid dimensions to avoid overflow: sizeX * sizeY * sizeZ must fit in s32.
		const s64 voxelCount64 = (s64)data.sizeX * (s64)data.sizeY * (s64)data.sizeZ;
		if (voxelCount64 > (s64)INT_MAX || voxelCount64 <= 0)
		{
			TFE_System::logWrite(LOG_WARNING, "Voxel", "Voxel grid too large (%dx%dx%d) for '%s'.", data.sizeX, data.sizeY, data.sizeZ, key.c_str());
			return nullptr;
		}
		const s32 voxelCount = (s32)voxelCount64;
		std::vector<u8> grid(voxelCount, 0);
		for (const VoxVoxel& voxel : data.voxels)
		{
			if (voxel.x >= data.sizeX || voxel.y >= data.sizeY || voxel.z >= data.sizeZ)
			{
				continue;
			}
			if (voxel.color == 0)
			{
				continue;
			}
			if (paletteMap[voxel.color] == 0)
			{
				continue;
			}
			const s32 index = voxel.x + voxel.y * data.sizeX + voxel.z * data.sizeX * data.sizeY;
			grid[index] = voxel.color;
		}

		MemoryRegion* memRegion = (pool == POOL_GAME) ? s_gameRegion : s_levelRegion;
		MemoryRegion* prevTexAlloc = TFE_Jedi::bitmap_getAllocator();
		TFE_Jedi::bitmap_setAllocator(memRegion);
		TextureData* paletteTex = createPaletteTexture(key, paletteMap, pool);
		TFE_Jedi::bitmap_setAllocator(prevTexAlloc);

		if (!paletteTex)
		{
			return nullptr;
		}

		struct TempPoly
		{
			s32 indices[4];
			u8 color;
		};

		std::vector<vec3> vertices;
		std::vector<TempPoly> polys;
		std::unordered_map<u64, s32> vertexMap;  // vertex deduplication

		// Voxel-to-world axis mapping:
		//   worldX = voxY, worldY = -voxZ (negated, DF Y points down), worldZ = voxX.
		// halfX/halfZ center the model around the origin in X and Z.
		const f32 scale = getVoxelScale(key);
		const fixed16_16 scaleDiv = div16(SPRITE_SCALE_FIXED, floatToFixed16(scale));

		const fixed16_16 halfX = div16(intToFixed16(data.sizeY), intToFixed16(2));  // worldX extent = voxY
		const fixed16_16 halfZ = div16(intToFixed16(data.sizeX), intToFixed16(2));  // worldZ extent = voxX

		// Returns the index of the vertex, reusing an existing one if possible.
		auto getOrAddVertex = [&](const vec3& v) -> s32
		{
			const u64 k = vertexKey(v);
			auto it = vertexMap.find(k);
			if (it != vertexMap.end())
			{
				// Verify it's actually the same vertex (hash collision check).
				const vec3& existing = vertices[it->second];
				if (existing.x == v.x && existing.y == v.y && existing.z == v.z)
				{
					return it->second;
				}
			}
			const s32 idx = (s32)vertices.size();
			vertices.push_back(v);
			vertexMap[k] = idx;
			return idx;
		};

		auto addFace = [&](const vec3& v0, const vec3& v1, const vec3& v2, const vec3& v3, u8 color)
		{
			TempPoly poly = {};
			poly.indices[0] = getOrAddVertex(v0);
			poly.indices[1] = getOrAddVertex(v1);
			poly.indices[2] = getOrAddVertex(v2);
			poly.indices[3] = getOrAddVertex(v3);
			poly.color = color;
			polys.push_back(poly);
		};

		const s32 strideY = data.sizeX;
		const s32 strideZ = data.sizeX * data.sizeY;

		for (s32 z = 0; z < data.sizeZ; z++)
		{
			for (s32 y = 0; y < data.sizeY; y++)
			{
				for (s32 x = 0; x < data.sizeX; x++)
				{
					const s32 index = x + y * strideY + z * strideZ;
					const u8 color = grid[index];
					if (!color)
					{
						continue;
					}

					// Neighbor checks in voxel space, mapped to world axes:
					//   worldX = voxY, worldY = -voxZ, worldZ = voxX.
					const bool worldNegX = (y == 0)              || (grid[index - strideY] == 0);
					const bool worldPosX = (y == data.sizeY - 1) || (grid[index + strideY] == 0);
					const bool worldNegZ = (x == 0)              || (grid[index - 1] == 0);
					const bool worldPosZ = (x == data.sizeX - 1) || (grid[index + 1] == 0);
					const bool worldPosY = (z == 0)              || (grid[index - strideZ] == 0);
					const bool worldNegY = (z == data.sizeZ - 1) || (grid[index + strideZ] == 0);

					const fixed16_16 fx0 = div16(intToFixed16(y) - halfX, scaleDiv);
					const fixed16_16 fx1 = div16(intToFixed16(y + 1) - halfX, scaleDiv);
					const fixed16_16 fz0 = div16(intToFixed16(x) - halfZ, scaleDiv);
					const fixed16_16 fz1 = div16(intToFixed16(x + 1) - halfZ, scaleDiv);
					// Negate Y so the model is right-side up (Dark Forces Y points down).
					const fixed16_16 fy0 = -div16(intToFixed16(z + 1), scaleDiv);
					const fixed16_16 fy1 = -div16(intToFixed16(z), scaleDiv);

					vec3 v000 = { fx0, fy0, fz0 };
					vec3 v100 = { fx1, fy0, fz0 };
					vec3 v010 = { fx0, fy1, fz0 };
					vec3 v110 = { fx1, fy1, fz0 };
					vec3 v001 = { fx0, fy0, fz1 };
					vec3 v101 = { fx1, fy0, fz1 };
					vec3 v011 = { fx0, fy1, fz1 };
					vec3 v111 = { fx1, fy1, fz1 };

					// Original winding order (CW front faces, matching 3DO convention).
					if (worldPosX) { addFace(v100, v110, v111, v101, color); }
					if (worldNegX) { addFace(v000, v001, v011, v010, color); }
					if (worldPosZ) { addFace(v001, v101, v111, v011, color); }
					if (worldNegZ) { addFace(v000, v010, v110, v100, color); }
					if (worldPosY) { addFace(v010, v011, v111, v110, color); }
					if (worldNegY) { addFace(v000, v100, v101, v001, color); }
				}
			}
		}

		if (polys.empty() || vertices.empty())
		{
			return nullptr;
		}

		JediModel* model = (JediModel*)TFE_Memory::region_alloc(memRegion, sizeof(JediModel));
		memset(model, 0, sizeof(JediModel));

		model->isBridge = 0;
		model->vertexCount = (s32)vertices.size();
		model->vertices = (vec3*)TFE_Memory::region_alloc(memRegion, vertices.size() * sizeof(vec3));
		memcpy(model->vertices, vertices.data(), vertices.size() * sizeof(vec3));

		model->polygonCount = (s32)polys.size();
		model->polygons = (JmPolygon*)TFE_Memory::region_alloc(memRegion, polys.size() * sizeof(JmPolygon));
		memset(model->polygons, 0, polys.size() * sizeof(JmPolygon));

		model->textureCount = 1;
		model->textures = (TextureData**)TFE_Memory::region_alloc(memRegion, sizeof(TextureData*));
		model->textures[0] = paletteTex;
		model->flags = 0;
		model->drawId = nullptr;

		for (s32 i = 0; i < model->polygonCount; i++)
		{
			JmPolygon* poly = &model->polygons[i];
			poly->index = i;
			poly->shading = PSHADE_FLAT;
			poly->color = paletteMap[polys[i].color];
			poly->texture = nullptr;
			poly->vertexCount = 4;
			poly->p08 = 0;
			poly->p24 = 0;

			poly->indices = (s32*)TFE_Memory::region_alloc(memRegion, sizeof(s32) * 4);
			memcpy(poly->indices, polys[i].indices, sizeof(s32) * 4);

			poly->uv = nullptr;
		}

		model->polygonNormals = (vec3*)TFE_Memory::region_alloc(memRegion, polys.size() * sizeof(vec3));
		for (s32 i = 0; i < model->polygonCount; i++)
		{
			const JmPolygon* poly = &model->polygons[i];
			const vec3* v0 = &model->vertices[poly->indices[0]];
			const vec3* v1 = &model->vertices[poly->indices[1]];
			const vec3* v2 = &model->vertices[poly->indices[2]];
			// Use same vertex order as 3DO models: (v1, v2, v0)
			computePolygonNormal(v1, v2, v0, &model->polygonNormals[i]);
		}

		// Compute radius.
		f32 maxDistSq = 0.0f;
		for (s32 i = 0; i < model->vertexCount; i++)
		{
			const f32 x = fixed16ToFloat(model->vertices[i].x);
			const f32 y = fixed16ToFloat(model->vertices[i].y);
			const f32 z = fixed16ToFloat(model->vertices[i].z);
			const f32 distSq = x * x + y * y + z * z;
			maxDistSq = std::max(maxDistSq, distSq);
		}
		model->radius = floatToFixed16(sqrtf(maxDistSq));

		TFE_System::logWrite(LOG_MSG, "Voxel", "Built '%s': %d polys, %d verts.", key.c_str(), model->polygonCount, model->vertexCount);
		return model;
	}

	static const std::string* findVoxelPath(const std::string& key)
	{
		auto it = s_voxelFiles.find(key);
		if (it != s_voxelFiles.end())
		{
			return &it->second;
		}

		// Try indexed variants (e.g., PROBE_01).
		for (s32 i = 1; i <= 99; i++)
		{
			char suffix[8];
			snprintf(suffix, sizeof(suffix), "_%02d", i);
			std::string candidate = key + suffix;
			auto it2 = s_voxelFiles.find(candidate);
			if (it2 != s_voxelFiles.end())
			{
				return &it2->second;
			}
		}

		// Try alphabetic variants (e.g., IARMOR_A, IARMOR_B).
		for (char c = 'A'; c <= 'Z'; c++)
		{
			std::string candidate = key + "_" + c;
			auto it2 = s_voxelFiles.find(candidate);
			if (it2 != s_voxelFiles.end())
			{
				return &it2->second;
			}
		}
		return nullptr;
	}

	void addVoxelRootPath(const char* path)
	{
		if (!path || !path[0])
		{
			return;
		}

		if (!FileUtil::directoryExits(path))
		{
			TFE_System::logWrite(LOG_WARNING, "Voxel", "Root path not found: '%s'.", path);
			return;
		}

		std::string fixed = path;
		std::replace(fixed.begin(), fixed.end(), '\\', '/');
		s_voxelRoots.push_back(fixed);
		s_voxelFilesDirty = true;
		TFE_System::logWrite(LOG_MSG, "Voxel", "Added root path '%s'.", fixed.c_str());
	}

	void clearLevelData()
	{
		for (s32 pool = 0; pool < POOL_COUNT; pool++)
		{
			for (auto& entry : s_voxelModels[pool])
			{
				s_voxelModelSet.erase(entry.second);
			}
			s_voxelModels[pool].clear();
		}
	}

	JediModel* getModelForName(const char* baseName, AssetPool pool)
	{
		if (!baseName || !baseName[0])
		{
			return nullptr;
		}

		const std::string key = normalizeBaseName(baseName);
		if (key.empty())
		{
			return nullptr;
		}

		auto it = s_voxelModels[pool].find(key);
		if (it != s_voxelModels[pool].end())
		{
			return it->second;
		}

		scanVoxelFiles();
		const std::string* path = findVoxelPath(key);
		if (!path)
		{
			return nullptr;
		}

		VoxData data;
		if (!loadVoxFile(path->c_str(), data))
		{
			TFE_System::logWrite(LOG_WARNING, "Voxel", "Failed to load '%s'.", path->c_str());
			return nullptr;
		}

		JediModel* model = buildVoxelModel(key, data, pool);
		if (!model)
		{
			return nullptr;
		}

		TFE_Model_Jedi::registerModel(baseName, model, pool);
		s_voxelModels[pool][key] = model;
		s_voxelModelSet.insert(model);

		return model;
	}

	JediModel* getModelForAssetName(const char* assetName, AssetPool pool)
	{
		if (!assetName || !assetName[0])
		{
			return nullptr;
		}

		const std::string key = normalizeBaseName(assetName);
		if (key.empty())
		{
			return nullptr;
		}

		auto it = s_voxelModels[pool].find(key);
		if (it != s_voxelModels[pool].end())
		{
			return it->second;
		}

		scanVoxelFiles();
		const std::string* path = findVoxelPath(key);
		if (!path)
		{
			return nullptr;
		}

		VoxData data;
		if (!loadVoxFile(path->c_str(), data))
		{
			TFE_System::logWrite(LOG_WARNING, "Voxel", "Failed to load '%s'.", path->c_str());
			return nullptr;
		}

		JediModel* model = buildVoxelModel(key, data, pool);
		if (!model)
		{
			return nullptr;
		}

		TFE_Model_Jedi::registerModel(assetName, model, pool);
		s_voxelModels[pool][key] = model;
		s_voxelModelSet.insert(model);

		return model;
	}

	bool isVoxelModel(const JediModel* model)
	{
		if (!model)
		{
			return false;
		}
		return s_voxelModelSet.find(model) != s_voxelModelSet.end();
	}
}
