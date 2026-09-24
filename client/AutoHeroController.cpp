/*
 * AutoHeroController.cpp, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#include "StdInc.h"
#include "AutoHeroController.h"

#include "CPlayerInterface.h"
#include "PlayerLocalState.h"
#include "GameEngine.h"

#include "../lib/autoheroes/AutoHeroConfig.h"
#include "../lib/ConditionalWait.h"
#include "../lib/callback/CCallback.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/mapObjects/CGDwelling.h"
#include "../lib/mapObjects/IOwnableObject.h"
#include "../lib/mapObjects/army/CArmedInstance.h"
#include "../lib/pathfinder/CGPathNode.h"

namespace
{

bool isMvpCollectTarget(const CGObjectInstance * target)
{
	if(!target)
		return false;

	// Stage 6 MVP intentionally starts with objects that can be collected without
	// choosing a battle target or changing ownership of strategic objects.
	switch(target->ID)
	{
	case Obj::RESOURCE:
	case Obj::RANDOM_RESOURCE:
	case Obj::CAMPFIRE:
	case Obj::FLOTSAM:
	case Obj::LEAN_TO:
	case Obj::MYSTICAL_GARDEN:
	case Obj::SEA_CHEST:
	case Obj::WATER_WHEEL:
	case Obj::WINDMILL:
	case Obj::WAGON:
	case Obj::CORPSE:
	case Obj::SHIPWRECK_SURVIVOR:
	case Obj::ARTIFACT:
	case Obj::RANDOM_ART:
	case Obj::RANDOM_TREASURE_ART:
	case Obj::RANDOM_MINOR_ART:
	case Obj::RANDOM_MAJOR_ART:
	case Obj::RANDOM_RELIC_ART:
	case Obj::SPELL_SCROLL:
		return true;
	default:
		return false;
	}
}

bool isBattleAction(EPathNodeAction action)
{
	return action == EPathNodeAction::BATTLE || action == EPathNodeAction::TELEPORT_BATTLE;
}

bool isTeleportAction(EPathNodeAction action)
{
	return action == EPathNodeAction::TELEPORT_NORMAL
		|| action == EPathNodeAction::TELEPORT_BLOCKING_VISIT
		|| action == EPathNodeAction::TELEPORT_BATTLE;
}

bool isSecondarySkillLearningTarget(const CGObjectInstance * target)
{
	if(!target)
		return false;

	return target->ID == Obj::WITCH_HUT
		|| target->ID == Obj::UNIVERSITY
		|| target->ID == Obj::SCHOLAR;
}

bool isLevelUpTarget(const CGObjectInstance * target, bool allowSecondarySkillLearning)
{
	if(!target)
		return false;

	if(!allowSecondarySkillLearning && isSecondarySkillLearningTarget(target))
		return false;

	switch(target->ID)
	{
	case Obj::STAR_AXIS:
	case Obj::SCHOLAR:
	case Obj::SCHOOL_OF_MAGIC:
	case Obj::SCHOOL_OF_WAR:
	case Obj::GARDEN_OF_REVELATION:
	case Obj::MARLETTO_TOWER:
	case Obj::MERCENARY_CAMP:
	case Obj::LEARNING_STONE:
	case Obj::ARENA:
	case Obj::LIBRARY_OF_ENLIGHTENMENT:
	case Obj::SHRINE_OF_MAGIC_INCANTATION:
	case Obj::SHRINE_OF_MAGIC_GESTURE:
	case Obj::SHRINE_OF_MAGIC_THOUGHT:
	case Obj::WITCH_HUT:
		return true;
	default:
		return false;
	}
}

bool isCaptureTarget(const CGObjectInstance * target, PlayerColor player)
{
	if(!target || target->ID == Obj::HERO || target->ID == Obj::TOWN)
		return false;

	if(dynamic_cast<const IOwnableObject *>(target) == nullptr)
		return false;

	return target->getOwner() != player;
}

bool isRecruitTarget(const CGObjectInstance * target, PlayerColor player)
{
	const auto * dwelling = dynamic_cast<const CGDwelling *>(target);
	if(!dwelling || dwelling->ID == Obj::WAR_MACHINE_FACTORY)
		return false;

	return dwelling->ID == Obj::REFUGEE_CAMP || dwelling->getOwner() == player;
}

}

AutoHeroController::AutoHeroController(CPlayerInterface & owner_)
	: owner(owner_)
{
}

const CGHeroInstance * AutoHeroController::activeHero() const
{
	if(!activeHeroId)
		return nullptr;
	return owner.cb ? owner.cb->getHero(*activeHeroId) : nullptr;
}

AutoHeroes::DecisionPolicy AutoHeroController::decisionPolicy() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).decisionPolicy : AutoHeroes::DecisionPolicy::ASK_HUMAN;
}

bool AutoHeroController::allowsSecondarySkillLearning() const
{
	const auto * hero = activeHero();
	return hero && AutoHeroes::readHeroConfig(hero->id).allowSecondarySkillLearning;
}

bool AutoHeroController::allowsAction(AutoHeroes::Action action) const
{
	const auto * hero = activeHero();
	return hero && AutoHeroes::readHeroConfig(hero->id).allows(action);
}

const CGHeroInstance * AutoHeroController::currentHero() const
{
	return activeHero();
}

int AutoHeroController::goldReserve() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).goldReserve : 0;
}

AutoHeroes::RecruitmentScope AutoHeroController::recruitmentScope() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).recruitmentScope : AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY;
}

int AutoHeroController::maxForeignFactionSlots() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).maxForeignFactionSlots : 0;
}

bool AutoHeroController::shouldAutoFight(const CGHeroInstance * hero) const
{
	if(!running || !hero || !activeHeroId || hero->id != *activeHeroId || !activeAction || *activeAction != AutoHeroes::Action::FIGHT_NEUTRALS)
		return false;

	return AutoHeroes::readHeroConfig(hero->id).combatPolicy != AutoHeroes::CombatPolicy::DISABLED;
}

bool AutoHeroController::isAutoBattleActive() const
{
	return running && activeAction && *activeAction == AutoHeroes::Action::FIGHT_NEUTRALS;
}

void AutoHeroController::onBattleFinished()
{
	if(!running)
		return;

	waitingForMovement = false;
	waitingForDialog = false;
	ENGINE->dispatchMainThread([this]()
	{
		if(running)
			process();
	});
}

void AutoHeroController::onDialogResolved()
{
	if(!running)
		return;
	waitingForDialog = false;
}

bool AutoHeroController::start(bool endTurnWhenFinished)
{
	if(running)
	{
		if(endTurnWhenFinished)
		{
			endTurnAfterRun = true;
			logGlobal->info("AutoHeroes v0.8: End Turn requested while controller is running; turn will end after automation");
		}
		return true;
	}

	if(!owner.makingTurn || owner.isHeroMoving() || owner.showingDialog->isBusy())
	{
		logGlobal->warn("AutoHeroes v0.8: cannot start while turn, movement or dialog state is busy");
		return false;
	}

	heroQueue.clear();
	for(const auto * hero : owner.localState->getWanderingHeroes())
	{
		if(!hero || hero->isGarrisoned() || hero->movementPointsRemaining() <= 100)
			continue;
		if(!AutoHeroes::isHeroEnabled(hero->id))
			continue;

		heroQueue.push_back(hero->id);
	}

	if(heroQueue.empty())
	{
		logGlobal->info("AutoHeroes v0.8: no enabled hero with movement points is available");
		return false;
	}

	running = true;
	waitingForMovement = false;
	waitingForDialog = false;
	endTurnAfterRun = endTurnWhenFinished;
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;
	activeAction.reset();
	logGlobal->info("AutoHeroes v1.0: starting local controller for %d configured heroes%s", static_cast<int>(heroQueue.size()), endTurnAfterRun ? " before End Turn" : "");
	process();
	return true;
}

void AutoHeroController::cancel()
{
	if(running)
		logGlobal->info("AutoHeroes v0.8: local controller stopped");

	running = false;
	waitingForMovement = false;
	waitingForDialog = false;
	endTurnAfterRun = false;
	heroQueue.clear();
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;
	activeAction.reset();
}

void AutoHeroController::update()
{
	if(!running || waitingForMovement)
		return;

	if(waitingForDialog)
	{
		if(owner.showingDialog->isBusy())
			return;
		waitingForDialog = false;
	}

	process();
}

void AutoHeroController::onHeroMovementFinished(const CGHeroInstance * hero)
{
	if(!running || !waitingForMovement || !activeHeroId || !hero || hero->id != *activeHeroId)
		return;

	waitingForMovement = false;

	const bool moved = hero->visitablePos() != movementStartPosition
		|| hero->movementPointsRemaining() < movementStartPoints;
	if(!moved)
	{
		logGlobal->warn("AutoHeroes v0.8: hero %s did not make progress; skipping it to avoid a loop", hero->getNameTextID());
		advanceHero();
		return;
	}

	if(owner.showingDialog->isBusy())
	{
		waitingForDialog = true;
		return;
	}

	process();
}

void AutoHeroController::finishRun()
{
	const bool shouldEndTurn = endTurnAfterRun;

	running = false;
	waitingForMovement = false;
	waitingForDialog = false;
	endTurnAfterRun = false;
	heroQueue.clear();
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;

	logGlobal->info("AutoHeroes v0.8: local controller finished%s", shouldEndTurn ? "; continuing End Turn" : "; human turn remains active");
	owner.onAutoHeroesFinished(shouldEndTurn);
}

void AutoHeroController::advanceHero()
{
	activeHeroId.reset();
	activeAction.reset();
	stepsForCurrentHero = 0;
	++heroQueueIndex;
}

void AutoHeroController::process()
{
	if(!running || waitingForMovement)
		return;

	if(!owner.makingTurn)
	{
		cancel();
		return;
	}

	if(owner.showingDialog->isBusy())
	{
		waitingForDialog = true;
		return;
	}

	while(heroQueueIndex < heroQueue.size())
	{
		if(!activeHeroId)
			activeHeroId = heroQueue[heroQueueIndex];

		const CGHeroInstance * hero = activeHero();
		if(!hero || hero->isGarrisoned() || hero->movementPointsRemaining() <= 100 || !AutoHeroes::isHeroEnabled(hero->id))
		{
			advanceHero();
			continue;
		}

		// Safety valve for maps that keep producing the same revisitable target.
		if(stepsForCurrentHero >= 24)
		{
			logGlobal->warn("AutoHeroes v0.8: step limit reached for hero %s", hero->getNameTextID());
			advanceHero();
			continue;
		}

		if(startNextAction(hero))
			return;

		logGlobal->info("AutoHeroes v1.0: no configured target found for hero %s", hero->getNameTextID());
		advanceHero();
	}

	logGlobal->info("AutoHeroes v1.0: configured heroes finished");
	finishRun();
}

bool AutoHeroController::startNextAction(const CGHeroInstance * hero)
{
	const AutoHeroes::HeroConfig config = AutoHeroes::readHeroConfig(hero->id);

	for(const auto action : config.priority)
	{
		if(!config.allows(action))
			continue;

		std::optional<int3> destination;
		bool allowDestinationBattle = false;
		switch(action)
		{
		case AutoHeroes::Action::COLLECT_RESOURCES:
			destination = findCollectTarget(hero, config);
			break;
		case AutoHeroes::Action::LEVEL_UP:
			destination = findLevelTarget(hero, config);
			break;
		case AutoHeroes::Action::RECRUIT_CREATURES:
			destination = findRecruitTarget(hero, config);
			break;
		case AutoHeroes::Action::EXPLORE:
			destination = findExploreTarget(hero, config);
			break;
		case AutoHeroes::Action::CAPTURE_OBJECTS:
			destination = findCaptureTarget(hero, config);
			break;
		case AutoHeroes::Action::FIGHT_NEUTRALS:
			destination = findFightTarget(hero, config);
			allowDestinationBattle = destination.has_value();
			break;
		}

		if(destination)
		{
			activeAction = action;
			if(startMovement(hero, *destination, allowDestinationBattle))
				return true;
			activeAction.reset();
		}
	}

	return false;
}

bool AutoHeroController::startMovement(const CGHeroInstance * hero, const int3 & destination, bool allowDestinationBattle)
{
	if(!hero || destination == hero->visitablePos())
		return false;

	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return false;

	CGPath path;
	if(!paths->getPath(path, destination, EPathfindingLayer::AUTO))
		return false;

	if(!path.hasNextNode() || path.nextNode().turns != 0 || !pathIsSafeForMvp(hero, path, destination, allowDestinationBattle))
		return false;

	const CGPathNode * destinationNode = paths->getPathInfo(destination);
	const int destinationTurns = destinationNode ? destinationNode->turns : -1;

	owner.localState->setPath(hero, path);
	owner.localState->setSelection(hero);

	movementStartPosition = hero->visitablePos();
	movementStartPoints = hero->movementPointsRemaining();
	++stepsForCurrentHero;
	waitingForMovement = true;

	logGlobal->info("AutoHeroes v1.0: moving hero %s from %s to %s (MP=%d, destination turns=%d)", hero->getNameTextID(), movementStartPosition.toString(), destination.toString(), movementStartPoints, destinationTurns);
	owner.moveHero(hero, path);

	if(!owner.isHeroMoving())
	{
		waitingForMovement = false;
		return false;
	}

	return true;
}

std::optional<int3> AutoHeroController::findCollectTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return std::nullopt;

	const int3 mapSize = owner.cb->getMapSize();
	std::optional<int3> best;
	float bestCost = std::numeric_limits<float>::max();

	for(int z = 0; z < mapSize.z; ++z)
	{
		for(int x = 0; x < mapSize.x; ++x)
		{
			for(int y = 0; y < mapSize.y; ++y)
			{
				const int3 tile(x, y, z);
				if(!owner.cb->isVisible(tile))
					continue;

				for(const auto * object : owner.cb->getVisitableObjs(tile))
				{
					if(!isMvpCollectTarget(object))
						continue;
					if(object->wasVisited(hero))
						continue;

					const int3 destination = object->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable() || node->turns != 0 || node->cost >= bestCost)
						continue;

					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					best = destination;
					bestCost = node->cost;
				}
			}
		}
	}

	return best;
}

std::optional<int3> AutoHeroController::findLevelTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return std::nullopt;

	const int3 mapSize = owner.cb->getMapSize();
	std::optional<int3> best;
	int bestTurns = std::numeric_limits<int>::max();
	float bestCost = std::numeric_limits<float>::max();

	for(int z = 0; z < mapSize.z; ++z)
		for(int x = 0; x < mapSize.x; ++x)
			for(int y = 0; y < mapSize.y; ++y)
			{
				const int3 tile(x, y, z);
				if(!owner.cb->isVisible(tile))
					continue;

				for(const auto * object : owner.cb->getVisitableObjs(tile))
				{
					if(!isLevelUpTarget(object, config.allowSecondarySkillLearning) || object->wasVisited(hero))
						continue;

					if((object->ID == Obj::SCHOOL_OF_MAGIC || object->ID == Obj::SCHOOL_OF_WAR)
						&& owner.cb->getResourceAmount(EGameResID::GOLD) - 1000 < config.goldReserve)
						continue;

					const int3 destination = object->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;

					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					if(node->turns < bestTurns || (node->turns == bestTurns && node->cost < bestCost))
					{
						best = destination;
						bestTurns = node->turns;
						bestCost = node->cost;
					}
				}
			}

	return best;
}

std::optional<int3> AutoHeroController::findRecruitTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	const auto paths = owner.getPathsInfo(hero);
	const auto player = owner.cb->getPlayerID();
	if(!paths || !player)
		return std::nullopt;

	auto resources = owner.cb->getResourceAmount();
	resources[EGameResID::GOLD] = std::max(0, resources[EGameResID::GOLD] - config.goldReserve);

	const int3 mapSize = owner.cb->getMapSize();
	std::optional<int3> best;
	int bestTurns = std::numeric_limits<int>::max();
	float bestCost = std::numeric_limits<float>::max();

	for(int z = 0; z < mapSize.z; ++z)
		for(int x = 0; x < mapSize.x; ++x)
			for(int y = 0; y < mapSize.y; ++y)
			{
				const int3 tile(x, y, z);
				if(!owner.cb->isVisible(tile))
					continue;

				for(const auto * object : owner.cb->getVisitableObjs(tile))
				{
					if(!isRecruitTarget(object, *player))
						continue;

					const auto * dwelling = dynamic_cast<const CGDwelling *>(object);
					bool useful = false;
					for(const auto & level : dwelling->creatures)
					{
						if(level.first == 0 || level.second.empty())
							continue;
						const CreatureID creature = level.second.back();
						if(config.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY
							&& creature.toCreature()->getFactionID() != hero->getFactionID())
							continue;
						if(!hero->getSlotFor(creature).validSlot())
							continue;
						if(creature.toCreature()->maxAmount(resources) <= 0)
							continue;
						useful = true;
						break;
					}
					if(!useful)
						continue;

					const int3 destination = object->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;
					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					if(node->turns < bestTurns || (node->turns == bestTurns && node->cost < bestCost))
					{
						best = destination;
						bestTurns = node->turns;
						bestCost = node->cost;
					}
				}
			}

	return best;
}

std::optional<int3> AutoHeroController::findCaptureTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	const auto paths = owner.getPathsInfo(hero);
	const auto player = owner.cb->getPlayerID();
	if(!paths || !player)
		return std::nullopt;

	const int3 mapSize = owner.cb->getMapSize();
	std::optional<int3> best;
	int bestTurns = std::numeric_limits<int>::max();
	float bestCost = std::numeric_limits<float>::max();

	for(int z = 0; z < mapSize.z; ++z)
		for(int x = 0; x < mapSize.x; ++x)
			for(int y = 0; y < mapSize.y; ++y)
			{
				const int3 tile(x, y, z);
				if(!owner.cb->isVisible(tile))
					continue;

				for(const auto * object : owner.cb->getVisitableObjs(tile))
				{
					if(!isCaptureTarget(object, *player))
						continue;
					const int3 destination = object->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;
					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					if(node->turns < bestTurns || (node->turns == bestTurns && node->cost < bestCost))
					{
						best = destination;
						bestTurns = node->turns;
						bestCost = node->cost;
					}
				}
			}

	return best;
}

std::optional<int3> AutoHeroController::findFightTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	if(config.combatPolicy == AutoHeroes::CombatPolicy::DISABLED)
		return std::nullopt;

	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return std::nullopt;

	const double requiredRatio = config.combatPolicy == AutoHeroes::CombatPolicy::SAFE_ONLY ? 2.0 : 1.35;
	const double heroStrength = static_cast<double>(std::max<ui64>(1, hero->estimateHeroCombatValue()));
	const int3 mapSize = owner.cb->getMapSize();
	std::optional<int3> best;
	int bestTurns = std::numeric_limits<int>::max();
	float bestCost = std::numeric_limits<float>::max();

	for(int z = 0; z < mapSize.z; ++z)
		for(int x = 0; x < mapSize.x; ++x)
			for(int y = 0; y < mapSize.y; ++y)
			{
				const int3 tile(x, y, z);
				if(!owner.cb->isVisible(tile))
					continue;
				for(const auto * object : owner.cb->getVisitableObjs(tile))
				{
					if(!object || object->ID != Obj::MONSTER)
						continue;
					const auto * army = dynamic_cast<const CArmedInstance *>(object);
					if(!army)
						continue;
					const double enemyStrength = static_cast<double>(std::max<ui64>(1, army->estimateCombatValue()));
					if(heroStrength / enemyStrength < requiredRatio)
						continue;

					const int3 destination = object->visitablePos();
					if(!withinConfiguredRadius(hero, config, destination))
						continue;
					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;
					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination, true))
						continue;

					if(node->turns < bestTurns || (node->turns == bestTurns && node->cost < bestCost))
					{
						best = destination;
						bestTurns = node->turns;
						bestCost = node->cost;
					}
				}
			}

	return best;
}

std::optional<int3> AutoHeroController::findExploreTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return std::nullopt;

	const int3 mapSize = owner.cb->getMapSize();
	std::optional<int3> best;
	int bestTurns = std::numeric_limits<int>::max();
	int bestHiddenNeighbours = -1;
	float bestCost = std::numeric_limits<float>::max();

	for(int z = 0; z < mapSize.z; ++z)
	{
		for(int x = 0; x < mapSize.x; ++x)
		{
			for(int y = 0; y < mapSize.y; ++y)
			{
				const int3 tile(x, y, z);
				if(tile == hero->visitablePos() || !owner.cb->isVisible(tile) || !withinConfiguredRadius(hero, config, tile))
					continue;
				if(!owner.cb->getVisitableObjs(tile).empty())
					continue;
				if(owner.cb->guardingCreaturePosition(tile) != int3(-1, -1, -1))
					continue;

				const CGPathNode * node = paths->getPathInfo(tile);
				if(!node || !node->reachable() || node->accessible != EPathAccessibility::ACCESSIBLE)
					continue;

				int hiddenNeighbours = 0;
				for(int dx = -1; dx <= 1; ++dx)
				{
					for(int dy = -1; dy <= 1; ++dy)
					{
						if(dx == 0 && dy == 0)
							continue;
						const int nx = x + dx;
						const int ny = y + dy;
						if(nx < 0 || ny < 0 || nx >= mapSize.x || ny >= mapSize.y)
							continue;
						if(!owner.cb->isVisible(int3(nx, ny, z)))
							++hiddenNeighbours;
					}
				}

				if(hiddenNeighbours == 0)
					continue;

				CGPath path;
				if(!paths->getPath(path, tile, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, tile, false))
					continue;

				// Prefer a frontier reachable sooner. If nothing useful can be reached
				// this turn, selecting a next-turn frontier still gives us a safe path
				// whose current-turn segment can be walked with the remaining movement.
				if(node->turns < bestTurns
					|| (node->turns == bestTurns && hiddenNeighbours > bestHiddenNeighbours)
					|| (node->turns == bestTurns && hiddenNeighbours == bestHiddenNeighbours && node->cost < bestCost))
				{
					best = tile;
					bestTurns = node->turns;
					bestHiddenNeighbours = hiddenNeighbours;
					bestCost = node->cost;
				}
			}
		}
	}

	return best;
}

bool AutoHeroController::pathIsSafeForMvp(const CGHeroInstance * hero, const CGPath & path, const int3 & destination, bool allowDestinationBattle) const
{
	if(path.nodes.size() < 2)
		return false;

	for(const auto & node : path.nodes)
	{
		const bool isDestination = node.coord == destination;
		if(isTeleportAction(node.action))
			return false;
		if(node.accessible == EPathAccessibility::GUARDED || isBattleAction(node.action))
		{
			if(!(allowDestinationBattle && isDestination))
				return false;
		}

		if(node.coord != hero->visitablePos() && !isDestination)
		{
			if(node.action != EPathNodeAction::NORMAL && node.action != EPathNodeAction::UNKNOWN)
				return false;
		}
	}

	return true;
}

bool AutoHeroController::withinConfiguredRadius(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config, const int3 & destination) const
{
	return config.movementRadius <= 0 || hero->visitablePos().dist2d(destination) <= config.movementRadius;
}
