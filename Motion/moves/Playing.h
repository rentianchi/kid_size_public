#pragma once

#include <rhoban_geometry/point.h>
#include <services/TeamPlayService.h>
#include "STM.h"

class Walk;
class Kick;
class Head;
class Placer;
class PlayingMove : public STM
{
public:
  struct Place
  {
    bool ok;
    rhoban_geometry::Point position;
    rhoban_utils::Angle orientation;
  };

  PlayingMove(Walk* walk, Kick* kick);
  std::string getName();

  void onStart();
  void onStop();
  void step(float elapsed);

  void localizeStep(float elapsed);
  void approachStep(float elapsed);
  void walkBallStep(float elapsed);
  void letPlayStep(float elapsed);

  virtual void enterState(std::string state);
  virtual void exitState(std::string state);

protected:
  double t;
  double backwardT;
  double localizeWalkDuration;

  bool useKickController;
  bool stopOnHandle;
  bool teamConfidence;
  double avoidRadius;
  double walkBallDistance;

  Walk* walk;
  Kick* kick;
  Head* head;
  Placer* placer;
};
