/*
 * AutoHeroController.cpp, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#include "StdInc.h"
#include "AutoHeroController.h"

#include "CPlayerInterface.h"
#include "PlayerLocalState.h"

#include "../lib/autoheroes/AutoHeroConfig.h"
#include "../lib/callback/CCallback.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGObjectInstance.h"
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

bool AutoHeroController::start()
{
	if(running)
	{
		logGlobal->warn("AutoHeroes v0.6: controller is already running");
		return false;
	}

	if(!owner.makingTurn || owner.isHeroMoving() || owner.showingDialog->isBusy())
	{
		logGlobal->warn("AutoHeroes v0.6: cannot start while turn, movement or dialog state is busy");
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
		logGlobal->info("AutoHeroes v0.6: no enabled hero with movement points is available");
		return false;
	}

	running = true;
	waitingForMovement = false;
	waitingForDialog = false;
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;
	logGlobal->info("AutoHeroes v0.6: starting local controller for %d configured heroes", static_cast<int>(heroQueue.size()));
	process();
	return true;
}

void AutoHeroController::cancel()
{
	if(running)
		logGlobal->info("AutoHeroes v0.6: local controller stopped");

	running = false;
	waitingForMovement = false;
	waitingForDialog = false;
	heroQueue.clear();
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;
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
		logGlobal->warn("AutoHeroes v0.6: hero %s did not make progress; skipping it to avoid a loop", hero->getNameTextID());
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

void AutoHeroController::advanceHero()
{
	activeHeroId.reset();
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
			logGlobal->warn("AutoHeroes v0.6: step limit reached for hero %s", hero->getNameTextID());
			advanceHero();
			continue;
		}

		if(startNextAction(hero))
			return;

		logGlobal->info("AutoHeroes v0.6: no MVP target found for hero %s", hero->getNameTextID());
		advanceHero();
	}

	logGlobal->info("AutoHeroes v0.6: configured heroes finished; human turn remains active");
	cancel();
}

bool AutoHeroController::startNextAction(const CGHeroInstance * hero)
{
	const AutoHeroes::HeroConfig config = AutoHeroes::readHeroConfig(hero->id);

	for(const auto action : config.priority)
	{
		if(!config.allows(action))
			continue;

		std::optional<int3> destination;
		switch(action)
		{
		case AutoHeroes::Action::COLLECT_RESOURCES:
			destination = findCollectTarget(hero, config);
			break;
		case AutoHeroes::Action::EXPLORE:
			destination = findExploreTarget(hero, config);
			break;
		default:
			// Stage 6 MVP only automates collection and exploration. Other actions
			// stay configured and will be implemented on top of this stable controller.
			break;
		}

		if(destination && startMovement(hero, *destination))
			return true;
	}

	return false;
}

bool AutoHeroController::startMovement(const CGHeroInstance * hero, const int3 & destination)
{
	if(!hero || destination == hero->visitablePos())
		return false;

	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return false;

	CGPath path;
	if(!paths->getPath(path, destination, EPathfindingLayer::AUTO))
		return false;

	if(!path.hasNextNode() || path.nextNode().turns != 0 || !pathIsSafeForMvp(hero, path, destination))
		return false;

	owner.localState->setPath(hero, path);
	owner.localState->setSelection(hero);

	movementStartPosition = hero->visitablePos();
	movementStartPoints = hero->movementPointsRemaining();
	++stepsForCurrentHero;
	waitingForMovement = true;

	logGlobal->info("AutoHeroes v0.6: moving hero %s from %s to %s", hero->getNameTextID(), movementStartPosition.toString(), destination.toString());
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

					const int3 destination = object->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable() || node->turns != 0 || node->cost >= bestCost)
						continue;

					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination))
						continue;

					best = destination;
					bestCost = node->cost;
				}
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
	int bestHiddenNeighbours = -1;
	int bestDistance = -1;

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
				if(!node || !node->reachable() || node->turns != 0 || node->accessible != EPathAccessibility::ACCESSIBLE)
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
				if(!paths->getPath(path, tile, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, tile))
					continue;

				const int distance = hero->visitablePos().dist2d(tile);
				if(hiddenNeighbours > bestHiddenNeighbours || (hiddenNeighbours == bestHiddenNeighbours && distance > bestDistance))
				{
					best = tile;
					bestHiddenNeighbours = hiddenNeighbours;
					bestDistance = distance;
				}
			}
		}
	}

	return best;
}

bool AutoHeroController::pathIsSafeForMvp(const CGHeroInstance * hero, const CGPath & path, const int3 & destination) const
{
	if(path.nodes.size() < 2)
		return false;

	for(const auto & node : path.nodes)
	{
		if(node.turns != 0 || node.accessible == EPathAccessibility::GUARDED || isBattleAction(node.action) || isTeleportAction(node.action))
			return false;

		if(node.coord != hero->visitablePos() && node.coord != destination)
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
