#pragma once

#include "services/Service.h"

#include <rhoban_utils/history/history.h>
#include <vive_provider/udp_message_manager.h>

#include <Eigen/Geometry>

/**
 * Provide interface to the data provided by rhoban vive provider
 */
class ViveService : public Service
{
public:
  ViveService();

  /**
   * Implement ElapseTick.
   */
  bool tick(double elapsed) override;

  /**
   * Return true if the current configuration of the vive is valid and messages were received
   */
  bool isActive() const;

  /**
   * Return the transformation from the field basis to the vive basis.
   * time_stamp is in micro-seconds and can be specified according to local steady_clock or according to system_clock.
   * throws an out_of_range error if status does not contain information for the tracker id
   */
  Eigen::Affine3d getFieldToVive(uint64_t time_stamp, bool system_clock = false);

  /**
   * @see getFieldToVive
   */
  Eigen::Affine3d getFieldToCamera(uint64_t time_stamp, bool system_clock = false);

  /**
   * Return the transformation from vive referential to camera referential
   */
  Eigen::Affine3d getViveToCamera() const;

  /**
   * Return the tagged positions in field referential
   */
  std::vector<Eigen::Vector3d> getTaggedPositions(uint64_t time_stamp, bool system_clock = false) const;
  /**
   * Return the position of all the trackers
   */
  std::vector<Eigen::Vector3d> getOthersTrackersPos(uint64_t time_stamp, bool system_clock = false) const;

  void setPosOffset(const Eigen::Vector3d& pos);
  void setRoll(double roll);
  void setPitch(double pitch);
  void setYaw(double yaw);

  std::string cmdVive();

  void loadLog(const std::string& path);

private:
  /**
   * Manager for vive messages
   */
  vive_provider::UDPMessageManager vive_manager;

  /**
   * RhIO binding
   */
  RhIO::Bind bind;

  double camera_x;
  double camera_y;
  double camera_z;

  double camera_roll;
  double camera_pitch;
  double camera_yaw;

  std::string tracker_serial;

  /**
   * Time difference in seconds between the clock of the computer recording vive_information and the NUC
   */
  double extra_time_offset;

  /**
   * History of poses
   */
  rhoban_utils::HistoryCollection histories;
};
