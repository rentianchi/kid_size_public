#pragma once

#include "SpecialImageHandler.hpp"

#include <thread>
#include "Filters/Pipeline.hpp"
#include "Filters/Custom/FieldBorderData.hpp"
#include "Application/Application.hpp"

#include "rhoban_utils/timing/time_stamp.h"

#include <Eigen/Core>
#include <utility>
#include <string>
#include <vector>
#include <map>

class MoveScheduler;
namespace Vision {

class LogBenchmark;
class RobocupBenchmark;

namespace Localisation {
class BallStackFilter;
class RobotFilter;
class SpeedEstimator;
}

/**
 * Robocup
 *
 * Robocup vision pipeline struture
 * is defined here using VisionReloaded
 * framework
 */
class Robocup : public Vision::Application::Application {
  friend class Vision::RobocupBenchmark; // RobocupBenchmark need an access
  friend class Vision::LogBenchmark;     // LogBenchmark need an access
private:
  /**
   * Note on mutex:
   * - globalMutex:
   *   -> Required at any time when writing data
   *   -> Required when coherency is required between different information
   *sources
   * - csMutex:
   *   -> CameraState mutex, used for informations on timestamps and camera
   *state
   * - visionMutex:
   *   -> Used for pipelines and informations coming from vision
   *
   * WARNING:
   * - When a threads locks globalMutex, this thread cannot possess lock on any
   *other
   *   mutex.
   * - It is forbidden to lock 2 specific mutex without holding the globalMutex
   */
  mutable std::mutex globalMutex;
  mutable std::mutex visionMutex;

  mutable std::mutex logMutex;

  mutable std::mutex csMutex;

  int imageDelay; // Delay between the image capture time [ms]

  // Logging
  std::vector<cv::Mat> imagesBuffer;
  std::vector<::rhoban_utils::TimeStamp> imagesTimes;
  bool logging;
  std::string logPrefix;
  ::rhoban_utils::TimeStamp endLog;

  void initImageHandlers();

  void initObservationTypes();
  
  void initRhIO();
  void publishToRhIO();
  void importFromRhIO();

  MoveScheduler *_scheduler;

public:
  // Properties for monitoring images
  std::vector<SpecialImageHandler> imageHandlers;

  // Benchmark options
  bool benchmark;
  int benchmarkDetail;

  /**
   * Initialize and start
   * the Robocup pipeline
   */
  Robocup(MoveScheduler *scheduler);

  // Create a robocup config based on configFile
  // Required for config file
  Robocup(const std::string &configFile, MoveScheduler *scheduler);

  /**
   * Initialize and start
   * the Robocup pipeline with cmd-line args
   */
  Robocup(int argc, char **argv);

  /**
   * Stop the pipeline
   */
  virtual ~Robocup();

  void run();

  virtual void init() override;
  virtual void step() override;
  virtual void finish() override;

  void startLogging(unsigned int timeMS, const std::string & logDir);
  void endLogging();

  // How many frames were captured?
  int getFrames();

  // What is the current camera status?
  std::string getCameraStatus() const;
  double getLastUpdate() const;

  void ballReset(float x, float y);

  /**
   * Clears the ball filter (no ball in it)
   */
  void ballClear();
  void robotsClear();

  /**
   * Asks the model to start logging all the low level input
   * and dumps the read data in path
   */
  void startLoggingLowLevel(const std::string path);
  /**
   * Asks the model to stop logging the low level
   */
  void stopLoggingLowLevel();
  /**
   * Tells the model to read the low level values from a log file instead than
   * from the
   * actual low level
   */
  void setLogMode(const std::string path);

  void readPipeline();
  void getUpdatedCameraStateFromPipeline();
  void loggingStep();
  void updateBallInformations();

  /// Get all goals currently stored and remove them from the list
  std::vector<cv::Point2f> stealGoals();
  
  /// Lock mutex on tags, retrieve indices and position of tags
  /// Finally clear all memory about tags
  void stealTags(std::vector<int> & indices,
                 std::vector<Eigen::Vector3d> & positions,
		 std::vector<std::pair<float, float> > & centers,
		 double * timestamp);


  //steal the observations from the visual compass
  void stealCompasses(std::vector<double> &orientations, std::vector<double> &dispersions);

  /// Get all clipping loc info currently stored and remove them from the list
  std::vector<Vision::Filters::FieldBorderData> stealClipping();

  // Apply a kick on the ball stack filter
  void applyKick(double x, double y);

  cv::Mat getRobotView(int width = 600, int height = 600);
  cv::Mat getTaggedImg();
  cv::Mat getTaggedImg(int width, int height);
  cv::Mat getRadarImg(int width, int height);

  cv::Mat getImg(const std::string &name, int wishedWidth, int wishedHeight,
                 bool gray);

  const Pipeline &getPipeline() const { return pipeline; }

  /* JSON STUFF */
  virtual Json::Value toJson() const override;
  virtual void fromJson(const Json::Value & v, const std::string & dir_name) override;
  virtual std::string getClassName() const override { return "vision_config"; }

  //TODO : move this into radar refactoring
  std::vector<cv::Point2f> keepFrontRobots(std::vector<cv::Point2f> & robots);
  
  void closeCamera();

  void resetAllTagLevels();

  /**
   * Pipeline main loop thread
   */
  std::thread *_runThread;
  bool _doRun;

  // BALL
  /// Ball position filter
  Localisation::BallStackFilter * ballStackFilter;
  //  Opponent robots filter
  Localisation::RobotFilter * robotFilter;
  /// Ball speed estimator
  Localisation::SpeedEstimator * ballSpeedEstimator;

  // Sensors and related
  Utils::CameraState * cs;
  ::rhoban_utils::TimeStamp lastTS, sourceTS;
  double timeSinceLastFrame; // in seconds

  bool ballDetected;
  std::vector<double> ballsX, ballsY, ballsRadius;

  // Estimating ball speed
  bool _firstLoop = true;

  // Connection status
  bool activeSource;

private:
  /// Detected positions for goals in "origin" basis
  std::vector<cv::Point2f> detectedGoals;

  /// Detected robots in "origin" basis
  std::vector<cv::Point2f> detectedRobots;
  
  std::vector<std::string> observationTypes;

  /// For each type of observation, the map contains a list of
  /// detected positions for the observation in "self" basis with a living time
  /// (can stay alive for more than 1 step)
  std::map<std::string, std::vector<std::pair<cv::Point2f, float>>> rememberObservations;

  /// Controls access to the goals
  mutable std::mutex goalsMutex;

  /// Controls access to the clipping
  mutable std::mutex clippingMutex;

  /// Are aruca tags used?
  bool useTags;

  //do we use the visualcompass?
  bool useVisualCompass;

  /// Indexes of the tags detected
  std::vector<int> detectedTagsIndices;
  /// Positions of the tags detected (in world frame)
  std::vector<Eigen::Vector3d> detectedTagsPositions;
  /// Positions of the center of the tags on the image (x, y), range [-1, 1]
  std::vector<std::pair<float, float> > detectedTagsCenters;
  /// timestamp of the tag detection
  double detectedTimestamp=0.0;

  /// Controls access to the tags
  mutable std::mutex tagsMutex;

  /// Controls access to the visualcompass
  mutable std::mutex compassMutex;

  //Orientations and corresponding dispersions (quality) for the visual compass
  std::vector<double> detectedOrientations;
  std::vector<double> detectedDispersions;

  std::vector<double> radarOrientations;
  std::vector<double> tmporientations;
  std::vector<double> tmpdispersions;

  /// key: featureName
  /// values: feature providers
  std::map<std::string,std::vector<std::string>> featureProviders;

  // clipping data for the localisation
  std::vector<Vision::Filters::FieldBorderData> clipping_data;
  
  /// Was robot handled at previous step
  bool wasHandled;

  /// Was robot fallen at previous step
  bool wasFallen;
};
}
