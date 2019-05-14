#include "GoalKeeper.h"
#include <math.h>

#include "Placer.h"
#include "Walk.h"
#include <services/LocalisationService.h>
//#include <services/TeamPlayService.h>
#include <services/StrategyService.h>
#include <services/DecisionService.h>
#include <robocup_referee/constants.h>
//#include <services/TeamPlayService.h>
#include "Playing.h"
#include "rhoban_utils/logging/logger.h"

static rhoban_utils::Logger logger("GoalKeeper");

using namespace robocup_referee;
using namespace rhoban_utils;
using namespace rhoban_geometry;

#define STATE_INIT "init"    // init of the goal, begining of the match
#define STATE_WAIT "wait"    // wait for the ball in danger zone (home position)
#define STATE_ALIGNBALL "align"    // align goalkeeper with ball in x position
#define STATE_ATTACK "attack"      // go to ball and shoot it out
#define STATE_STOP "stop"          // stop and take the block position
#define STATE_GOHOME "gohome"      // go back to home position (align then home)

/*
 *                         y
 *  |----------------------------------------------|
 *  |____           |      |                       |
 *  |___|           |      |                       |
 *  |  ||           |      |                       |
 *  |  ||           |      |                       |
 *  |  ||           |      0--->x                  |
 *  | a||           |      |                       |
 *  |  ||       c   |      |                       |
 *  |__||           |      |                       |
 *  |__b|           |      |                       |
 *  |               |      |                       |
 *  |----------------------------------------------|
 *
 * a = attack
 * b = attacklimit
 * c = dangerzone
 */

GoalKeeper::GoalKeeper(Walk* walk, Placer* placer) : walk(walk), placer(placer)
{
  initializeBinding();

  bind->bindNew("homeX", homeX, RhIO::Bind::PullOnly)
    ->comment("")
    ->defaultValue(-4.0);
  bind->bindNew("homeY", homeY, RhIO::Bind::PullOnly)
    ->comment("by default middle of field")
    ->defaultValue(0.0);
  
  bind->bindNew("xAttack", xAttack, RhIO::Bind::PullOnly)
      ->comment("Distance x between home and limit to attack ")
    ->defaultValue(1.5);
  bind->bindNew("yAttack", yAttack, RhIO::Bind::PullOnly)
      ->comment("Distance y between home and limit to attack")
      ->defaultValue(1.5);
  bind->bindNew("distanceAttack", distanceAttack, RhIO::Bind::PullOnly)
      ->comment("Distance x between robot and limit to attack")
      ->defaultValue(0.5);
  

  bind->bindNew("xAttackHys", xAttackHys, RhIO::Bind::PullOnly)
      ->comment("Distance x between home and limit to attack hysteresis")
      ->defaultValue(1.75);
  bind->bindNew("yAttackHys", yAttackHys, RhIO::Bind::PullOnly)
      ->comment("Distance x between home and limit to attack hysteresis")
      ->defaultValue(1.75);

  bind->bindNew("xIgnoreBall",xIgnoreBall, RhIO::Bind::PullOnly)
    ->comment("Distance x between home and limit of the danger zone")
    ->defaultValue(3.5);

  bind->bindNew("xApprox", xApprox, RhIO::Bind::PullOnly)
      ->comment("Acceptable distance x between good position and goal position")
      ->defaultValue(0.1);
  bind->bindNew("yApprox", yApprox, RhIO::Bind::PullOnly)
      ->comment("Acceptable distance x between good position and goal position")
      ->defaultValue(0.1);

  bind->bindNew("t", t, RhIO::Bind::PushOnly)->comment("Duration of the current state");

}

std::string GoalKeeper::getName()
{
  return "goal_keeper";
}

void GoalKeeper::onStart()
{
  bind->pull();
  setState(STATE_INIT);
  RhIO::Root.setFloat("/moves/placer/marginX", 0.1);
  RhIO::Root.setFloat("/moves/placer/marginY", 0.1);
 
  
}

void GoalKeeper::onStop()
{
  setState(STATE_WAIT);

}
Point GoalKeeper::home()
{
  return Point(homeX, homeY);
}


//is the ball in a zone
bool GoalKeeper::ballInZone(float xd, float yd)
{
  auto loc = getServices()->localisation;
  auto ball = loc->getBallPosField();
  auto decision = getServices()->decision;

  float lineX = loc->getOurGoalPosField().x +xd;
  // float lineY = Constants::field.goal_area_width / 2  yd;
  
  
  return decision->isBallQualityGood && ball.x < lineX && fabs(ball.y)<yd;
}

//is the ball in the danger zone
bool GoalKeeper::ballInDangerZone()
{
  return ballInZone(xIgnoreBall, Constants::field.field_width/2.0); 
}

//is the ball in the attack hys zone
bool GoalKeeper::ballInAttackHysZone()
{
  return ballInZone(xAttackHys, yAttackHys);
}


//is the ball in attack zone
bool GoalKeeper::ballInAttackZone()
{
  return ballInZone(xAttack, yAttack);
}

//Are we safe ?  (ball too far from us
bool GoalKeeper::isBallSafe()
{
  return !ballInAttackZone() && !ballInAttackHysZone() && !ballInDangerZone();
}


//if ball in Danger Zone
Point GoalKeeper::alignBallPos()
{
  auto loc = getServices()->localisation;
  auto ball = loc->getBallPosField();

  //if ball align with goals, move in front of the ball
  if(fabs(ball.y)<(Constants::field.goal_width/2.0))
    {
      return Point(homeX, (ball.y+homeY)/2);
    }
  
  else
    {
      
     
      //move to the ball proportionnaly with distance x from the goal 
      float coeffa = (Constants::field.goal_width/2.0)/
	(homeX-(xIgnoreBall-Constants::field.field_width/2.0));
      float coeffb = (Constants::field.goal_width/2.0)-coeffa*homeX;

      //if ball near our x limit

      if (ball.x<homeX)
	{
	  if (ball.y>0)
	    return Point(ball.x, Constants::field.goal_width/2.0-
			 (loc->getOurGoalPosField().x-ball.x)*coeffa-coeffb);
	  else
	    return Point(ball.x, -Constants::field.goal_width/2.0+
			 (loc->getOurGoalPosField().x-ball.x)*coeffa+coeffb);
	}
      else
	{
	       
	  if (ball.y>0)
	    return Point(homeX, Constants::field.goal_width/2.0-
			 (loc->getOurGoalPosField().x-ball.x)*coeffa-coeffb);
	  else
	    return Point(homeX, -Constants::field.goal_width/2.0+
			 (loc->getOurGoalPosField().x-ball.x)*coeffa+coeffb);
	}
      
    }

    
}

bool GoalKeeper::goodEnoughPos(Point pos, Point needed_pos)
{
  return (fabs(pos.x-needed_pos.x)<xApprox) && (fabs(pos.y-needed_pos.y)<yApprox);
}

void GoalKeeper::step(float elapsed)
{
  bind->pull();
  t += elapsed;

  

  if (state == STATE_INIT)
    {
      if(t <1.0)
	return;
      setState(STATE_GOHOME);
      logger.log("state : starting match : GO_HOME");
      return;
    }

  auto loc = getServices()->localisation;
  auto decision = getServices()->decision;
  auto pos = loc->getFieldPos();
  auto ball = loc->getBallPosField();

  if(pos.getDist(ball)<distanceAttack)
    {
      if (decision->isBallQualityGood && state!=STATE_ATTACK)
	{
	  setState(STATE_ATTACK);
	  logger.log("state : ball is near, ATTACK !");
	}
      
    }
  else
    {
      if (state==STATE_ATTACK)
	{
	  if ((!decision->isBallQualityGood || !ballInAttackHysZone()))
	    {
	      setState(STATE_ALIGNBALL);
	      logger.log("state : ball is not a danger anymore : ALIGN");
	    }
	}  
      
      else if (isBallSafe())
	{
	  if (state == STATE_GOHOME)
	    {
	      if(placer->arrived)
		{
		  setState(STATE_WAIT);
		  logger.log("state : waiting for ball in dangerZone : WAIT");
		}     
	    }
	  else
	    {
	      if (state!=STATE_WAIT)
		{
		  setState(STATE_GOHOME);
		  logger.log("state : ball is far, going back home : GO_HOME");
		}
	      
	    }
	}
      
      else if (ballInAttackZone())
	{
	  if (decision->isBallQualityGood && state!=STATE_ATTACK)
	    {
	      setState(STATE_ATTACK);
	      logger.log("state : ball in Attack Zone : ATTACK !");
	    }
	  
	}
      
      else if (state == STATE_STOP)
	{
	  Point needed_pos = alignBallPos();
	  
	  if (pos.getDist(needed_pos)>0.1)
	    {
	      setState(STATE_ALIGNBALL);
	      placer->goTo(needed_pos.x, needed_pos.y, 0);
	      logger.log("state : we are too far from optimized pos : ALIGN");
	    }
	}
      
      else if (state==STATE_ALIGNBALL)
	  {
	    if (placer->arrived)
	      {
		setState(STATE_STOP);
		logger.log("state : we are in position : STOP");
	      }
	    else
	      {
		Point needed_pos = alignBallPos();
		placer->goTo(needed_pos.x, needed_pos.y, 0);
	      }
	  }

      else
	{
	  Point needed_pos = alignBallPos();
	  
	  if (pos.getDist(needed_pos)>0.1)
	    {
	      setState(STATE_ALIGNBALL);
	      placer->goTo(needed_pos.x, needed_pos.y, 0);
	      logger.log("state : ball in DangerZone, positionning : ALIGN");
	    }
	} 
    
    }
  
  
bind->push();
  
}


 void GoalKeeper::enterState(std::string state)
 {
   bind->pull();
  auto& strategy = getServices()->strategy;
  t = 0.0;

  if (state == STATE_GOHOME)
    {
      placer->goTo(homeX, homeY, 0);
      if (placedByHand)
	{
	  placer->setDirectMode(false);
	}
      else
	{
	  placer->setDirectMode(true);
	}
      auto loc  = getServices()->localisation;
      auto pos = loc->getFieldPos();

      if (goodEnoughPos(pos, home()))
	{
	  setState(STATE_WAIT);
	}
      
      else 
	{
	  startMove("placer", 0.0);
	}
    }
  else if (state == STATE_ATTACK)
    {
      startMove(strategy->getDefaultApproach(), 0.0);
      startMove("clearing_kick_controler", 0.0);
    }
  else if (state == STATE_ALIGNBALL)
    {
      placer->setDirectMode(false);
      startMove("placer", 0.0);
    }
   else if (state == STATE_STOP)
     {
       /*setAngle("left_hip_pitch", -30);
       setAngle("right_hip_pitch", -30);
       setAngle("left_knee", 50);
       setAngle("right_knee", 50);
       setAngle("left_ankle_pitch", -25);
       setAngle("right_ankle_pitch", -25);
       setAngle("left_shoulder_roll", 15);
       setAngle("right_shoulder_roll", -15);
       setAngle("left_elbow", 0);
       setAngle("right_elbow", 0);*/
       }
  
  
 }

 void GoalKeeper::exitState(std::string state)
 {
   auto& strategy = getServices()->strategy;

   if (state == STATE_INIT)
     {
       auto loc = getServices()->localisation;
       auto pos = loc->getFieldPos();
       
       if (goodEnoughPos(pos, home()))
	 {
	   placedByHand = true;
	 }
       else
	 {
	   placedByHand = false;
	 }
     }
   else if (state == STATE_GOHOME)
     {
       stopMove("placer", 0.0);
       placer->setDirectMode(true);
     }
   else if (state == STATE_ALIGNBALL)
     {
       stopMove("placer", 0.0);
       placer->setDirectMode(true);
     }
   else if (state == STATE_STOP)
     {
      
       
     }
   else if (state == STATE_ATTACK)
     {
       stopMove(strategy->getDefaultApproach(), 0.0);
       stopMove("clearing_kick_controler", 0.0);
     }
 }
 
 
      
      
      
	  
