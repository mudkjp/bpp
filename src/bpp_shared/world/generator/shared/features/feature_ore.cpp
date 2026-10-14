/*
 * Copyright (c) 2025-2026, Pixel Brush <pixelbrush.dev>
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 * Based on code by Mojang Studios (2011)
*/

#include "../feature_gen.h"

#include <array>
#include <cassert>

bool FeatureGenerator::GenerateOre(BlockType _type, WorldWrapper& _world, Java::Random& _rand, Int3 _pos, int32_t _blobSize) {
	float angle = _rand.NextFloat() * JavaMath::PI_FLOAT;
	double xStart = double(float(_pos.x + 8) + MathHelper::Sin(angle) * float(_blobSize) / 8.0F);
	double xEnd = double(float(_pos.x + 8) - MathHelper::Sin(angle) * float(_blobSize) / 8.0F);
	double zStart = double(float(_pos.z + 8) + MathHelper::Cos(angle) * float(_blobSize) / 8.0F);
	double zEnd = double(float(_pos.z + 8) - MathHelper::Cos(angle) * float(_blobSize) / 8.0F);
	double yStart = double(_pos.y + _rand.NextInt(3) + 2);
	double yEnd = double(_pos.y + _rand.NextInt(3) + 2);

	// The blob is an ellipsoid test per voxel: dx*dx + dy*dy + dz*dz < 1. Each d only depends on its own
	// axis, so the divisions and squares are computed once per step instead of once per voxel. The values
	// and the order they are summed in are unchanged, so the same voxels pass the test as before.
	constexpr int32_t MAX_BLOB_SPAN = 16;
	std::array<double, MAX_BLOB_SPAN> dxSquares;
	std::array<double, MAX_BLOB_SPAN> dySquares;
	std::array<double, MAX_BLOB_SPAN> dzSquares;

	for (int32_t i = 0; i <= _blobSize; ++i) {
		double xC = xStart + (xEnd - xStart) * double(i) / double(_blobSize);
		double yC = yStart + (yEnd - yStart) * double(i) / double(_blobSize);
		double zC = zStart + (zEnd - zStart) * double(i) / double(_blobSize);
		double blobScale = _rand.NextDouble() * double(_blobSize) / 16.0;
		double radXZ = double(MathHelper::Sin(float(i) * JavaMath::PI_FLOAT / float(_blobSize)) + 1.0F) * blobScale +
		               1.0;
		double radY = double(MathHelper::Sin(float(i) * JavaMath::PI_FLOAT / float(_blobSize)) + 1.0F) * blobScale + 1.0;
		int32_t minX = MathHelper::FloorDouble(xC - radXZ / 2.0);
		int32_t maxX = MathHelper::FloorDouble(xC + radXZ / 2.0);
		int32_t minY = MathHelper::FloorDouble(yC - radY / 2.0);
		int32_t maxY = MathHelper::FloorDouble(yC + radY / 2.0);
		int32_t minZ = MathHelper::FloorDouble(zC - radXZ / 2.0);
		int32_t maxZ = MathHelper::FloorDouble(zC + radXZ / 2.0);

		const double halfXZ = radXZ / 2.0;
		const double halfY = radY / 2.0;
		assert(maxX - minX < MAX_BLOB_SPAN && maxY - minY < MAX_BLOB_SPAN && maxZ - minZ < MAX_BLOB_SPAN);
		for (int32_t x = minX; x <= maxX; ++x) {
			const double dx = (double(x) + 0.5 - xC) / halfXZ;
			dxSquares[size_t(x - minX)] = dx * dx;
		}
		for (int32_t y = minY; y <= maxY; ++y) {
			const double dy = (double(y) + 0.5 - yC) / halfY;
			dySquares[size_t(y - minY)] = dy * dy;
		}
		for (int32_t z = minZ; z <= maxZ; ++z) {
			const double dz = (double(z) + 0.5 - zC) / halfXZ;
			dzSquares[size_t(z - minZ)] = dz * dz;
		}

		for (int32_t x = minX; x <= maxX; ++x) {
			const double dx2 = dxSquares[size_t(x - minX)];
			if (dx2 >= 1.0)
				continue;
			for (int32_t y = minY; y <= maxY; ++y) {
				const double dy2 = dySquares[size_t(y - minY)];
				if (dx2 + dy2 >= 1.0)
					continue;
				for (int32_t z = minZ; z <= maxZ; ++z) {
					if (dx2 + dy2 + dzSquares[size_t(z - minZ)] < 1.0)
						_world.SetBlockIf({ x, y, z }, BLOCK_STONE, _type);
				}
			}
		}
	}
	return true;
}