#pragma once

#include <thread>
#include "moves/KickController.h"
#include <rhoban_utils/control/control.h>
#include <services/TeamPlayService.h>

#include "rhoban_csa_mdp/core/policy.h"
#include "rhoban_geometry/point.h"
#include "rhoban_utils/angle.h"
#include <strategy/KickStrategy.hpp>
#include <strategy/KickValueIteration.hpp>

class Walk;

class MCKickController : public KickController
{
public:
  MCKickController();
  std::string getName();

  void onStart();
  void onStop();
  void execute();

  void step(float elapsed);

  KickValueIteration kickValueIteration;

  // Avoid the opponents ?
  bool avoidOpponents;

protected:
  KickStrategy strategy;
  std::string strategyFile;
  KickStrategy::Action action;
  std::thread* thread;

  bool forceUpdate;
  rhoban_geometry::Point lastUpdateBall;

  // The collection of available kicks
  csa_mdp::KickModelCollection kmc;

  // Updating the target action
  void updateAction();

  std::string cmdReloadStrategy();

  bool useMonteCarlo;
  bool shouldReload;
  bool enableLateral;

  KickStrategy::Action _bestAction;

  bool available;
};
