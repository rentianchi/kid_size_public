#include <iostream>
#include <stdexcept>
#include <rhoban_utils/angle.h>
#include <rhoban_utils/util.h>
#include <Utils/Angle.h>
#include <Model/NamesModel.h>
#include <rhoban_utils/timing/time_stamp.h>
#include "services/ModelService.h"
#include "services/LocalisationService.h"
#include "moves/Move.h"
#include "scheduler/MoveScheduler.h"
#include <string>
#include <vector>
#include <algorithm>

/**
 *  - DOF goals
 * 
 *  - DOF reads
 *  - Pressure
 *  - IMU
 *  - Camera pose
 */

using namespace rhoban_utils;

ModelService::ModelService()
  : _noIMU(false)
  , _isReadVoltages(false)
  , _histories()
  , _goalModel(InitHumanoidModel<Leph::HumanoidFixedModel>())
  , _readModel(InitHumanoidModel<Leph::HumanoidFixedPressureModel>())
  , _correctedModel(InitHumanoidModel<Leph::HumanoidFixedPressureModel>())
  , _cameraModel()
  , _imuOffset(Eigen::Vector3d::Zero())
  , _isUpdateReadBase(false)
  , _wasUpdateReadBase(false)
  , _forceUpdateReadBase(false)
  , _timestamp()
  , _isReplay(false)
  , _replayTimestamp(0.0)
  , _bind("model")
  , _odometry(Leph::OdometryDisplacementModel::DisplacementProportionalXYA, Leph::OdometryNoiseModel::NoiseDisable)
  , _doWriteLog(false)
  , _logPath("")
{
  // Reading the correection to bring to the model
  rhoban_model_learning::CalibrationModel calibration_model;
  calibration_model.loadFile("calibration.json");
  // _cameraModel = calibration_model.getCameraModel();
  _imuOffset = calibration_model.getImuOffsetsRad();

  // Load odometry model parameters from file
  // Open file
  std::ifstream fileOdo("odometryModel.params");
  // Check open
  if (fileOdo.is_open())
  {
    // Read data
    int tmpTypeDisplacement;
    int tmpTypeNoise;
    fileOdo >> tmpTypeDisplacement;
    fileOdo >> tmpTypeNoise;
    Leph::OdometryDisplacementModel::Type typeDisplacement = (Leph::OdometryDisplacementModel::Type)tmpTypeDisplacement;
    Leph::OdometryNoiseModel::Type typeNoise = (Leph::OdometryNoiseModel::Type)tmpTypeNoise;
    Eigen::VectorXd odoParams = Leph::ReadEigenVectorFromStream(fileOdo);
    // Assign the odometry model
    _odometry = Leph::Odometry(typeDisplacement, typeNoise);
    double isError = _odometry.setParameters(odoParams);
    // Check parameters validity
    if (isError > 0.0)
    {
      throw std::logic_error("ModelService loading invalid " + std::string("odometry parameters in the file ") +
                             std::string("odometryModel.params. Just delete it !"));
    }
    fileOdo.close();
  }
  else
  {
    // Warning message
    std::cout << "WARNING odometryModel.params configuration"
              << " not found. Default model is used." << std::endl;
  }
  fileOdo.close();

  // RhIO command to start/stop logging
  RhIO::Root.newCommand("/modelservice_start_log",
                        "<filepath>. Start logging LowLevel in ModelService",
                        [this](const std::vector<std::string>& args) -> std::string {
                          if (args.size() != 2 || args[0] == "" || (args[1] != "true" && args[1] != "false"))
                          {
                            return "Usage: model_start_log <filepath> <isBinaryFormat>";
                          }
                          this->startLogging(args[0]);
                          return "LowLevel Logging Start to be written to: " + args[0];
                        });
  RhIO::Root.newCommand("/modelservice_stop_log", "Stop logging in ModelService and write to file",
                        [this](const std::vector<std::string>& args) -> std::string {
                          this->stopLogging();
                          return "LowLevel Logging Stopped and written to: " + this->_logPath;
                        });
  // Load Replay
  RhIO::Root.newCommand("/modelservice_load_log", "<filepath> Load and play in ModelService given file",
                        [this](const std::vector<std::string>& args) -> std::string {
                          if (args.size() != 1 || args[0] == "")
                          {
                            return "Usage: model_load_log <filepath>";
                          }
                          this->loadReplays(args[0]);
                          return "LowLevel Start Replay from: " + args[0];
                        });
  // Reset odometry
  RhIO::Root.newCommand("/modelservice_odometry_reset", "Reset to origin corrected and goal odometry",
                        [this](const std::vector<std::string>& args) -> std::string {
                          // Corrected model
                          this->_odometry.reset();
                          this->_correctedModel.setOrientation(Eigen::Matrix3d::Identity(), true);
                          this->_correctedModel.setOdometryState(Eigen::Vector2d(0.0, 0.0));
                          // Goal model
                          this->_goalModel.setOrientation(Eigen::Matrix3d::Identity(), true);
                          this->_goalModel.setOdometryState(Eigen::Vector2d(0.0, 0.0));
                          // Reset localization
                          Eigen::Vector3d fieldInWorld(0.0, 0.0, 0.0);
                          Helpers::getServices()->localisation->setPosSelf(fieldInWorld, 0.0, 1.0, 1.0);
                          Helpers::getServices()->localisation->resetFieldFilter();
                          return "Reset odometry and localization to origin";
                        });
  // Bind service parameters
  _bind.bindNew("forceUpdate", _forceUpdateReadBase, RhIO::Bind::PullOnly)
      ->comment("Force read model base update")
      ->defaultValue(false);
  _bind.bindNew("noIMU", _noIMU, RhIO::Bind::PullOnly)
      ->comment("Disable IMU pitch/roll use for all models")
      ->persisted(true)
      ->defaultValue(false);
  _bind.bindNew("isReadVoltages", _isReadVoltages, RhIO::Bind::PullOnly)
      ->comment("Enable lowlevel control and power voltage read and log")
      ->persisted(true)
      ->defaultValue(false);
  _bind.bindNew("isBaseUpdated", _isUpdateReadBase, RhIO::Bind::PushOnly)->comment("Is the models currently updated");
  _bind.bindNew("lowlevel_state", _lowlevelState, RhIO::Bind::PushOnly)->comment("RhAL missing state");
  // Initialize the model type value
  RhIO::Root.newStr("/model/modelType")
      ->persisted(true)
      ->comment("Loaded model type. sigmaban or grosban.")
      ->defaultValue("sigmaban");
  // Add odometry export RhIO node
  _bind.node().newFloat("x")->comment("Corrected odometry X world position");
  _bind.node().newFloat("y")->comment("Corrected odometry Y world position");
  _bind.node().newFloat("theta")->comment("Corrected odometry Azimuth world in radian");
  _bind.node().newFloat("read_x")->comment("Read odometry X world position");
  _bind.node().newFloat("read_y")->comment("Read odometry Y world position");
  _bind.node().newFloat("read_theta")->comment("Read odometry Azimuth world in radian");
  // Initialize model publishing
  publishModelState("goal_", _goalModel, true);
  publishModelState("read_", _readModel, true);
  publishModelState("corrected_", _correctedModel, true);

  // Initialize history
  for (const std::string& name : Leph::NamesDOF)
  {
    _histories.number("read:" + name);
    _histories.number("goal:" + name);
  }
  _histories.number("is_base_updated");
  _histories.number("read:left_pressure_weight");
  _histories.number("read:left_pressure_x");
  _histories.number("read:left_pressure_y");
  _histories.number("read:right_pressure_weight");
  _histories.number("read:right_pressure_x");
  _histories.number("read:right_pressure_y");
  _histories.angle("read:imu_yaw");
  _histories.angle("read:imu_pitch");
  _histories.angle("read:imu_roll");
  _histories.number("read:magneto");
  _histories.number("read:base_x");
  _histories.number("read:base_y");
  _histories.number("read:base_z");
  _histories.angle("read:base_yaw");
  _histories.angle("read:base_pitch");
  _histories.angle("read:base_roll");
  _histories.number("read:is_left_support_foot");
  _histories.number("goal:base_x");
  _histories.number("goal:base_y");
  _histories.number("goal:base_z");
  _histories.angle("goal:base_yaw");
  _histories.angle("goal:base_pitch");
  _histories.angle("goal:base_roll");
  _histories.number("goal:is_left_support_foot");
  // Corrected odometry
  _histories.number("read:odometry_x");
  _histories.number("read:odometry_y");
  _histories.angle("read:odometry_yaw");

  for (auto& it : _histories.entries())
  {
    it.second->setWindowSize(60.0);
  }
}

bool ModelService::tick(double elapsed)
{
  tickDumpLogs();

  _bind.pull();

  tickCheckLowlevelState();

  tickFindTimestamp();

  tickAssignReadDOF();

  tickAssignReadPressure();

  // Update the base position only if
  // the robot is moving
  if (_forceUpdateReadBase || _isUpdateReadBase)
  {
    if (!Helpers::isPython) {
      _goalModel.updateBase();
      _readModel.updateBase();
    }
  }

  if (_isUpdateReadBase)
  {
    _wasUpdateReadBase = true;
  }

  tickAssignReadIMU();

  // If replay mode is enable, history is not update
  if (_isReplay)
  {
    // Update corrected from history
    pastReadModel(_replayTimestamp, _readModel);
    // Publish it for debug
    publishModelState("read_", _readModel);
    return true;
  }

  tickOdometryUpdate();

  tickRhIOPublish();

  tickHistoryUpdate();

  // Update corrected odometry model
  pastReadModel(RhAL::duration_float(_timestamp), _correctedModel);

  // RhIO push
  _bind.push();

  return true;
}

void ModelService::flushAll(double gain)
{
  flush(true, true, true, true, true, gain);
}
void ModelService::flushArms(double gain)
{
  flush(false, true, true, false, false, gain);
}
void ModelService::flushHead(double gain)
{
  flush(true, false, false, false, false, gain);
}
void ModelService::flushLegs(double gain)
{
  flush(false, false, false, true, true, gain);
}
void ModelService::flushLeftLeg(double gain)
{
  flush(false, false, false, true, false, gain);
}
void ModelService::flushRightLeg(double gain)
{
  flush(false, false, false, false, true, gain);
}

void ModelService::flush(bool doHead, bool doLeftArm, bool doRightArm, bool doLeftLeg, bool doRightLeg, double gain)
{
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  if (doHead)
  {
    manager->dev<RhAL::DXL>("head_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("head_yaw")) * gain);
    manager->dev<RhAL::DXL>("head_yaw")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("head_pitch")) * gain);
  }
  if (doLeftArm)
  {
    manager->dev<RhAL::DXL>("left_shoulder_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_shoulder_pitch")) * gain);
    manager->dev<RhAL::DXL>("left_shoulder_roll")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_shoulder_roll")) * gain);
    manager->dev<RhAL::DXL>("left_elbow")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_elbow")) * gain);
  }
  if (doRightArm)
  {
    manager->dev<RhAL::DXL>("right_shoulder_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_shoulder_pitch")) * gain);
    manager->dev<RhAL::DXL>("right_shoulder_roll")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_shoulder_roll")) * gain);
    manager->dev<RhAL::DXL>("right_elbow")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_elbow")) * gain);
  }
  if (doLeftLeg)
  {
    manager->dev<RhAL::DXL>("left_hip_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_hip_pitch")) * gain);
    manager->dev<RhAL::DXL>("left_hip_roll")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_hip_roll")) * gain);
    manager->dev<RhAL::DXL>("left_hip_yaw")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_hip_yaw")) * gain);
    manager->dev<RhAL::DXL>("left_ankle_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_ankle_pitch")) * gain);
    manager->dev<RhAL::DXL>("left_ankle_roll")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_ankle_roll")) * gain);
    manager->dev<RhAL::DXL>("left_knee")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("left_knee")) * gain);
  }
  if (doRightLeg)
  {
    manager->dev<RhAL::DXL>("right_hip_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_hip_pitch")) * gain);
    manager->dev<RhAL::DXL>("right_hip_roll")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_hip_roll")) * gain);
    manager->dev<RhAL::DXL>("right_hip_yaw")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_hip_yaw")) * gain);
    manager->dev<RhAL::DXL>("right_ankle_pitch")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_ankle_pitch")) * gain);
    manager->dev<RhAL::DXL>("right_ankle_roll")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_ankle_roll")) * gain);
    manager->dev<RhAL::DXL>("right_knee")
        .goalPosition()
        .writeValue(RhAL::Rad2Deg(_goalModel.get().getDOF("right_knee")) * gain);
  }
  // Resync goal model with RhAL target position
  for (const std::string& name : Leph::NamesDOF)
  {
    _goalModel.get().setDOF(name, RhAL::Deg2Rad(manager->dev<RhAL::DXL>(name).goalPosition().getWrittenValue()));
  }
}

Leph::HumanoidFixedModel& ModelService::goalModel()
{
  return _goalModel;
}
Leph::HumanoidFixedPressureModel& ModelService::readModel()
{
  return _readModel;
}
Leph::HumanoidFixedPressureModel& ModelService::correctedModel()
{
  return _correctedModel;
}

void ModelService::pastReadModel(double timestamp, Leph::HumanoidFixedPressureModel& pastReadModel)
{
  // Assign DOF position
  for (const std::string& name : Leph::NamesDOF)
  {
    pastReadModel.get().setDOF(name, _histories.number("read:" + name)->interpolate(timestamp));
  }
  // Update for pressure sensors
  double left_weight = _histories.number("read:left_pressure_weight")->interpolate(timestamp);
  double left_x = _histories.number("read:left_pressure_x")->interpolate(timestamp);
  double left_y = _histories.number("read:left_pressure_y")->interpolate(timestamp);
  double right_weight = _histories.number("read:right_pressure_weight")->interpolate(timestamp);
  double right_x = _histories.number("read:right_pressure_x")->interpolate(timestamp);
  double right_y = _histories.number("read:right_pressure_y")->interpolate(timestamp);
  if (left_weight + right_weight > 0.0001)
  {
    pastReadModel.setPressure((left_weight + right_weight), left_weight / (left_weight + right_weight),
                              right_weight / (left_weight + right_weight), left_x, left_y, right_x, right_y);
  }
  // Update model support foot
  pastReadModel.updateBase();
  // Update for imu orientation extrinsic Euler angles
  double yaw = _histories.angle("read:odometry_yaw")->interpolate(timestamp);
  double pitch = _histories.angle("read:imu_pitch")->interpolate(timestamp);
  double roll = _histories.angle("read:imu_roll")->interpolate(timestamp);
  Eigen::Matrix3d imuMatrix = Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitZ()).toRotationMatrix() *
                              Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitY()).toRotationMatrix() *
                              Eigen::AngleAxisd(roll, Eigen::Vector3d::UnitX()).toRotationMatrix();
  if (_noIMU)
  {
    pastReadModel.setYaw(yaw);
  }
  else
  {
    pastReadModel.setOrientation(imuMatrix);
  }
  // Assign interpolated past corrected odometry state
  pastReadModel.setOdometryState(Eigen::Vector2d(_histories.number("read:odometry_x")->interpolate(timestamp),
                                                 _histories.number("read:odometry_y")->interpolate(timestamp)));
}
double ModelService::pastMagneto(double timestamp)
{
  throw std::logic_error("We don't use magneto anymore");
}
bool ModelService::pastIsBaseUpdated(double timestamp)
{
  return (_histories.number("is_base_updated")->interpolate(timestamp) >= 0.5);
}

void ModelService::setReadBaseUpdate(bool isEnable)
{
  _isUpdateReadBase = isEnable;
}

bool ModelService::wasReadBaseUpdate()
{
  bool result = _wasUpdateReadBase;
  _wasUpdateReadBase = false;
  return result;
}

const RhAL::TimePoint& ModelService::readTimestamp()
{
  return _timestamp;
}

Eigen::Vector3d ModelService::odometryDiff(double timestamp1, double timestamp2)
{
  Eigen::Vector3d state1;
  state1.x() = _histories.number("read:odometry_x")->interpolate(timestamp1);
  state1.y() = _histories.number("read:odometry_y")->interpolate(timestamp1);
  state1.z() = _histories.angle("read:odometry_yaw")->interpolate(timestamp1);

  Eigen::Vector3d state2;
  state2.x() = _histories.number("read:odometry_x")->interpolate(timestamp2);
  state2.y() = _histories.number("read:odometry_y")->interpolate(timestamp2);
  state2.z() = _histories.angle("read:odometry_yaw")->interpolate(timestamp2);

  return odometryDiff(state1, state2);
}

void ModelService::startLogging(const std::string& filepath)
{
  _histories.startNamedLog(filepath);
  _logPath = filepath;
}

void ModelService::stopLogging()
{
  // Ask write to next tick
  _doWriteLog = true;
}

void ModelService::startNamedLog(const std::string& filePath)
{
  _histories.startNamedLog(filePath);
}

void ModelService::stopNamedLog(const std::string& filePath)
{
  _histories.stopNamedLog(filePath);
}

void ModelService::loadReplays(const std::string& filepath)
{
  _isReplay = true;
  _histories.loadReplays(filepath);
  _replayTimestamp = _histories.smallestTimestamp();
  std::cout << "Replay timestamp: " << _replayTimestamp << std::endl;
}

void ModelService::setReplayTimestamp(double ts)
{
  _replayTimestamp = ts;
}

Leph::Odometry& ModelService::getOdometryModel()
{
  return _odometry;
}

const Leph::CameraModel& ModelService::getCameraModel() const
{
  return _cameraModel;
}

Leph::HumanoidFixedModel::SupportFoot ModelService::getSupportFoot() const
{
  return _readModel.getSupportFoot();
}

const std::string& ModelService::getLowLevelState() const
{
  return _lowlevelState;
}

Eigen::Vector3d ModelService::odometryDiff(const Eigen::Vector3d& state1, const Eigen::Vector3d& state2) const
{
  // Vector in world
  double vectX = state2.x() - state1.x();
  double vectY = state2.y() - state1.y();
  double angle = Leph::AngleDistance(state1.z(), state2.z());

  double vectInSrcX = vectX * cos(-state1.z()) - vectY * sin(-state1.z());
  double vectInSrcY = vectX * sin(-state1.z()) + vectY * cos(-state1.z());

  return Eigen::Vector3d(vectInSrcX, vectInSrcY, angle);
}

void ModelService::odometryInt(const Eigen::Vector3d& diff, Eigen::Vector3d& state) const
{
  double vectX = diff.x() * cos(state.z()) - diff.y() * sin(state.z());
  double vectY = diff.x() * sin(state.z()) + diff.y() * cos(state.z());
  state.x() += vectX;
  state.y() += vectY;
  state.z() += diff.z();
  if (state.z() > M_PI)
    state.z() -= 2.0 * M_PI;
  if (state.z() < -M_PI)
    state.z() += 2.0 * M_PI;
}

void ModelService::publishModelState(const std::string& prefix, const Leph::HumanoidFixedModel& model, bool initialize)
{
  // RhIO Initialization
  if (initialize)
  {
    for (const std::string& name : Leph::NamesDOF)
    {
      _bind.node().newFloat(prefix + name);
    }
    _bind.node().newFloat(prefix + "base_x");
    _bind.node().newFloat(prefix + "base_y");
    _bind.node().newFloat(prefix + "base_z");
    _bind.node().newFloat(prefix + "base_yaw");
    _bind.node().newFloat(prefix + "base_pitch");
    _bind.node().newFloat(prefix + "base_roll");
    _bind.node().newInt(prefix + "support_foot");
  }
  // Assign DOF position
  for (const std::string& name : Leph::NamesDOF)
  {
    _bind.node().setFloat(prefix + name, model.get().getDOF(name));
  }
  _bind.node().setFloat(prefix + "base_x", model.get().getDOF("base_x"));
  _bind.node().setFloat(prefix + "base_y", model.get().getDOF("base_y"));
  _bind.node().setFloat(prefix + "base_z", model.get().getDOF("base_z"));
  _bind.node().setFloat(prefix + "base_yaw", model.get().getDOF("base_yaw"));
  _bind.node().setFloat(prefix + "base_pitch", model.get().getDOF("base_pitch"));
  _bind.node().setFloat(prefix + "base_roll", model.get().getDOF("base_roll"));
  _bind.node().setInt(prefix + "support_foot", (int)model.getSupportFoot());
}

void ModelService::tickDumpLogs()
{
  if (_doWriteLog)
  {
    // Open log file
    _histories.stopNamedLog(_logPath);

    _doWriteLog = false;
    _logPath = "";
  }
}

std::string ModelService::getCameraState()
{
  auto loc = getServices()->localisation;
  // Check camera state
  std::string cameraState = "No vision";

  double lastUpdate = loc->getLastVisionUpdate();
  if (lastUpdate > 0)
  {
    std::string statusVision = loc->getCameraStatus();
    double lastUpdateVision = lastUpdate / 1000.0;
    if (statusVision.find("lost") != std::string::npos)
    {
      cameraState = "Vision lost " + std::to_string(lastUpdateVision);
    }
    else
    {
      cameraState = "";
    }
  }

  return cameraState;
}

void ModelService::tickCheckLowlevelState()
{
  // Retrieve the RhAL Manager
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  _lowlevelState = "";

  // Checking for camera state
  std::string cameraState = getCameraState();
  if (cameraState != "")
  {
    _lowlevelState += cameraState;
    _lowlevelState += '\n';
  }

  // Checking for missing devices
  const auto& devs = manager->devContainer();
  for (const auto& it : devs)
  {
    if (!it.second->isPresent())
    {
      _lowlevelState += it.first + "\n";
    }
  }
}

void ModelService::tickOdometryUpdate()
{
  // Compute odometry displacement correction
  // without adding noise
  _odometry.update(_readModel, nullptr);
  // Record corrected odometry to history
  _histories.number("read:odometry_x")->pushValue(RhAL::duration_float(_timestamp), _odometry.state().x());
  _histories.number("read:odometry_y")->pushValue(RhAL::duration_float(_timestamp), _odometry.state().y());
  _histories.angle("read:odometry_yaw")->pushValue(RhAL::duration_float(_timestamp), _odometry.state().z());
}

void ModelService::tickFindTimestamp()
{
  // Retrieve the RhAL Manager
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  // Compute the max timestamp over
  // all DOF and look for communication error
  RhAL::TimePoint timestamp;
  for (const std::string& name : Leph::NamesDOF)
  {
    RhAL::ReadValueFloat value = manager->dev<RhAL::DXL>(name).position().readValue();
    if (timestamp < value.timestamp)
    {
      timestamp = value.timestamp;
    }
  }
  // Assign timestamp
  _timestamp = timestamp;
}

void ModelService::tickAssignReadDOF()
{
  // Retrieve the RhAL Manager
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  // Assign DOF values to the model
  for (const std::string& name : Leph::NamesDOF)
  {
    RhAL::ReadValueFloat value = manager->dev<RhAL::DXL>(name).position().readValue();
    _readModel.get().setDOF(name, RhAL::Deg2Rad(value.value));
  }
}

void ModelService::tickAssignReadPressure()
{
  // Retrieve the RhAL Manager
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  // Update for pressure sensors
  double left_weight = manager->dev<RhAL::PressureSensor4>("left_pressure").getWeight();
  double left_x = manager->dev<RhAL::PressureSensor4>("left_pressure").getX();
  double left_y = manager->dev<RhAL::PressureSensor4>("left_pressure").getY();
  double right_weight = manager->dev<RhAL::PressureSensor4>("right_pressure").getWeight();
  double right_x = manager->dev<RhAL::PressureSensor4>("right_pressure").getX();
  double right_y = manager->dev<RhAL::PressureSensor4>("right_pressure").getY();
  if (left_weight + right_weight > 0.0001)
  {
    _readModel.setPressure((left_weight + right_weight), left_weight / (left_weight + right_weight),
                           right_weight / (left_weight + right_weight), left_x, left_y, right_x, right_y);
  }
}

void ModelService::tickAssignReadIMU()
{
  // Retrieve the RhAL Manager
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  // Update IMU orientation in extrinsic Euler angles
  double yaw = manager->dev<RhAL::GY85>("imu").getGyroYaw();
  double pitch = manager->dev<RhAL::GY85>("imu").getPitch();
  double roll = manager->dev<RhAL::GY85>("imu").getRoll();
  // Use imu offset
  Eigen::Matrix3d imuMatrix = Eigen::AngleAxisd(yaw + _imuOffset.z(), Eigen::Vector3d::UnitZ()).toRotationMatrix() *
                              Eigen::AngleAxisd(pitch + _imuOffset.y(), Eigen::Vector3d::UnitY()).toRotationMatrix() *
                              Eigen::AngleAxisd(roll + _imuOffset.x(), Eigen::Vector3d::UnitX()).toRotationMatrix();
  if (_noIMU)
  {
    _readModel.setYaw(yaw);
  }
  else
  {
    _readModel.setOrientation(imuMatrix);
  }
}

void ModelService::tickRhIOPublish()
{
  // Export to RhIO
  Eigen::Vector3d current = _readModel.get().getPose();
  _bind.node().setFloat("x", _odometry.state().x());
  _bind.node().setFloat("y", _odometry.state().y());
  _bind.node().setFloat("theta", _odometry.state().z());
  _bind.node().setFloat("read_x", current.x());
  _bind.node().setFloat("read_y", current.y());
  _bind.node().setFloat("read_theta", current.z());
  // Publish Model to RhIO
  publishModelState("goal_", _goalModel);
  publishModelState("read_", _readModel);
  publishModelState("corrected_", _correctedModel);
}

void ModelService::tickHistoryUpdate()
{
  // Retrieve the RhAL Manager
  RhAL::StandardManager* manager = Helpers::getScheduler()->getManager();
  // Push and record history values
  for (const std::string& name : Leph::NamesDOF)
  {
    RhAL::ReadValueFloat value = manager->dev<RhAL::DXL>(name).position().readValue();
    _histories.number("read:" + name)->pushValue(RhAL::duration_float(value.timestamp), RhAL::Deg2Rad(value.value));
    _histories.number("goal:" + name)->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF(name));
    // If asked read control and power voltage
    // from lowlevel and request a new read for
    // next loop
    if (_isReadVoltages)
    {
      RhAL::ReadValueFloat volt = manager->dev<RhAL::DXL>(name).voltage().readValue();
      _histories.number("volt_power:" + name)->pushValue(RhAL::duration_float(volt.timestamp), volt.value);
      manager->dev<RhAL::DXL>(name).voltage().askRead();
    }
  }
  // Records imu values
  RhAL::ReadValueFloat imuRead = manager->dev<RhAL::GY85>("imu").getGyroYawValue();
  _histories.angle("read:imu_pitch")->pushValue(RhAL::duration_float(imuRead.timestamp),
                                         manager->dev<RhAL::GY85>("imu").getPitch());
  _histories.angle("read:imu_roll")->pushValue(RhAL::duration_float(imuRead.timestamp),
                                        manager->dev<RhAL::GY85>("imu").getRoll());
  _histories.angle("read:imu_yaw")->pushValue(RhAL::duration_float(imuRead.timestamp),
                                       manager->dev<RhAL::GY85>("imu").getGyroYaw());
  // Records pressure values
  double left_weight = manager->dev<RhAL::PressureSensor4>("left_pressure").getWeight();
  double left_x = manager->dev<RhAL::PressureSensor4>("left_pressure").getX();
  double left_y = manager->dev<RhAL::PressureSensor4>("left_pressure").getY();
  double right_weight = manager->dev<RhAL::PressureSensor4>("right_pressure").getWeight();
  double right_x = manager->dev<RhAL::PressureSensor4>("right_pressure").getX();
  double right_y = manager->dev<RhAL::PressureSensor4>("right_pressure").getY();
  _histories.number("read:left_pressure_weight")->pushValue(RhAL::duration_float(_timestamp), left_weight);
  _histories.number("read:left_pressure_x")->pushValue(RhAL::duration_float(_timestamp), left_x);
  _histories.number("read:left_pressure_y")->pushValue(RhAL::duration_float(_timestamp), left_y);
  _histories.number("read:right_pressure_weight")->pushValue(RhAL::duration_float(_timestamp), right_weight);
  _histories.number("read:right_pressure_x")->pushValue(RhAL::duration_float(_timestamp), right_x);
  _histories.number("read:right_pressure_y")->pushValue(RhAL::duration_float(_timestamp), right_y);
  // Records odometry integration
  _histories.number("read:base_x")->pushValue(RhAL::duration_float(_timestamp), _readModel.get().getDOF("base_x"));
  _histories.number("read:base_y")->pushValue(RhAL::duration_float(_timestamp), _readModel.get().getDOF("base_y"));
  _histories.number("read:base_z")->pushValue(RhAL::duration_float(_timestamp), _readModel.get().getDOF("base_z"));
  _histories.angle("read:base_yaw")->pushValue(RhAL::duration_float(_timestamp), _readModel.get().getDOF("base_yaw"));
  _histories.angle("read:base_pitch")->pushValue(RhAL::duration_float(_timestamp), _readModel.get().getDOF("base_pitch"));
  _histories.angle("read:base_roll")->pushValue(RhAL::duration_float(_timestamp), _readModel.get().getDOF("base_roll"));
  _histories.number("read:is_left_support_foot")->pushValue(
      RhAL::duration_float(_timestamp),
      (double)(_readModel.getSupportFoot() == Leph::HumanoidFixedModel::LeftSupportFoot));
  _histories.number("goal:base_x")->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF("base_x"));
  _histories.number("goal:base_y")->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF("base_y"));
  _histories.number("goal:base_z")->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF("base_z"));
  _histories.angle("goal:base_yaw")->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF("base_yaw"));
  _histories.angle("goal:base_pitch")->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF("base_pitch"));
  _histories.angle("goal:base_roll")->pushValue(RhAL::duration_float(_timestamp), _goalModel.get().getDOF("base_roll"));
  _histories.number("goal:is_left_support_foot")->pushValue(
      RhAL::duration_float(_timestamp),
      (double)(_goalModel.getSupportFoot() == Leph::HumanoidFixedModel::LeftSupportFoot));
  // Records walk enable boolean
  _histories.number("is_base_updated")->pushValue(RhAL::duration_float(_timestamp), (double)_isUpdateReadBase);
}
