#include <math.h>
#include "WalkTest.h"
#include "services/ModelService.h"
#include "rhoban_utils/angle.h"

using namespace rhoban_utils;

WalkTest::WalkTest()
{
  Move::initializeBinding();
  swingGainStart = 0.04;

  bind->bindNew("walkEnable", walkEnable, RhIO::Bind::PullOnly)->defaultValue(false);
  bind->bindNew("trunkPitch", trunkPitch, RhIO::Bind::PullOnly)->defaultValue(-15);

  bind->bindNew("trunkXOffset", engine.trunkXOffset, RhIO::Bind::PullOnly)->defaultValue(engine.trunkXOffset);
  bind->bindNew("trunkZOffset", engine.trunkZOffset, RhIO::Bind::PullOnly)->defaultValue(engine.trunkZOffset);
  bind->bindNew("frequency", engine.frequency, RhIO::Bind::PullOnly)->defaultValue(engine.frequency);
  bind->bindNew("footYOffset", engine.footYOffset, RhIO::Bind::PullOnly)->defaultValue(engine.footYOffset);
  bind->bindNew("riseGain", engine.riseGain, RhIO::Bind::PullOnly)->defaultValue(engine.riseGain);
  bind->bindNew("riseDuration", engine.riseDuration, RhIO::Bind::PullOnly)->defaultValue(engine.riseDuration);
  bind->bindNew("swingGain", engine.swingGain, RhIO::Bind::PullOnly)->defaultValue(engine.swingGain);
  bind->bindNew("swingGainStart", swingGainStart, RhIO::Bind::PullOnly)->defaultValue(swingGainStart);
  bind->bindNew("swingPhase", engine.swingPhase, RhIO::Bind::PullOnly)->defaultValue(engine.swingPhase);
  bind->bindNew("footYOffsetPerYSpeed", engine.footYOffsetPerYSpeed, RhIO::Bind::PullOnly)
      ->defaultValue(engine.footYOffsetPerYSpeed);

  bind->bindNew("xSpeed", engine.xSpeed, RhIO::Bind::PullOnly)->defaultValue(0.0);
  bind->bindNew("ySpeed", engine.ySpeed, RhIO::Bind::PullOnly)->defaultValue(0.0);
  bind->bindNew("yawSpeed", engine.yawSpeed, RhIO::Bind::PullOnly)->defaultValue(0.0);

  bind->bindNew("armsRoll", armsRoll, RhIO::Bind::PullOnly)
      ->defaultValue(-5.0)
      ->minimum(-20.0)
      ->maximum(150.0)
      ->persisted(true);
  bind->bindNew("elbowOffset", elbowOffset, RhIO::Bind::PullOnly)
      ->defaultValue(-168.0)
      ->minimum(-200.0)
      ->maximum(30.0)
      ->persisted(true);

  timeSinceLastStep = 0;
}

std::string WalkTest::getName()
{
  return "walk_test";
}

void WalkTest::onStart()
{
  Leph::HumanoidFixedModel& model = getServices()->model->goalModel();
  engine.initByModel(model);

  bind->node().setBool("walkEnable", false);

  bind->node().setFloat("xSpeed", 0.0);
  bind->node().setFloat("ySpeed", 0.0);
  bind->node().setFloat("yawSpeed", 0.0);

  engine.swingGain = 0;
  engine.riseGain = 0;
  engine.xSpeed = 0;
  engine.ySpeed = 0;
  engine.yawSpeed = 0;
  stepCount = 0;
  engine.reset();
}

void WalkTest::step(float elapsed)
{
  bind->pull();

  // Ticking 
  timeSinceLastStep += elapsed;
  double over = engine.update(timeSinceLastStep);
  if (over > 0)
  {
    timeSinceLastStep = over;

    if (!walkEnable)
    {
      engine.swingGain = 0;
      engine.riseGain = 0;
      engine.xSpeed = 0;
      engine.ySpeed = 0;
      engine.yawSpeed = 0;
      stepCount = 0;
    }
    else
    {
      stepCount += 1;

      if (stepCount <= 2)
      {
        engine.swingGain = swingGainStart;
      }
    }

    // Creating a new footstep
    engine.newStep();

    // Updating the engine again with the time elapsed since it began
    engine.update(timeSinceLastStep);
  }

  // Assigning to robot
  engine.assignModel(getServices()->model->goalModel());

  // Flushing engine leg orders to robot
  ModelService* model = getServices()->model;
  model->flushLegs(_smoothing);

  // Applying extra trunk pitch
  setAngle("left_hip_pitch", trunkPitch);
  setAngle("right_hip_pitch", trunkPitch);

  // IMU Pitch to arms
  float imuPitch = rad2deg(getPitch());
  setAngle("left_shoulder_pitch", imuPitch);
  setAngle("right_shoulder_pitch", imuPitch);
  // Rolls to arms
  setAngle("left_shoulder_roll", armsRoll);
  setAngle("right_shoulder_roll", -armsRoll);
  // Elbows
  setAngle("left_elbow", elbowOffset);
  setAngle("right_elbow", elbowOffset);

  bind->push();
}