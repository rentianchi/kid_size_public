#include <rhoban_utils/logging/logger.h>
#include "DecisionService.h"
#include "LocalisationService.h"
#include "RefereeService.h"
#include <scheduler/MoveScheduler.h>
#include <RhIO.hpp>

static rhoban_utils::Logger out("referee");
using namespace robocup_referee;

RefereeService::RefereeService()
  : timeSincePlaying(0)
  , timeSinceGamePlaying(0)
  , remaining(0)
  , id(0)
  , teamId(0)
  , alive(0)
  , force(false)
  , playing(false)
  , gamePlaying(false)
  , wasPenalized(false)
  , dumpGameState(false)
  , startPlayingDuration(15.0)
  , timeSinceGameInterruption(-1)
  , lastGameInterruptionType(0)
{
  _state = "";

  bind = new RhIO::Bind("referee");

  bind->bindNew("state", _state, RhIO::Bind::PushOnly)->comment("State of the Referee services");

  bind->bindNew("id", id, RhIO::Bind::PullOnly)->comment("The robot ID")->defaultValue(id)->persisted(true);
  bind->bindNew("teamId", teamId, RhIO::Bind::PullOnly)->comment("The team ID")->defaultValue(teamId)->persisted(true);
  bind->bindNew("force", force, RhIO::Bind::PullOnly)->comment("Force the playing to true")->defaultValue(force);
  bind->bindNew("startPlayingDuration", startPlayingDuration, RhIO::Bind::PullOnly)
      ->comment("Duration of the start playing phase")
      ->defaultValue(startPlayingDuration);
  bind->bindNew("timeSincePlaying", timeSincePlaying, RhIO::Bind::PushOnly)
      ->comment("Time elapsed since playing")
      ->defaultValue(timeSincePlaying);
  bind->bindNew("timeSinceGamePlaying", timeSinceGamePlaying, RhIO::Bind::PushOnly)
      ->comment("Time elapsed since game playing")
      ->defaultValue(timeSinceGamePlaying);
  bind->bindNew("timeSinceGameInterruption", timeSinceGameInterruption, RhIO::Bind::PushOnly)
      ->comment("Time elapsed since game interruption [s], 0 if there has not been any game interruption since start")
      ->defaultValue(timeSinceGameInterruption);
  bind->bindNew("lastGameInterruptionType", lastGameInterruptionType, RhIO::Bind::PushOnly)
      ->comment("Last game interruption type, 0 if there has not been any game interruption")
      ->defaultValue(lastGameInterruptionType);
  bind->bindNew("dumpGameState", dumpGameState, RhIO::Bind::PullOnly)
      ->comment("Activate dump of game status")
      ->defaultValue(dumpGameState);
  bind->bindNew("ipFilter", ipFilter, RhIO::Bind::PullOnly)
      ->defaultValue("")
      ->comment("IP adress to filter for game controller (can be empty for no filtr)");

  bind->bindNew("alive", alive, RhIO::Bind::PullOnly)->comment("Referee alive status")->defaultValue(2);

  bind->bindFunc("infoPlaying", "Are we playing?", &RefereeService::cmdPlaying, *this);
  bind->bindNew("throwIn", throwIn, RhIO::Bind::PushOnly)->defaultValue(false);

  bind->bindNew("canScore", canScore, RhIO::Bind::PushOnly)->defaultValue(true);

  bind->pull();

  start();
}

RefereeService::~RefereeService()
{
  delete bind;
}

bool RefereeService::tick(double elapsed)
{
  bind->pull();
  checkPlaying();
  if (isPlaying())
  {
    timeSincePlaying += elapsed;
  }
  else
  {
    timeSincePlaying = 0;
  }
  if (isGamePlaying())
  {
    timeSinceGamePlaying += elapsed;
  }
  else
  {
    timeSinceGamePlaying = 0;
  }

  const GameState& gs = getGameState();
  // Treat game interruptions
  if (isGameInterruption())
  {
    timeSinceGameInterruption = 0;
    lastGameInterruptionType = gs.getSecGameState();
    lastGameInterruptionTeam = gs.getSecondaryTeam();
  }
  else if (lastGameInterruptionType != 0)
  {
    timeSinceGameInterruption += elapsed;
  }
  // Removing penalized robots from shared localization
  auto loc = getServices()->localisation;
  for (int k = 0; k < gs.getNbTeam(); k++)
  {
    auto team = gs.getTeam(k);
    if (team.getTeamNumber() == teamId)
    {
      for (int robot_id = 0; robot_id < team.getNbRobots(); robot_id++)
      {
        if (robot_id != id && team.getRobot(robot_id).getPenalty() != Constants::PENALTY_NONE)
        {
          loc->removeSharedOpponentProvider(robot_id);
        }
      }
    }
  }

  // Checking conditions to enter in the "can't score" state
  if (myTeamKickOff())
  {
    if (getGameState().getActualGameState() == Constants::STATE_SET)
    {
      if (canScore == true)
      {
        out.log("Current game in SET, setting canScore to false");
      }
      canScore = false;
    }
  }

  if (isIndirectGameInterruption() && gs.getSecondaryTeam() == teamId)
  {
    if (canScore == true)
    {
      out.log("Indirect game interruption for us, setting canScore to false");
    }
    canScore = false;
  }

  // Checking conditions to enter the "can score" test
  if (!canScore)
  {
    DecisionService* decision = getServices()->decision;

    if (decision->hasMateKickedRecently || getScheduler()->getMove("kick")->isRunning())
    {
      out.log("We kicked recently or are now kicking, setting canScore to true");
      canScore = true;
    }
  }

  setState(teamId, id, alive);

  if (dumpGameState)
  {
    std::cout << &gs << std::endl;
  }

  setTextualState();

  bind->push();

  return true;
}

bool RefereeService::isIPValid(std::string ip)
{
  if (ipFilter != "")
  {
    // If an ip filter is specified, we reject the messages coming from other game controllers
    if (ip != ipFilter)
    {
      out.warning("Rejecting message from game controller %s (filter: only accepting from %s)", ip.c_str(),
                  ipFilter.c_str());
      return false;
    }
  }

  return true;
}

int RefereeService::gameTime()
{
  auto gameState = getGameState();

  // No recent update from the game controller
  if (gameState.getLastUpdate() < 500)
  {
    return 600 - remaining;
  }
  else
  {
    return -1;
  }
}

const std::string& RefereeService::getState() const
{
  return _state;
}

bool RefereeService::myTeamKickOff()
{
  const auto& gs = getGameState();
  int kickingTeamId = gs.getKickOffTeam();
  return kickingTeamId == teamId || kickingTeamId < 0;
}

bool RefereeService::isDroppedBall()
{
  const auto& gs = getGameState();
  int kickingTeamId = gs.getKickOffTeam();
  return kickingTeamId < 0;
}

bool RefereeService::isGameInterruption()
{
  const auto& gs = getGameState();
  switch (gs.getSecGameState())
  {
    case Constants::STATE2_DIRECT_FREE_KICK:
    case Constants::STATE2_INDIRECT_FREE_KICK:
    case Constants::STATE2_PENALTY_KICK:
    case Constants::STATE2_CORNER_KICK:
    case Constants::STATE2_GOAL_KICK:
    case Constants::STATE2_THROW_IN:
      return true;
  }
  return false;
}

bool RefereeService::isIndirectGameInterruption()
{
  const auto& gs = getGameState();
  switch (gs.getSecGameState())
  {
    case Constants::STATE2_INDIRECT_FREE_KICK:
    case Constants::STATE2_THROW_IN:
      return true;
  }
  return false;
}

bool RefereeService::isRecentGameInterruption()
{
  // We are basing the answers on time from the last free kick, but this
  // may be available directly in the referee in the future, see
  // https://github.com/RoboCup-Humanoid-TC/GameController/issues/19
  return lastGameInterruptionType != 0 && timeSinceGameInterruption < 10;
}

bool RefereeService::myTeamGameInterruption()
{
  const auto& gs = getGameState();
  return lastGameInterruptionTeam == teamId;
}

bool RefereeService::isThrowIn()
{
  throwIn = lastGameInterruptionType == Constants::STATE2_THROW_IN;
  return throwIn;
}

bool RefereeService::isPenalized()
{
  return isPenalized(id);
}

bool RefereeService::isPenalized(int id)
{
  return getRemainingPenaltyTime(id) >= 0;
}

int RefereeService::getRemainingPenaltyTime(int id)
{
  const GameState& gameState = getGameState();
  for (int k = 0; k < gameState.getNbTeam(); k++)
  {
    const Team& team = gameState.getTeam(k);
    if (team.getTeamNumber() == teamId)
    {
      int idz = id - 1;
      if (idz >= 0 && idz < team.getNbRobots())
      {
        auto robot = team.getRobot(idz);
        int penalty = robot.getPenalty();
        if (robot.getRedCardCount() > 0 || penalty == Constants::PENALTY_SUBSTITUTE)
        {
          return std::numeric_limits<int>::max();
        }
        if (penalty != 0)
        {
          return robot.getSecsTillUnpenalised();
        }
      }
    }
  }

  return -1;
}

bool RefereeService::isServingPenalty()
{
  int secs_remaining = getRemainingPenaltyTime(id);
  return secs_remaining >= 0 && secs_remaining < 30;
}

bool RefereeService::isOpponentKickOffStart()
{
  auto gameState = getGameState();

  // No recent update from the game controller
  if (gameState.getLastUpdate() < 500)
  {
    // The game is running for less than 10s
    if (timeSinceGamePlaying < 10)
    {
      // XXX: It is possible that NO team have the kick off in case of dropped ball
      int team = gameState.getKickOffTeam();
      // We are not the kick off team
      if (team >= 0 && team != teamId)
      {
        return true;
      }
    }
  }

  return false;
}

bool RefereeService::isPlaying()
{
  return playing;
}

bool RefereeService::isGamePlaying()
{
  return gamePlaying;
}

bool RefereeService::isInitialPhase()
{
  if (force)
    return false;
  return getGameState().getActualGameState() == Constants::STATE_INITIAL;
}

bool RefereeService::isPlacingPhase()
{
  if (force)
    return false;
  return getGameState().getActualGameState() == Constants::STATE_READY;
}

bool RefereeService::isFreezePhase()
{
  if (force)
    return false;
  return getGameState().getActualGameState() == Constants::STATE_SET ||
         (isGameInterruption() && getGameState().getSecondaryMode() != 1);
}

bool RefereeService::isFinishedPhase()
{
  if (force)
    return false;
  return getGameState().getActualGameState() == Constants::STATE_FINISHED;
}

void RefereeService::checkPlaying()
{
  bind->pull();

  // This is the force flag
  if (force)
  {
    gamePlaying = playing = true;
    return;
  }

  // Game state
  auto gameState = getGameState();
  remaining = gameState.getEstimatedSecs();

  // If there were no update from the game controller the last
  // five seconds, suppose we are playing
  if (gameState.getLastUpdate() > 500)
  {
    gamePlaying = playing = true;
    return;
  }

  // Checking the game state
  if (gameState.getActualGameState() != Constants::STATE_PLAYING)
  {
    wasPenalized = false;
    gamePlaying = playing = false;
    return;
  }

  gamePlaying = true;

  // Checking if our team is here and if the current player is not penalized
  for (int k = 0; k < gameState.getNbTeam(); k++)
  {
    auto team = gameState.getTeam(k);
    if (team.getTeamNumber() == teamId)
    {
      int idz = id - 1;
      if (idz >= 0 && idz < team.getNbRobots())
      {
        auto robot = team.getRobot(idz);
        if (robot.getPenalty() != 0)
        {
          wasPenalized = true;
          playing = false;
          return;
        }
      }
    }
  }

  playing = true;
}

double RefereeService::getTimeSincePlaying()
{
  if (!isPlaying())
  {
    return 0;
  }
  else
  {
    return timeSincePlaying;
  }
}

bool RefereeService::hasStartedPlayingRecently()
{
  return isPlaying() && (timeSincePlaying < startPlayingDuration);
}

std::string RefereeService::cmdPlaying()
{
  if (isPlaying())
  {
    std::stringstream ss;
    ss << "We are playing since " << timeSincePlaying << "s." << std::endl;
    ss << "The game is playing since " << timeSinceGamePlaying << "s." << std::endl;
    ss << "Referee time: " << remaining << "." << std::endl;
    ss << "Referee last update: " << (getGameState().getLastUpdate() / 100.0) << "." << std::endl;

    if (isOpponentKickOffStart())
    {
      ss << "Opponent kick off, ball should not be touched.";
    }

    return ss.str();
  }
  else if (isInitialPhase())
  {
    return "We are in initial phase";
  }
  else if (isPlacingPhase())
  {
    return "We are in placing phase";
  }
  else if (isFreezePhase())
  {
    return "We are in the freeze phase";
  }
  else if (isPenalized(id))
  {
    return "I am penalized.";
  }
  else
  {
    return "We are not playing.";
  }
}

int RefereeService::getSecondaryTime()
{
  const auto& gs = getGameState();
  return gs.getSecondarySecs();
}

void RefereeService::setTextualState()
{
  if (isPlaying())
  {
    if (hasStartedPlayingRecently())
    {
      _state = "Let play ";
    }
    else
    {
      _state = "Playing ";
    }
  }
  else if (isInitialPhase())
  {
    _state = "Initial ";
  }
  else if (isPlacingPhase())
  {
    _state = "Placing ";
  }
  else if (isFreezePhase())
  {
    _state = "Freeze ";
  }
  else if (isPenalized(id))
  {
    _state = "Penalized ";
  }
  else if (isThrowIn())
  {
    _state = "ThrowIn";
  }
  else
  {
    _state = "Not playing ";
  }
  _state += std::to_string(getGameState().getLastUpdate() / 100.0);
}
