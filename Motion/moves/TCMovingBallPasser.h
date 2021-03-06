#pragma once

#include "moves/KickController.h"
#include "ApproachMove.h"
#include <strategy/KickStrategy.hpp>

class Head;
class Walk;

class TCMovingBallPasser : public KickController
{
public:
  TCMovingBallPasser(Walk* walk, Head* head);

  /// Implement Move
  virtual std::string getName() override;
  virtual void onStart() override;
  virtual void onStop() override;
  virtual void step(float elapsed) override;

  bool isRunning;
  double t;
  double kickDirection;
  bool yPositive;

  Walk* walk;
  Head* head;
};
