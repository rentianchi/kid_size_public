#include "Binding/LocalisationBinding.hpp"
#include "Binding/Robocup.hpp"

#include "CameraState/CameraState.hpp"

#include "Localisation/Ball/BallStackFilter.hpp"
#include "Localisation/Field/FeatureObservation.hpp"
#include "Localisation/Field/FieldObservation.hpp"
#include "Localisation/Field/RobotController.hpp"
#include "Localisation/Field/TagsObservation.hpp"

#include "Utils/Drawing.hpp"
#include "Utils/Interface.h"
#include "Utils/OpencvUtils.h"

#include "scheduler/MoveScheduler.h"
#include "services/DecisionService.h"
#include "services/LocalisationService.h"
#include "services/ModelService.h"
#include "services/RefereeService.h"

#include "unistd.h"

#include <hl_communication/perception.pb.h>

#include <rhoban_utils/logging/logger.h>
#include <rhoban_utils/util.h>
#include <utility>
#include <string>
#include <vector>
#include <map>
#include <cmath>
#include <cstdlib>

using namespace hl_monitoring;
using namespace rhoban_utils;
using namespace Vision::Localisation;
using namespace hl_communication;

using Vision::Utils::CameraState;

static rhoban_utils::Logger fieldLogger("RobocupFieldPF");

namespace Vision
{
LocalisationBinding::LocalisationBinding(MoveScheduler* scheduler_, Robocup* vision_binding_)
  : vision_binding(vision_binding_)
  , scheduler(scheduler_)
  , nb_particles_ff(5000)
  , robotQ(-1)
  , isGoalKeeper(false)
  , consistencyEnabled(true)
  , consistencyScore(1)
  , consistencyStepCost(0.005)
  , consistencyBadObsCost(0.02)
  , consistencyGoodObsGain(0.1)
  , consistencyResetInterval(30)
  , consistencyMaxNoise(5.0)
  , cs(new CameraState(scheduler_))
  , period(1.0)
  , maxNoiseBoost(10.0)
  , noiseBoostDuration(5)
  , isForbidden(false)
  , bind(nullptr)
  , _runThread(nullptr)
  , odometryMode(false)
{
  scheduler->getServices()->localisation->setLocBinding(this);
  field_filter = new Localisation::FieldPF();

  init();

  currTS = getNowTS();
  lastTS = currTS;
  lastFieldReset = currTS;
  lastUniformReset = currTS;

  _runThread = new std::thread(std::bind(&LocalisationBinding::run, this));
}

LocalisationBinding::~LocalisationBinding()
{
}

void LocalisationBinding::run()
{
  while (true)
  {
    step();
    if (scheduler->isFakeMode())
    {
      while (true)
      {
        // Here, we check if there is a premature exit or if enough time ha
        // been elapsed according to vision TimeStamps
        double elapsed = diffSec(currTS, getNowTS());
        bool referee_allow_playing = refereeAllowsToPlay();
        bool premature_exit = field_filter->isResetPending() && referee_allow_playing;
        if (elapsed > period || premature_exit)
          break;
        // Sleep for 10 ms
        usleep(10 * 1000);
      }
    }
    else
    {
      double elapsed = diffSec(currTS, getNowTS());
      fieldLogger.log("Step time: %lf", elapsed);
      if (elapsed < period)
      {
        int sleep_us = (int)((period - elapsed) * 1000 * 1000);
        // Sleep a total time of sleep_us by small intervals and interrupt if
        // there is a reset pending
        int count = sleep_us / 10000;
        for (int k = 0; k < count; k++)
        {
          bool referee_allow_playing = refereeAllowsToPlay();
          bool premature_exit = field_filter->isResetPending() && referee_allow_playing;
          if (premature_exit)
          {
            fieldLogger.log("Premature exit from sleep (reset pending)");
            break;
          }
          usleep(10000);
        }
      }
    }
  }
}

void LocalisationBinding::init()
{
  initRhIO();
  importFromRhIO();
  field_filter->initializeAtUniformRandom(nb_particles_ff);
}

// TODO: eventually build Image handlers

void LocalisationBinding::initRhIO()
{
  // Only bind once
  if (bind != nullptr)
  {
    return;
  }

  bind = new RhIO::Bind("localisation");

  // Init interface with RhIO
  RhIO::Root.newCommand("localisation/resetFilters", "Reset all particle filters to an uniform distribution",
                        [this](const std::vector<std::string>& args) -> std::string {
                          lastFieldReset = getNowTS();
                          currTS = lastFieldReset;
                          lastUniformReset = lastFieldReset;
                          vision_binding->ballStackFilter->clear();
                          vision_binding->clearRememberObservations = true;
                          consistencyScore = 0;
                          field_filter->askForReset();
                          return "Field have been reset";
                        });
  RhIO::Root.newCommand("localisation/bordersReset", "Reset on the borders",
                        [this](const std::vector<std::string>& args) -> std::string {
                          fieldReset(FieldPF::ResetType::Borders);
                          return "Field have been reset";
                        });
  RhIO::Root.newCommand("localisation/fallReset", "Apply a fall event on field particle filter",
                        [this](const std::vector<std::string>& args) -> std::string {
                          fieldReset(FieldPF::ResetType::Fall);
                          return "Field have been reset";
                        });
  RhIO::Root.newCommand(
      "localisation/customReset", "Reset the field particle filter at the custom position with custom noise [m,deg]",
      [this](const std::vector<std::string>& args) -> std::string {
        unsigned int k = 0;

        auto rhioNode = &(RhIO::Root.child("/localisation/field/fieldPF"));
        for (std::string item : { "customX", "customY", "customTheta", "customNoise", "customThetaNoise" })
        {
          if (args.size() > k)
          {
            rhioNode->setFloat(item, atof(args[k].c_str()));
          }
          k++;
        }
        lastFieldReset = getNowTS();
        currTS = lastFieldReset;
        consistencyScore = 1;
        field_filter->askForReset(FieldPF::ResetType::Custom);
        return "Field have been reset";
      });
  // Number of particles in the field filter
  bind->bindNew("field/nbParticles", nb_particles_ff, RhIO::Bind::PullOnly)
      ->defaultValue(nb_particles_ff)
      ->comment("Number of particles in the localisation filter");
  bind->bindNew("field/odometryMode", odometryMode, RhIO::Bind::PullOnly)
      ->defaultValue(odometryMode)
      ->comment("Is the localization based only on odometry?");
  // consistency
  bind->bindNew("consistency/enabled", consistencyEnabled, RhIO::Bind::PullOnly)
      ->defaultValue(consistencyEnabled)
      ->comment("Is consistency check enabled? (If disable, consistencyScore is not updated)");
  bind->bindNew("consistency/elapsedSinceReset", elapsedSinceReset, RhIO::Bind::PushOnly)
      ->defaultValue(0)
      ->comment("Elapsed time since last reset (from any source) [s]");
  bind->bindNew("consistency/elapsedSinceUniformReset", elapsedSinceUniformReset, RhIO::Bind::PushOnly)
      ->defaultValue(0)
      ->comment("Elapsed time since last uniform reset (from any source) [s]");
  bind->bindNew("consistency/score", consistencyScore, RhIO::Bind::PushOnly)
      ->defaultValue(consistencyScore)
      ->maximum(1.0)
      ->minimum(0.0)
      ->comment("Current consistency quality");
  bind->bindNew("consistency/stepCost", consistencyStepCost, RhIO::Bind::PullOnly)
      ->defaultValue(consistencyStepCost)
      ->comment("The reduction of consistencyScore at each step");
  bind->bindNew("consistency/badObsCost", consistencyBadObsCost, RhIO::Bind::PullOnly)
      ->defaultValue(consistencyBadObsCost)
      ->comment("The reduction of consistencyScore for each bad observation");
  bind->bindNew("consistency/goodObsGain", consistencyGoodObsGain, RhIO::Bind::PullOnly)
      ->defaultValue(consistencyGoodObsGain)
      ->comment("The increase of consistencyScore for each 'good' observation");
  bind->bindNew("consistency/resetInterval", consistencyResetInterval, RhIO::Bind::PullOnly)
      ->defaultValue(consistencyResetInterval)
      ->comment("The minimal time to wait between two consistency resets [s]");
  bind->bindNew("consistency/maxNoise", consistencyMaxNoise, RhIO::Bind::PullOnly)
      ->defaultValue(consistencyMaxNoise)
      ->comment("Noise factor at 0 consistencyScore");
  bind->bindNew("period", period, RhIO::Bind::PullOnly)
      ->defaultValue(period)
      ->maximum(30.0)
      ->minimum(0.0)
      ->comment("Period between two ticks from the particle filter");
  bind->bindNew("consistency/elapsedSinceConvergence", elapsedSinceConvergence, RhIO::Bind::PushOnly)
      ->defaultValue(0)
      ->comment("Elapsed time since last convergence or reset [s]");
  bind->bindNew("field/maxNoiseBoost", maxNoiseBoost, RhIO::Bind::PullOnly)
      ->defaultValue(maxNoiseBoost)
      ->maximum(30.0)
      ->minimum(1.0)
      ->comment("Maximal multiplier for exploration in boost mode");
  bind->bindNew("field/noiseBoostDuration", noiseBoostDuration, RhIO::Bind::PullOnly)
      ->defaultValue(noiseBoostDuration)
      ->maximum(30.0)
      ->minimum(0.0)
      ->comment("Duration of the noise boost after global reset [s]");
  bind->bindNew("debugLevel", debugLevel, RhIO::Bind::PullOnly)
      ->defaultValue(1)
      ->comment("Verbosity level for Localisation: 0 -> silent");

  RhIO::Root.newFrame("localisation/TopView", "Top view");

  // Binding Localisation items
  RobotController::bindWithRhIO();
  FeatureObservation::bindWithRhIO();
  TagsObservation::bindWithRhIO();
}

void LocalisationBinding::importFromRhIO()
{
  RobotController::importFromRhIO();
  FeatureObservation::importFromRhIO();
  TagsObservation::importFromRhIO();
  field_filter->importFromRhIO();

  bind->pull();
}

void LocalisationBinding::publishToRhIO()
{
  bind->push();

  field_filter->publishToRhIO();

  bool isStreaming = RhIO::Root.frameIsStreaming("/localisation/TopView");
  if (isStreaming)
  {
    int width = 1040;
    int height = 740;
    cv::Mat topView = getTopView(width, height);
    RhIO::Root.framePush("/localisation/TopView", topView);
  }
}

void LocalisationBinding::step()
{
  importFromRhIO();

  currTS = getNowTS();
  cs->updateInternalModel(currTS);

  elapsedSinceReset = diffSec(lastFieldReset, currTS);
  elapsedSinceUniformReset = diffSec(lastUniformReset, currTS);

  // Always steal informations from vision
  stealFromVision();

  // Get information from the referee
  bool refereeAllowTicks = refereeAllowsToPlay();

  // When the robot is penalized do not update anything, but increase reactivity
  if (!refereeAllowTicks)
  {
    lastForbidden = currTS;
    isForbidden = true;
    if (debugLevel > 0)
    {
      fieldLogger.log("Referee forbid ticks");
    }
    // Avoid having a uniform reset pending when robot is penalized or in initial phase
    field_filter->cancelPendingReset(FieldPF::ResetType::Uniform);

    FieldPF::ResetType pending_reset = field_filter->getPendingReset();
    if (pending_reset == FieldPF::ResetType::Custom)
    {
      field_filter->applyPendingReset();
    }

    importFiltersResults();
    publishToLoc();
    publishToRhIO();
    return;
  }

  // Determining if the robot is fallen
  DecisionService* decisionService = scheduler->getServices()->decision;
  if (decisionService->isFallen)
  {
    if (debugLevel > 0)
    {
      fieldLogger.log("Robot is fallen, forbidding ticks");
    }
    publishToRhIO();
    return;
  }

  FieldPF::ResetType pending_reset = field_filter->getPendingReset();
  double elapsed_since_forbidden = diffSec(lastForbidden, currTS);
  double start_without_reset_delay = 10;  //[s]: to free the robot if it is not allowed to play
  // Wait a proper reset for some time
  // (avoid starting a tick before receiving informations from 'robocup' move)
  if (isForbidden && elapsed_since_forbidden < start_without_reset_delay &&
      (pending_reset == FieldPF::ResetType::None || pending_reset == FieldPF::ResetType::Uniform))
  {
    std::ostringstream msg;
    msg << "Delaying restart of filter: "
        << "elapsed since forbidden:" << elapsed_since_forbidden << " "
        << "Pending reset: '" << FieldPF::getName(pending_reset) << "'";
    if (debugLevel > 0)
    {
      fieldLogger.log(msg.str().c_str());
    }

    importFiltersResults();
    publishToLoc();
    publishToRhIO();
    return;
  }

  isForbidden = false;

  if (debugLevel > 0)
  {
    fieldLogger.log("consistency: %d", consistencyEnabled);
  }

  // Compute observations if there is no reset pending
  ObservationVector observations;
  if (!field_filter->isResetPending() && !odometryMode)
  {
    observations = extractObservations();
  }

  // Update consistency
  if (consistencyEnabled && !odometryMode)
  {
    applyWatcher(observations);
  }
  else
  {
    consistencyScore = 1.0;
  }

  // Update filter with the provided observations
  updateFilter(observations);

  // Avoid memory leaks
  for (size_t id = 0; id < observations.size(); id++)
  {
    delete (observations[id]);
  }

  importFiltersResults();

  publishToLoc();
  publishToRhIO();
}

TimeStamp LocalisationBinding::getNowTS()
{
  if (scheduler->isFakeMode())
  {
    return vision_binding->sourceTS;
  }
  return TimeStamp::now();
}

std::vector<FeatureObservation*> LocalisationBinding::extractFeatureObservations()
{
  std::vector<FeatureObservation*> featureObservations;
  for (const auto& entry : *features)
  {
    Field::POIType poiType = entry.first;
    for (const cv::Point3f& feature_pos_in_world : entry.second)
    {
      // TODO: consider possible case of 3d features
      cv::Point2f pos_in_self = cs->getPosInSelf(cv::Point2f(feature_pos_in_world.x, feature_pos_in_world.y));
      double robotHeight = cs->getHeight();

      rhoban_geometry::PanTilt panTiltToFeature = cs->panTiltFromXY(pos_in_self, robotHeight);
      FeatureObservation* newObs = new FeatureObservation(poiType, panTiltToFeature, robotHeight);
      // Adding new observation or merging based on similarity
      bool has_similar = false;
      for (FeatureObservation* featureObs : featureObservations)
      {
        if (FeatureObservation::isSimilar(*newObs, *featureObs))
        {
          has_similar = true;
          featureObs->merge(*newObs);
        }
      }
      if (has_similar)
      {
        delete (newObs);
      }
      else
      {
        featureObservations.push_back(newObs);
      }
    }
  }

  return featureObservations;
}

std::vector<TagsObservation*> LocalisationBinding::extractTagsObservations()
{
  std::vector<TagsObservation*> tagsObservations;
  std::map<int, std::vector<Eigen::Vector3d>> tagsInSelf;
  for (size_t markerId = 0; markerId < markerIndices.size(); markerId++)
  {
    Eigen::Vector3d pos_in_world = markerPositions[markerId];
    Eigen::Vector3d pos_in_self = cs->getSelfFromWorld(pos_in_world);
    tagsInSelf[markerIndices[markerId]].push_back(pos_in_self);
  }
  for (const std::pair<int, std::vector<Eigen::Vector3d>>& entry : tagsInSelf)
  {
    int nb_obs = entry.second.size();
    // Compute mean
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const Eigen::Vector3d& pos : entry.second)
    {
      mean += pos;
    }
    mean /= nb_obs;
    // Compute stddev
    Eigen::Vector3d err2 = Eigen::Vector3d::Zero();
    for (const Eigen::Vector3d& pos : entry.second)
    {
      Eigen::Vector3d diff = pos - mean;
      err2 += diff.cwiseProduct(diff);
    }
    Eigen::Vector3d dev = (err2 / nb_obs).cwiseSqrt();
    cv::Point3f cv_pos = Utils::eigenToCV(mean);
    cv::Point3f cv_dev = Utils::eigenToCV(dev);
    tagsObservations.push_back(new TagsObservation(entry.first, cv_pos, cv_dev, cs->getHeight(), entry.second.size()));
  }
  return tagsObservations;
}

void LocalisationBinding::stealFromVision()
{
  // Declaring local unused variables to fit signature
  std::vector<std::pair<float, float>> markerCenters;
  std::vector<std::pair<float, float>> markerCentersUndistorded;
  // Stealing data
  double tagTimestamp = 0;  // Unused
  features = vision_binding->stealFeatures();
  vision_binding->stealTags(markerIndices, markerPositions, markerCenters, markerCentersUndistorded, &tagTimestamp);
  if (debugLevel > 0)
  {
    std::ostringstream oss;
    int total_observations = 0;
    for (const auto& entry : *features)
    {
      int nb_obs = entry.second.size();
      total_observations += nb_obs;
      oss << nb_obs << " " << Field::poiType2String(entry.first) << ",";
    }
    total_observations += markerPositions.size();
    oss << markerPositions.size() << " marker";
    fieldLogger.log("Nb observations stolen: %d (%s)", total_observations, oss.str().c_str());
  }
}

LocalisationBinding::ObservationVector LocalisationBinding::extractObservations()
{
  // Declaration of the vectors used
  ObservationVector fieldObservations;

  int obsId = 0;
  for (FeatureObservation* obs : extractFeatureObservations())
  {
    fieldObservations.push_back(obs);
    if (debugLevel > 0)
    {
      cv::Point3f pos;
      if (obs->getSeenDir(&pos))
      {
        fieldLogger.log("Feature %d of type %s -> pan: %lf, tilt: %lf, weight: %1lf, pos: %lf, %lf, %lf", obsId,
                        obs->getPOITypeName().c_str(), obs->panTilt.pan.getSignedValue(),
                        obs->panTilt.tilt.getSignedValue(), obs->weight, pos.x, pos.y, pos.z);
      }
      else
      {
        fieldLogger.error("Failed to find score for feature %d of type %s -> pan: %lf, tilt: %lf, weight: %1lf", obsId,
                          obs->getPOITypeName().c_str(), obs->panTilt.pan.getSignedValue(),
                          obs->panTilt.tilt.getSignedValue(), obs->weight);
      }
    }
    obsId++;
  }

  for (TagsObservation* obs : extractTagsObservations())
  {
    fieldObservations.push_back(obs);
    if (debugLevel > 0)
    {
      fieldLogger.log("Tags %d -> id: %d, pos: (%.3lf, %.3lf, %.3lf), "
                      "dev: (%.3lf, %.3lf, %.3lf), height: %lf  weight: %lf",
                      obsId, obs->id, obs->seenPos.x, obs->seenPos.y, obs->seenPos.z, obs->stdDev.x, obs->stdDev.y,
                      obs->stdDev.z, obs->robotHeight, obs->weight);
    }
    obsId++;
  }

  // Add field observation, but only if we have some other observations
  if (fieldObservations.size() > 0)
  {
    fieldObservations.push_back(new FieldObservation(isGoalKeeper));
  }

  return fieldObservations;
}

void LocalisationBinding::updateFilter(
    const std::vector<rhoban_unsorted::Observation<Localisation::FieldPosition>*>& obs)
{
  ModelService* model_service = scheduler->getServices()->model;

  // ComputedOdometry
  double odom_start = lastTS.getTimeMS() / 1000.0;
  double odom_end = currTS.getTimeMS() / 1000.0;
  double elapsed = diffSec(lastTS, currTS);
  FieldPF::ResetType pending_reset = field_filter->getPendingReset();
  // If a reset has been asked, use odometry since reset.
  // Specific case for fall_reset, we still want to use the odometry prior to the reset
  if (pending_reset != FieldPF::ResetType::None && pending_reset != FieldPF::ResetType::Fall)
  {
    // Specific case for resets, we don't want to integrate motion before reset
    odom_start = lastFieldReset.getTimeMS() / 1000.0;
  }

  Eigen::Vector3d odo = model_service->odometryDiff(odom_start, odom_end);
  cv::Point2f robotMove;
  robotMove.x = odo(0);
  robotMove.y = odo(1);
  double orientationChange = rad2deg(odo(2));
  if (std::fabs(orientationChange) > 90)
  {
    fieldLogger.warning("unlikely orientation change received from odometry: %f deg", orientationChange);
  }

  // Use a boost of noise after an uniformReset
  double noiseGain = 1;
  if (odometryMode)
  {
    noiseGain = std::pow(10, -6);
  }
  else if (elapsedSinceUniformReset < noiseBoostDuration)
  {
    double ratio = elapsedSinceUniformReset / noiseBoostDuration;
    noiseGain = maxNoiseBoost * (1 - ratio) + ratio;
    fieldLogger.log("Using noise boost gain: %lf (%lf[s] elapsed)", noiseGain, elapsedSinceUniformReset);
  }
  else if (consistencyEnabled)
  {
    noiseGain = 1 + (1 - consistencyScore) * (consistencyMaxNoise - 1);
    fieldLogger.log("Using consistency boost gain: %lf (score: %lf)", noiseGain, consistencyScore);
  }

  RobotController rc(cv2rg(robotMove), orientationChange, noiseGain);

  double max_step_time = 5;  // Avoiding to have a huge exploration which causes errors
  if (elapsed > max_step_time)
  {
    fieldLogger.warning("Large time elapsed in fieldFilter: %f [s]", elapsed);
  }
  filterMutex.lock();
  field_filter->resize(nb_particles_ff);
  field_filter->step(rc, obs, std::min(max_step_time, elapsed));
  filterMutex.unlock();

  // If we updated the filter, it is important to update lastTS for next odometry.
  // If we skipped the step, it means that there is no point in using odometry from
  // lastTS to currTS, therefore, we can safely update lastTS
  lastTS = currTS;
}

void LocalisationBinding::publishToLoc()
{
  LocalisationService* loc = scheduler->getServices()->localisation;

  // update the loc service
  cv::Point2d c = field_filter->getCenterPositionInSelf();
  Angle o = field_filter->getOrientation();

  loc->setPosSelf(Eigen::Vector3d(c.x, c.y, 0), deg2rad(o.getValue()), robotQ, consistencyScore, consistencyEnabled);

  loc->setClusters(field_filter->getPositionsFromClusters());
}

void LocalisationBinding::applyWatcher(
    const std::vector<rhoban_unsorted::Observation<Localisation::FieldPosition>*>& obs)
{
  // Apply HighLevel PF
  double stepDeltaScore = -consistencyStepCost;
  const auto& particle = field_filter->getRepresentativeParticle();
  std::vector<rhoban_unsorted::BoundedScoreObservation<FieldPosition>*> castedObservations;
  int obsId = 0;
  for (rhoban_unsorted::Observation<FieldPosition>* o : obs)
  {
    FeatureObservation* featureObs = dynamic_cast<FeatureObservation*>(o);
    // Ignore non feature observations for quality check
    if (featureObs == nullptr)
    {
      continue;
    }
    // Checking Score of the particle
    double score = featureObs->potential(particle, true);
    double minScore = featureObs->getMinScore();
    // Debug
    if (debugLevel > 0)
    {
      fieldLogger.log("Observation %d: %s -> score: %f , minScore: %f", obsId, featureObs->toStr().c_str(), score,
                      minScore);
    }
    obsId++;
    // If score <= minScore, then observation is so different from expected result
    // that there is only two possibilities:
    // 1. Vision provided a false positive
    // 2. Representative particle location is really wrong
    if (score > minScore)
    {
      stepDeltaScore += consistencyGoodObsGain;
    }
    else
    {
      stepDeltaScore -= consistencyBadObsCost;
    }
  }

  /// Update consistency score
  consistencyScore += stepDeltaScore;
  consistencyScore = std::min(1.0, std::max(0.0, consistencyScore));
  if (debugLevel > 0)
  {
    fieldLogger.log("Updating consistency: deltaStep: %f | new consistency: %f", stepDeltaScore, consistencyScore);
  }

  /// Reset of the particle filter requires several conditions
  /// - We have not reseted the filter for  long time
  /// - ConsistencyScore has reached 0
  /// - There is no reset pending on the robot
  bool resetAllowed = elapsedSinceUniformReset > consistencyResetInterval;
  bool lowConsistency = consistencyScore <= 0;
  fieldLogger.error("resetAllowed: %d, consistency: %f (elapsed since UR: %f)", resetAllowed, consistencyScore,
                    elapsedSinceUniformReset);
  if (resetAllowed && lowConsistency && !field_filter->isResetPending())
  {
    lastFieldReset = getNowTS();
    lastUniformReset = lastFieldReset;
    // consistencyScore starts at 0
    consistencyScore = 0;
    field_filter->askForReset();
    if (debugLevel > 0)
    {
      std::ostringstream msg;
      msg << "Asking for a full reset: " << std::endl;
      msg << "consistencyScore: " << consistencyScore << " robotQ: " << robotQ;
      fieldLogger.log(msg.str().c_str());
    }
  }
}

void LocalisationBinding::importFiltersResults()
{
  filterMutex.lock();
  // Robot
  robot = field_filter->getRepresentativeParticle();
  robotQ = field_filter->getRepresentativeQuality();

  filterMutex.unlock();
}

cv::Mat LocalisationBinding::getTopView(int width, int height)
{
  filterMutex.lock();
  cv::Mat img(height, width, CV_8UC3);
  field_filter->draw(img);

  filterMutex.unlock();
  return img;
}

void LocalisationBinding::fieldReset(Localisation::FieldPF::ResetType type, float x, float y, float noise, float theta,
                                     float thetaNoise)
{
  lastFieldReset = getNowTS();

  if (type == Localisation::FieldPF::ResetType::Custom)
  {
    auto rhioNode = &(RhIO::Root.child("/localisation/field/fieldPF"));
    rhioNode->setFloat("customX", x);
    rhioNode->setFloat("customY", y);
    rhioNode->setFloat("customNoise", noise);
    rhioNode->setFloat("customTheta", theta);
    rhioNode->setFloat("customThetaNoise", thetaNoise);
  }

  if (type == Localisation::FieldPF::Uniform)
  {
    lastUniformReset = lastFieldReset;
    consistencyScore = 0;
  }
  else if (type != Localisation::FieldPF::ResetType::Fall)
  {
    consistencyScore = 1;
  }
  field_filter->askForReset(type);
}

bool LocalisationBinding::refereeAllowsToPlay()
{
  // On fake mode, always allow robot to play
  if (scheduler->isFakeMode() || !scheduler->getMove("robocup")->isRunning())
    return true;

  RefereeService* referee = scheduler->getServices()->referee;
  bool allowedPhase = referee->isPlacingPhase() || referee->isFreezePhase();
  bool penalized = referee->isPenalized() && !referee->isServingPenalty();
  return referee->isPlaying() || (allowedPhase && !penalized);
}

}  // namespace Vision
