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
#include "../lib/mapObjects/CGTownInstance.h"
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
	if(!dwelling || dynamic_cast<const CGTownInstance *>(target) || dwelling->ID == Obj::WAR_MACHINE_FACTORY)
		return false;

	return dwelling->ID == Obj::REFUGEE_CAMP || dwelling->getOwner() == player;
}

const char * actionName(AutoHeroes::Action action)
{
	switch(action)
	{
	case AutoHeroes::Action::COLLECT_RESOURCES: return "collect";
	case AutoHeroes::Action::LEVEL_UP: return "level";
	case AutoHeroes::Action::RECRUIT_CREATURES: return "recruit";
	case AutoHeroes::Action::EXPLORE: return "explore";
	case AutoHeroes::Action::CAPTURE_OBJECTS: return "capture";
	case AutoHeroes::Action::FIGHT_NEUTRALS: return "fight";
	}
	return "unknown";
}

}

AutoHeroController::AutoHeroController(CPlayerInterface & owner_)
	: owner(owner_)
{
}

void AutoHeroController::onNewTurn()
{
	attemptedRecruitTowns.clear();

	const int currentDay = owner.cb ? owner.cb->getCalendar().getCurrentDay() : -1;
	const bool firstDayOfWeek = currentDay > 0 && ((currentDay - 1) % 7 == 0);
	if(firstDayOfWeek)
	{
		weeklyRecruitTowns.clear();
		logGlobal->info("AutoHeroes v1.2: new week detected; cleared weekly town recruitment locks");
	}
	else
	{
		logGlobal->info("AutoHeroes v1.2: cleared per-turn town recruitment attempts; weekly locks=%d", static_cast<int>(weeklyRecruitTowns.size()));
	}
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

int AutoHeroController::recruitmentBudgetPercent() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::recruitmentBudgetPercent(AutoHeroes::readHeroConfig(hero->id).recruitmentBudget) : 100;
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

	if(activeAction && *activeAction == AutoHeroes::Action::RECRUIT_CREATURES)
	{
		const auto config = AutoHeroes::readHeroConfig(hero->id);
		const int recruited = recruitFromCurrentTown(hero, config);
		if(recruited > 0)
		{
			logGlobal->info("AutoHeroes v1.2: recruited %d creatures from town for hero %s", recruited, hero->getNameTextID());
			owner.closeAllDialogs();
		}
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
			logGlobal->info("AutoHeroes v1.2: hero %s selected action=%s target=%s", hero->getNameTextID(), actionName(action), destination->toString());
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

bool AutoHeroController::dwellingHasUsefulRecruit(const CGDwelling * dwelling, const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	if(!dwelling || !hero)
		return false;

	auto resources = owner.cb->getResourceAmount();
	const int budgetPercent = AutoHeroes::recruitmentBudgetPercent(config.recruitmentBudget);
	const int currentGold = resources[EGameResID::GOLD];
	resources[EGameResID::GOLD] = static_cast<int>((static_cast<int64_t>(currentGold) * budgetPercent) / 100);

	int foreignSlots = 0;
	for(const auto & stack : hero->Slots())
		if(stack.second->getType() && stack.second->getCreature()->getFactionID() != hero->getFactionID())
			++foreignSlots;

	for(auto levelIt = dwelling->creatures.rbegin(); levelIt != dwelling->creatures.rend(); ++levelIt)
	{
		const auto & level = *levelIt;
		if(level.first == 0 || level.second.empty())
			continue;

		const CreatureID creature = level.second.back();
		const auto * creatureType = creature.toCreature();
		if(config.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY
			&& creatureType->getFactionID() != hero->getFactionID())
			continue;

		bool alreadyHasCreature = false;
		for(const auto & stack : hero->Slots())
			if(stack.second->getCreatureID() == creature)
			{
				alreadyHasCreature = true;
				break;
			}

		if(creatureType->getFactionID() != hero->getFactionID()
			&& config.maxForeignFactionSlots >= 0
			&& !alreadyHasCreature
			&& foreignSlots >= config.maxForeignFactionSlots)
			continue;

		if(!hero->getSlotFor(creature).validSlot())
			continue;
		if(creatureType->maxAmount(resources) <= 0)
			continue;
		return true;
	}

	return false;
}

bool AutoHeroController::townRecruitLocked(const CGHeroInstance * hero, const CGTownInstance * town) const
{
	return hero && town && weeklyRecruitTowns.count({hero->id.getNum(), town->id.getNum()}) != 0;
}

void AutoHeroController::lockTownRecruit(const CGHeroInstance * hero, const CGTownInstance * town)
{
	if(!hero || !town)
		return;

	weeklyRecruitTowns.emplace(hero->id.getNum(), town->id.getNum());
	logGlobal->info("AutoHeroes v1.2: weekly recruit lock set hero=%s town=%s",
		hero->getNameTextID(), town->visitablePos().toString());
}

int AutoHeroController::recruitFromCurrentTown(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config)
{
	if(!hero || !owner.cb)
		return 0;

	const auto player = owner.cb->getPlayerID();
	if(!player)
		return 0;

	for(const auto * object : owner.cb->getVisitableObjs(hero->visitablePos()))
	{
		const auto * town = dynamic_cast<const CGTownInstance *>(object);
		if(!town || town->getOwner() != *player)
			continue;
		if(townRecruitLocked(hero, town))
		{
			logGlobal->info("AutoHeroes v1.2: town recruitment skipped hero=%s town=%s reason=weekly_lock",
				hero->getNameTextID(), town->visitablePos().toString());
			return 0;
		}
		if(attemptedRecruitTowns.count(town->id))
			return 0;

		attemptedRecruitTowns.insert(town->id);
		if(town->getVisitingHero() != hero && town->getGarrisonHero() != hero)
		{
			logGlobal->warn("AutoHeroes v1.2: hero %s reached town %s but is not registered as visiting/garrison hero; town recruitment skipped",
				hero->getNameTextID(), town->visitablePos().toString());
			return 0;
		}

		auto resources = owner.cb->getResourceAmount();
		const int budgetPercent = AutoHeroes::recruitmentBudgetPercent(config.recruitmentBudget);
		const int totalGold = resources[EGameResID::GOLD];
		const int budgetGold = static_cast<int>((static_cast<int64_t>(totalGold) * budgetPercent) / 100);
		resources[EGameResID::GOLD] = budgetGold;
		int recruitedTotal = 0;
		int foreignSlots = 0;
		for(const auto & stack : hero->Slots())
			if(stack.second->getType() && stack.second->getCreature()->getFactionID() != hero->getFactionID())
				++foreignSlots;

		logGlobal->info("AutoHeroes v1.2: town recruitment start hero=%s town=%s totalGold=%d budget=%d%% budgetGold=%d order=high-tier-first",
			hero->getNameTextID(), town->visitablePos().toString(), totalGold, budgetPercent, budgetGold);

		for(int levelIndex = static_cast<int>(town->creatures.size()) - 1; levelIndex >= 0; --levelIndex)
		{
			const auto & level = town->creatures[levelIndex];
			if(level.first == 0 || level.second.empty())
			{
				logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d available=%d result=SKIP reason=empty",
					town->visitablePos().toString(), levelIndex, static_cast<int>(level.first));
				continue;
			}

			const CreatureID creature = level.second.back();
			const auto * creatureType = creature.toCreature();
			const int available = static_cast<int>(level.first);
			const int goldPerUnit = static_cast<int>(creatureType->getFullRecruitCost()[EGameResID::GOLD]);

			if(config.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY
				&& creatureType->getFactionID() != hero->getFactionID())
			{
				logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d goldPerUnit=%d result=SKIP reason=faction_filter",
					town->visitablePos().toString(), levelIndex, creature.getNum(), available, goldPerUnit);
				continue;
			}

			bool alreadyHasCreature = false;
			for(const auto & stack : hero->Slots())
				if(stack.second->getCreatureID() == creature)
				{
					alreadyHasCreature = true;
					break;
				}

			if(creatureType->getFactionID() != hero->getFactionID()
				&& config.maxForeignFactionSlots >= 0
				&& !alreadyHasCreature
				&& foreignSlots >= config.maxForeignFactionSlots)
			{
				logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d goldPerUnit=%d result=SKIP reason=foreign_slot_limit",
					town->visitablePos().toString(), levelIndex, creature.getNum(), available, goldPerUnit);
				continue;
			}
			if(!hero->getSlotFor(creature).validSlot())
			{
				logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d goldPerUnit=%d result=SKIP reason=army_slots",
					town->visitablePos().toString(), levelIndex, creature.getNum(), available, goldPerUnit);
				continue;
			}

			if(goldPerUnit > 0 && resources[EGameResID::GOLD] < goldPerUnit)
			{
				logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d goldPerUnit=%d budgetGoldRemaining=%d result=SKIP reason=gold_budget",
					town->visitablePos().toString(), levelIndex, creature.getNum(), available, goldPerUnit, resources[EGameResID::GOLD]);
				continue;
			}

			const int affordable = creatureType->maxAmount(resources);
			const int count = std::min<int>(available, affordable);
			if(count <= 0)
			{
				logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d goldPerUnit=%d budgetGoldRemaining=%d result=SKIP reason=other_resources",
					town->visitablePos().toString(), levelIndex, creature.getNum(), available, goldPerUnit, resources[EGameResID::GOLD]);
				continue;
			}

			const int goldCost = goldPerUnit * count;
			owner.cb->recruitCreatures(town, hero, creature, count, levelIndex);
			resources -= creatureType->getFullRecruitCost() * count;
			recruitedTotal += count;
			if(creatureType->getFactionID() != hero->getFactionID() && !alreadyHasCreature)
				++foreignSlots;

			logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d count=%d goldCost=%d budgetGoldRemaining=%d result=BUY",
				town->visitablePos().toString(), levelIndex, creature.getNum(), available, count, goldCost, resources[EGameResID::GOLD]);
		}

		if(recruitedTotal > 0)
			lockTownRecruit(hero, town);

		logGlobal->info("AutoHeroes v1.2: town recruitment attempt at %s finished, recruited=%d budgetGoldRemaining=%d",
			town->visitablePos().toString(), recruitedTotal, resources[EGameResID::GOLD]);
		return recruitedTotal;
	}

	return 0;
}

std::optional<int3> AutoHeroController::findRecruitTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
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
					const auto * dwelling = dynamic_cast<const CGDwelling *>(object);
					const auto * town = dynamic_cast<const CGTownInstance *>(object);

					if(town)
					{
						if(town->getOwner() != *player || attemptedRecruitTowns.count(town->id) || townRecruitLocked(hero, town))
							continue;
						if(!dwellingHasUsefulRecruit(town, hero, config))
							continue;
					}
					else
					{
						if(!isRecruitTarget(object, *player) || !dwellingHasUsefulRecruit(dwelling, hero, config))
							continue;
					}

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

	// Match VCMI's long-standing conservative adventure-AI baseline more closely.
	// The detailed candidate log below lets the next test distinguish strength,
	// radius and path-safety rejection without handing the whole player to NK2.
	const double requiredRatio = config.combatPolicy == AutoHeroes::CombatPolicy::SAFE_ONLY ? 1.50 : 1.15;
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

					const int3 destination = object->visitablePos();
					const double enemyStrength = static_cast<double>(std::max<ui64>(1, army->estimateCombatValue()));
					const double ratio = heroStrength / enemyStrength;
					const bool ratioOk = ratio >= requiredRatio;
					const bool withinRadius = withinConfiguredRadius(hero, config, destination);
					const CGPathNode * node = paths->getPathInfo(destination);
					const bool reachable = node && node->reachable();
					CGPath path;
					const bool pathFound = reachable && paths->getPath(path, destination, EPathfindingLayer::AUTO);
					const bool pathSafe = pathFound && pathIsSafeForMvp(hero, path, destination, true);

					const char * reason = "accepted";
					if(!ratioOk)
						reason = "strength_ratio";
					else if(!withinRadius)
						reason = "radius";
					else if(!node)
						reason = "no_path_node";
					else if(!reachable)
						reason = "unreachable";
					else if(!pathFound)
						reason = "path_not_found";
					else if(!pathSafe)
						reason = "path_unsafe";

					const bool accepted = ratioOk && withinRadius && reachable && pathFound && pathSafe;
					logGlobal->info(
						"AutoHeroes v1.2 combat candidate hero=%s coord=%s heroStrength=%.0f enemyStrength=%.0f ratio=%.3f required=%.2f visible=1 withinRadius=%d reachable=%d pathFound=%d pathSafe=%d result=%s reason=%s",
						hero->getNameTextID(), destination.toString(), heroStrength, enemyStrength, ratio, requiredRatio,
						withinRadius ? 1 : 0, reachable ? 1 : 0, pathFound ? 1 : 0, pathSafe ? 1 : 0,
						accepted ? "ACCEPT" : "REJECT", reason);

					if(!accepted)
						continue;

					if(node->turns < bestTurns || (node->turns == bestTurns && node->cost < bestCost))
					{
						best = destination;
						bestTurns = node->turns;
						bestCost = node->cost;
					}
				}
			}

	if(best)
		logGlobal->info("AutoHeroes v1.2 combat selected hero=%s target=%s", hero->getNameTextID(), best->toString());
	else
		logGlobal->info("AutoHeroes v1.2 combat: no acceptable neutral target for hero=%s", hero->getNameTextID());

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
		const int3 guardingCreature = owner.cb->guardingCreaturePosition(node.coord);
		const bool guardedByDestination = allowDestinationBattle && guardingCreature == destination;
		const bool allowedCombatNode = allowDestinationBattle && (isDestination || guardedByDestination);

		if(isTeleportAction(node.action))
			return false;

		// When moving to a neutral stack VCMI may put BATTLE/GUARDED on the
		// guarded approach tile, not on the monster tile itself. Permit that
		// node only when the guard is exactly the neutral selected as destination.
		// Any unrelated guarded/battle node on the route remains forbidden.
		if(node.accessible == EPathAccessibility::GUARDED || isBattleAction(node.action))
		{
			if(!allowedCombatNode)
				return false;
		}

		if(node.coord != hero->visitablePos() && !isDestination)
		{
			if(node.action != EPathNodeAction::NORMAL && node.action != EPathNodeAction::UNKNOWN)
			{
				if(!(isBattleAction(node.action) && guardedByDestination))
					return false;
			}
		}
	}

	return true;
}

bool AutoHeroController::withinConfiguredRadius(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config, const int3 & destination) const
{
	return config.movementRadius <= 0 || hero->visitablePos().dist2d(destination) <= config.movementRadius;
}
