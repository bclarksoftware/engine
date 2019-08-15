/**
 * @file
 */

#pragma once

#include "voxel/polyvox/RawVolumeWrapper.h"
#include "math/Axis.h"
#include "../ModifierType.h"
#include "../Selection.h"

namespace voxedit {
namespace tool {

extern bool aabb(voxel::RawVolumeWrapper& target, const glm::ivec3& mins, const glm::ivec3& maxs, const voxel::Voxel& voxel, ModifierType modifierType, const Selections& selection, voxel::Region* modifiedRegion = nullptr);

}
}
