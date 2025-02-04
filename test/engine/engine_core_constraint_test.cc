// Copyright 2021 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Tests for engine/engine_core_constraint.c.

#include <array>
#include <cstddef>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mujoco/mjmodel.h>
#include <mujoco/mujoco.h>
#include "src/engine/engine_core_constraint.h"
#include "src/engine/engine_support.h"
#include "test/fixture.h"

namespace mujoco {
namespace {

using ::testing::DoubleNear;
using ::testing::Pointwise;
using CoreConstraintTest = MujocoTest;

std::vector<mjtNum> AsVector(const mjtNum* array, int n) {
  return std::vector<mjtNum>(array, array + n);
}

// compute rotation residual following formula in mj_instantiateEquality
void RotationResidual(const mjModel *model, mjData *data,
                        const mjtNum qpos[7], const mjtNum dqpos[6],
                        mjtNum res[3]) {
  // copy configuration, compute required quantities with mj_step1
  mju_copy(data->qpos, qpos, 7);

  // perturb configuration if given
  if (dqpos) {
    mj_integratePos(model, data->qpos, dqpos, 1);
  }

  // update relevant quantities
  mj_step1(model, data);

  // compute orientation residual
  mjtNum quat1[4], quat2[4], quat3[4];
  mju_copy4(quat1, data->xquat+4*1);
  mju_negQuat(quat2, data->xquat+4*2);
  mju_mulQuat(quat3, quat2, quat1);
  mju_copy3(res, quat3+1);
}

// validate rotational Jacobian used in welds
TEST_F(CoreConstraintTest, WeldRotJacobian) {
  constexpr char xml[] = R"(
  <mujoco>
    <option jacobian="dense"/>
    <worldbody>
      <body>
        <joint type="ball"/>
        <geom size=".1"/>
      </body>
      <body pos=".5 0 0">
        <joint axis="1 0 0" pos="0 0 .01"/>
        <joint axis="0 1 0" pos=".02 0 0"/>
        <joint axis="0 0 1" pos="0 .03 0"/>
        <geom size=".1"/>
      </body>
    </worldbody>
  </mujoco>
  )";
  mjModel* model = LoadModelFromString(xml);
  ASSERT_THAT(model, testing::NotNull());
  ASSERT_EQ(model->nq, 7);
  ASSERT_EQ(model->nv, 6);
  static const int nv = 6;  // for increased readabilty
  mjData* data = mj_makeData(model);

  // arbitrary initial values for the ball and hinge joints
  mjtNum qpos0[7] = {.5, .5, .5, .5, .7, .8, .9};

  // compute required quantities using mj_step1
  mj_step1(model, data);

  // get orientation error
  mjtNum res[3];
  RotationResidual(model, data, qpos0, NULL, res);

  // compute Jacobian with finite-differencing
  mjtNum jacFD[3*nv];
  mjtNum dqpos[nv] = {0};
  mjtNum dres[3];
  const mjtNum eps = 1e-6;
  for (int i=0; i < nv; i++) {
    // nudge i-th dof
    dqpos[i] = eps;

    // get nudged residual
    RotationResidual(model, data, qpos0, dqpos, dres);

    // remove nudge
    dqpos[i] = 0.0;

    // compute Jacobian column
    for (int j=0; j < 3; j++) {
      jacFD[nv*j + i] = (dres[j] - res[j]) / eps;
    }
  }

  // reset mjData to qpos0
  mju_copy(data->qpos, qpos0, 7);
  mj_step1(model, data);

  // intermediate quaternions quat1 and quat2
  mjtNum quat1[4], negQuat2[4];
  mju_copy4(quat1, data->xquat+4*1);
  mju_negQuat(negQuat2, data->xquat+4*2);

  // get analytical Jacobian following formula in mj_instantiateEquality
  mjtNum jacdif[3*nv], jac0[3*nv], jac1[3*nv];
  mjtNum point[3] = {0};

  // rotational Jacobian difference
  mj_jacDifPair(model, data, NULL, 2, 1, point, point,
                NULL, NULL, NULL, jac0, jac1, jacdif);

  // formula: 0.5 * neg(quat2) * (jac1-jac2) * quat1
  mjtNum axis[3], quat3[4], quat4[4];
  for (int j=0; j < nv; j++) {
    // axis = [jac1-jac2]_col(j)
    axis[0] = jacdif[0*nv+j];
    axis[1] = jacdif[1*nv+j];
    axis[2] = jacdif[2*nv+j];

    // apply formula
    mju_mulQuatAxis(quat3, negQuat2, axis);
    mju_mulQuat(quat4, quat3, quat1);

    // correct Jacobian
    jacdif[0*nv+j] = 0.5*quat4[1];
    jacdif[1*nv+j] = 0.5*quat4[2];
    jacdif[2*nv+j] = 0.5*quat4[3];
  }

  // test that analytical and finite-differenced Jacobians match
  EXPECT_THAT(AsVector(jacFD, 3*nv),
              Pointwise(DoubleNear(eps), AsVector(jacdif, 3*nv)));

  mj_deleteData(data);
  mj_deleteModel(model);
}

static const char* const kDoflessContactPath =
    "engine/testdata/core_constraint/dofless_contact.xml";
static const char* const kDoflessTendonFrictionalPath =
    "engine/testdata/core_constraint/dofless_tendon_frictional.xml";
static const char* const kDoflessTendonLimitedPath =
    "engine/testdata/core_constraint/dofless_tendon_limited.xml";
static const char* const kDoflessTendonLimitedMarginPath =
    "engine/testdata/core_constraint/dofless_tendon_limitedmargin.xml";
static const char* const kDoflessWeldPath =
    "engine/testdata/core_constraint/dofless_weld.xml";
static const char* const kJointLimitedBilateralMarginPath =
    "engine/testdata/core_constraint/joint_limited_bilateral_margin.xml";
static const char* const kTendonLimitedBilateralMarginPath =
    "engine/testdata/core_constraint/tendon_limited_bilateral_margin.xml";

TEST_F(CoreConstraintTest, JacobianPreAllocate) {
  for (const char* local_path :
       {kDoflessContactPath, kDoflessTendonFrictionalPath,
        kDoflessTendonLimitedPath, kDoflessTendonLimitedMarginPath,
        kDoflessWeldPath, kJointLimitedBilateralMarginPath,
        kTendonLimitedBilateralMarginPath}) {
    const std::string xml_path = GetTestDataFilePath(local_path);

    // iterate through dense and sparse
    for (mjtJacobian sparsity : {mjJAC_DENSE, mjJAC_SPARSE}) {
      mjModel* model = mj_loadXML(xml_path.c_str(), nullptr, nullptr, 0);
      model->opt.jacobian = sparsity;
      mjData* data = mj_makeData(model);

      mj_step(model, data);

      mj_deleteData(data);
      mj_deleteModel(model);
    }
  }
}

TEST_F(CoreConstraintTest, CombineSparseCount) {
  {
    std::array a_ind{0, 1};
    std::array b_ind{2};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 3);
  }

  {
    std::array a_ind{2};
    std::array b_ind{0, 1};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 3);
  }

  {
    std::array a_ind{0, 1};
    std::array b_ind{2, 3, 4};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 5);
  }

  {
    std::array a_ind{5, 6};
    std::array b_ind{1, 3, 8};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 5);
  }

  {
    std::array a_ind{1, 2, 3};
    std::array b_ind{0, 4};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 5);
  }

  {
    std::array a_ind{1, 4};
    std::array b_ind{2, 3};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 4);
  }

  {
    std::array a_ind{0, 1, 3};
    std::array b_ind{0, 3, 4};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 4);
  }

  {
    std::array a_ind{1, 3, 5, 6};
    std::array b_ind{1, 3, 5, 6};
    EXPECT_EQ(mju_combineSparseCount(
        a_ind.size(), b_ind.size(), a_ind.data(), b_ind.data()), 4);
  }

  EXPECT_EQ(mju_combineSparseCount(0, 0, nullptr, nullptr), 0);

  {
    std::array b_ind{1, 2};
    EXPECT_EQ(
        mju_combineSparseCount(0, b_ind.size(), nullptr, b_ind.data()), 2);
  }

  {
    std::array a_ind{0};
    EXPECT_EQ(
        mju_combineSparseCount(a_ind.size(), 0, a_ind.data(), nullptr), 1);
  }
}

}  // namespace
}  // namespace mujoco
