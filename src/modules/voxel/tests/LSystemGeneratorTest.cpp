/**
 * @file
 */

#include "AbstractVoxelTest.h"
#include "voxel/generator/LSystemGenerator.h"
#include "voxel/polyvox/Region.h"
#include "voxel/polyvox/PagedVolume.h"

namespace voxel {

class LSystemGeneratorTest: public AbstractVoxelTest {
};

TEST_F(LSystemGeneratorTest, testStatePushPop) {
	LSystemContext lsystemCtx;
	// we change the coordinates in x, y and z directions once, then we push a new state and pop that
	// new state again - which means, that we don't modify the initial state => hence the 1, 1, 1
	lsystemCtx.axiom = "AXYZ[XYZ]";
	lsystemCtx.voxels.emplace('A', createVoxel(Wood1));

	LSystemState state;
	LSystemGenerator::expand(&state, _ctx, lsystemCtx, _random, lsystemCtx.axiom, lsystemCtx.generations);

	ASSERT_EQ(1, state.pos.x);
	ASSERT_EQ(1, state.pos.y);
	ASSERT_EQ(1, state.pos.z);
}

TEST_F(LSystemGeneratorTest, testStatePushPopPositionChangeToInit) {
	LSystemContext lsystemCtx;
	lsystemCtx.axiom = "AXYZ[XYZ]xyz";
	lsystemCtx.voxels.emplace('A', createVoxel(Wood1));

	LSystemState state;
	LSystemGenerator::expand(&state, _ctx, lsystemCtx, _random, lsystemCtx.axiom, lsystemCtx.generations);

	ASSERT_EQ(0, state.pos.x);
	ASSERT_EQ(0, state.pos.y);
	ASSERT_EQ(0, state.pos.z);
}

TEST_F(LSystemGeneratorTest, testMultipleStates) {
	LSystemContext lsystemCtx;
	lsystemCtx.axiom = "AY[xYA]AY[XYA]AY";
	lsystemCtx.productionRules.emplace('A', lsystemCtx.axiom);
	lsystemCtx.voxels.emplace('A', createVoxel(Wood1));
	lsystemCtx.generations = 2;

	LSystemState state;
	LSystemGenerator::expand(&state, _ctx, lsystemCtx, _random, lsystemCtx.axiom, lsystemCtx.generations);

	ASSERT_EQ(0, state.pos.x);
	ASSERT_EQ(12, state.pos.y);
	ASSERT_EQ(0, state.pos.z);
}

TEST_F(LSystemGeneratorTest, testStatePositionChangeTwice) {
	LSystemContext lsystemCtx;
	lsystemCtx.axiom = "AXYZXYZ";
	lsystemCtx.voxels.emplace('A', createVoxel(Wood1));

	LSystemState state;
	LSystemGenerator::expand(&state, _ctx, lsystemCtx, _random, lsystemCtx.axiom, lsystemCtx.generations);

	ASSERT_EQ(2, state.pos.x);
	ASSERT_EQ(2, state.pos.y);
	ASSERT_EQ(2, state.pos.z);
}

TEST_F(LSystemGeneratorTest, testGenerateVoxels) {
	LSystemContext lsystemCtx;
	lsystemCtx.axiom = "AB";
	lsystemCtx.generations = 2;

	lsystemCtx.productionRules.emplace('A', "XAxYAXBXXYYZZ");
	lsystemCtx.productionRules.emplace('B', "A[zC]");

	lsystemCtx.voxels.emplace('A', createVoxel(Wood1));
	lsystemCtx.voxels.emplace('B', createVoxel(Grass1));
	lsystemCtx.voxels.emplace('C', createVoxel(Leaves4));

	LSystemGenerator::generate(_ctx, lsystemCtx, _random);
}

}
