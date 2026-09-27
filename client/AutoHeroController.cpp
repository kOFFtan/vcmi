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
#include "../lib/GameConstants.h"
#include "../lib/ConditionalWait.h"
#include "../lib/callback/CCallback.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/CGCreature.h"
#include "../lib/mapObjects/Quest.h"
#include "../lib/mapObjects/CGObjectInstance.h"
#include "../lib/mapObjects/CGDwelling.h"
#include "../lib/mapObjects/CGTownInstance.h"
#include "../lib/mapObjects/CRewardableObject.h"
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
	case Obj::TREASURE_CHEST:
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

bool isMagicShrineTarget(const CGObjectInstance * target)
{
	if(!target)
		return false;

	return target->ID == Obj::SHRINE_OF_MAGIC_INCANTATION
		|| target->ID == Obj::SHRINE_OF_MAGIC_GESTURE
		|| target->ID == Obj::SHRINE_OF_MAGIC_THOUGHT;
}

constexpr int LOCAL_OPPORTUNITY_RADIUS = 5;

int localTileDistance(const int3 & from, const int3 & to)
{
	if(from.z != to.z)
		return std::numeric_limits<int>::max();

	return std::max(std::abs(from.x - to.x), std::abs(from.y - to.y));
}

std::optional<SpellID> magicShrineSpell(const CGObjectInstance * target)
{
	const auto * shrine = dynamic_cast<const CRewardableObject *>(target);
	if(!shrine)
		return std::nullopt;

	for(const auto & visit : shrine->configuration.info)
	{
		if(visit.visitType != Rewardable::EEventType::EVENT_FIRST_VISIT)
			continue;
		if(!visit.reward.spells.empty())
			return visit.reward.spells.front();
		if(!visit.limiter.canLearnSpells.empty())
			return visit.limiter.canLearnSpells.front();
	}

	return std::nullopt;
}

int magicShrineLevel(const CGObjectInstance * target)
{
	if(!target)
		return -1;
	if(target->ID == Obj::SHRINE_OF_MAGIC_INCANTATION)
		return 1;
	if(target->ID == Obj::SHRINE_OF_MAGIC_GESTURE)
		return 2;
	if(target->ID == Obj::SHRINE_OF_MAGIC_THOUGHT)
		return 3;
	return -1;
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

bool isEmptyNeutralTown(const CGObjectInstance * target)
{
	const auto * town = dynamic_cast<const CGTownInstance *>(target);
	if(!town || town->getOwner() != PlayerColor::NEUTRAL)
		return false;

	if(town->getVisitingHero() || town->getGarrisonHero())
		return false;

	const auto * armedTown = dynamic_cast<const CArmedInstance *>(town);
	return !armedTown || armedTown->Slots().empty();
}

bool isCaptureTarget(const CGObjectInstance * target, PlayerColor player)
{
	if(!target || target->ID == Obj::HERO)
		return false;

	// Town recruitment is handled separately. Only a genuinely empty neutral
	// town is an automatic capture target; guarded/enemy towns need their own
	// danger evaluation before they can be automated safely.
	if(target->ID == Obj::TOWN)
		return isEmptyNeutralTown(target);

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
	logGlobal->info("AHDBG NEW_TURN day=%d firstDayOfWeek=%d weeklyLocksBefore=%d",
		currentDay, firstDayOfWeek ? 1 : 0, static_cast<int>(weeklyRecruitTowns.size()));
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

bool AutoHeroController::hasMultipleAutoHeroes() const
{
	return heroQueue.size() > 1;
}

bool AutoHeroController::isCombatHero(const CGHeroInstance * hero) const
{
	if(!hero)
		return false;
	if(!hasMultipleAutoHeroes())
		return true;
	return combatHeroId && hero->id == *combatHeroId;
}

AutoHeroes::DecisionPolicy AutoHeroController::decisionPolicy() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).decisionPolicy : AutoHeroes::DecisionPolicy::ASK_HUMAN;
}

AutoHeroes::TreasureChestChoice AutoHeroController::treasureChestChoice() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).treasureChestChoice : AutoHeroes::TreasureChestChoice::GOLD;
}

bool AutoHeroController::isTreasureChestInteraction() const
{
	return running && activeAction && *activeAction == AutoHeroes::Action::COLLECT_RESOURCES && activeCollectTreasureChest;
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

AutoHeroes::RecruitmentLocation AutoHeroController::recruitmentLocation() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).recruitmentLocation : AutoHeroes::RecruitmentLocation::ANYWHERE;
}

int AutoHeroController::maxForeignFactionSlots() const
{
	const auto * hero = activeHero();
	return hero ? AutoHeroes::readHeroConfig(hero->id).maxForeignFactionSlots : 0;
}

bool AutoHeroController::shouldAutoFight(const CGHeroInstance * hero) const
{
	if(!running || !hero || !activeHeroId || hero->id != *activeHeroId || !activeAction)
		return false;

	const bool directNeutralFight = *activeAction == AutoHeroes::Action::FIGHT_NEUTRALS;
	const bool guardedCollection = *activeAction == AutoHeroes::Action::COLLECT_RESOURCES && activeBattleGuard.has_value();
	if(!directNeutralFight && !guardedCollection)
		return false;

	return AutoHeroes::readHeroConfig(hero->id).combatPolicy != AutoHeroes::CombatPolicy::DISABLED;
}

bool AutoHeroController::isAutoBattleActive() const
{
	if(!running || !activeAction)
		return false;

	return *activeAction == AutoHeroes::Action::FIGHT_NEUTRALS
		|| (*activeAction == AutoHeroes::Action::COLLECT_RESOURCES && activeBattleGuard.has_value());
}

void AutoHeroController::onBattleStarted()
{
	if(!running)
		return;

	waitingForBattle = true;
	waitingForMovement = false;
	encounterGraceTicks = 0;
	battleCleanupTicks = 0;
	battleCleanupTarget = activeBattleGuard;
	++diagnosticsBattlesStarted;
	const auto * hero = activeHero();
	logGlobal->info("AHDBG BATTLE_START hero=%s action=%s guard=%s mp=%d",
		hero ? hero->getNameTextID() : "<none>",
		activeAction ? actionName(*activeAction) : "none",
		activeBattleGuard ? activeBattleGuard->toString() : "<none>",
		hero ? hero->movementPointsRemaining() : -1);
	logGlobal->info("AutoHeroes v1.7: battle started for active auto hero; automation paused until battle state is fully resolved");
}

void AutoHeroController::onBattleFinished()
{
	if(!running)
		return;

	waitingForBattle = false;
	waitingForMovement = false;
	waitingForDialog = false;
	encounterGraceTicks = 0;
	activeBattleGuard.reset();
	battleCleanupTicks = 30;
	++diagnosticsBattlesFinished;
	const auto * hero = activeHero();
	logGlobal->info("AHDBG BATTLE_END hero=%s mp=%d cleanupTicks=%d battles=%d/%d",
		hero ? hero->getNameTextID() : "<none>", hero ? hero->movementPointsRemaining() : -1, battleCleanupTicks,
		diagnosticsBattlesFinished, diagnosticsBattlesStarted);
	logGlobal->info("AutoHeroes v1.7: battle finished; waiting for battle state cleanup before resuming");
}

void AutoHeroController::onDialogResolved()
{
	if(!running)
		return;
	waitingForDialog = false;
	const auto * hero = activeHero();
	logGlobal->info("AHDBG DIALOG_RESOLVED hero=%s action=%s mp=%d encounterGrace=%d pendingQueries=%d",
		hero ? hero->getNameTextID() : "<none>", activeAction ? actionName(*activeAction) : "none",
		hero ? hero->movementPointsRemaining() : -1, encounterGraceTicks, pendingQueryReplies);
}

void AutoHeroController::onQueryOpened()
{
	if(!running)
		return;

	++pendingQueryReplies;
	querySettleTicks = 0;
	const auto * hero = activeHero();
	logGlobal->info("AHDBG QUERY_BARRIER state=OPEN hero=%s action=%s pending=%d",
		hero ? hero->getNameTextID() : "<none>", activeAction ? actionName(*activeAction) : "none", pendingQueryReplies);
}

void AutoHeroController::onQueryReplyApplied()
{
	if(!running || pendingQueryReplies <= 0)
		return;

	--pendingQueryReplies;
	if(pendingQueryReplies == 0)
		querySettleTicks = 3;

	const auto * hero = activeHero();
	logGlobal->info("AHDBG QUERY_BARRIER state=REPLY_APPLIED hero=%s action=%s pending=%d settleTicks=%d",
		hero ? hero->getNameTextID() : "<none>", activeAction ? actionName(*activeAction) : "none",
		pendingQueryReplies, querySettleTicks);
}

bool AutoHeroController::shouldAutoAcceptDwellingRecruit() const
{
	if(!running || !owner.cb)
		return false;

	const auto * hero = activeHero();
	if(!hero || !allowsAction(AutoHeroes::Action::RECRUIT_CREATURES))
		return false;

	const auto config = AutoHeroes::readHeroConfig(hero->id);
	if(config.recruitmentLocation == AutoHeroes::RecruitmentLocation::TOWN_ONLY)
		return false;

	for(const auto * object : owner.cb->getVisitableObjs(hero->visitablePos()))
	{
		const auto * dwelling = dynamic_cast<const CGDwelling *>(object);
		if(!dwelling || dynamic_cast<const CGTownInstance *>(dwelling) || dwelling->ID == Obj::WAR_MACHINE_FACTORY)
			continue;

		// A guarded hostile dwelling uses a visually similar yes/no query for combat.
		// Never auto-confirm that query as if it were a recruitment offer.
		if(dwelling->stacksCount() > 0)
			continue;

		const bool useful = dwellingHasUsefulRecruit(dwelling, hero, config);
		logGlobal->info("AHDBG FIELD_RECRUIT_OFFER hero=%s object=%s objectId=%d coord=%s useful=%d",
			hero->getNameTextID(), dwelling->getSubtypeName(), dwelling->id.getNum(),
			dwelling->visitablePos().toString(), useful ? 1 : 0);
		if(useful)
			return true;
	}

	return false;
}

std::optional<int> AutoHeroController::autoCreatureEncounterReply() const
{
	if(!running || !owner.cb || !activeBattleGuard)
		return std::nullopt;

	const auto * hero = activeHero();
	if(!hero)
		return std::nullopt;

	const CGCreature * monster = nullptr;
	for(const auto * object : owner.cb->getVisitableObjs(*activeBattleGuard))
	{
		if(object && object->ID == Obj::MONSTER)
		{
			monster = dynamic_cast<const CGCreature *>(object);
			if(monster)
				break;
		}
	}
	if(!monster)
		return std::nullopt;

	// After refusing an impossible join VCMI may immediately ask whether to
	// pursue the same stack. This is still the combat target selected by AutoHeroes.
	if(monster->refusedJoining)
	{
		logGlobal->info("AHDBG CREATURE_ENCOUNTER hero=%s type=pursue_after_refused_join reply=1",
			hero->getNameTextID());
		return 1;
	}

	const int action = monster->getEncounterAction(hero);
	if(action == CGCreature::FIGHT)
		return std::nullopt;

	if(action == CGCreature::FLEE)
	{
		logGlobal->info("AHDBG CREATURE_ENCOUNTER hero=%s type=flee reply=1 reason=selected_combat_target",
			hero->getNameTextID());
		return 1;
	}

	const CreatureID creature = monster->getCreatureID();
	const bool slotAvailable = hero->getSlotFor(creature).validSlot();
	const auto cfg = AutoHeroes::readHeroConfig(hero->id);
	const int totalGold = owner.cb->getResourceAmount(EGameResID::GOLD);
	const int spendableGold = static_cast<int>((static_cast<int64_t>(totalGold)
		* AutoHeroes::recruitmentBudgetPercent(cfg.recruitmentBudget)) / 100);
	const bool affordable = action <= spendableGold;
	const bool accept = slotAvailable && affordable;

	logGlobal->info(
		"AHDBG CREATURE_ENCOUNTER hero=%s type=join creature=%d cost=%d gold=%d spendable=%d slotAvailable=%d reply=%d",
		hero->getNameTextID(), creature.getNum(), std::max(0, action), totalGold, spendableGold,
		slotAvailable ? 1 : 0, accept ? 1 : 0);

	return accept ? 1 : 0;
}

bool AutoHeroController::shouldAutoAcceptBorderGuard() const
{
	if(!running || !owner.cb)
		return false;

	const auto * hero = activeHero();
	if(!hero)
		return false;

	for(const auto * object : owner.cb->getVisitableObjs(hero->visitablePos()))
	{
		const auto * guard = dynamic_cast<const QuestGuard *>(object);
		if(!guard)
			continue;
		const auto & quest = guard->getQuest();
		if(quest.mission.requiredKeys.empty() || quest.isCompleted)
			continue;
		if(!guard->checkQuest(hero))
			continue;

		logGlobal->info("AHDBG BORDER_GUARD_REPLY hero=%s objectId=%d coord=%s reply=1",
			hero->getNameTextID(), guard->id.getNum(), guard->visitablePos().toString());
		return true;
	}
	return false;
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

	std::sort(heroQueue.begin(), heroQueue.end(), [this](ObjectInstanceID lhs, ObjectInstanceID rhs)
	{
		const auto * left = owner.cb ? owner.cb->getHero(lhs) : nullptr;
		const auto * right = owner.cb ? owner.cb->getHero(rhs) : nullptr;
		const ui64 leftStrength = left ? left->estimateHeroCombatValue() : 0;
		const ui64 rightStrength = right ? right->estimateHeroCombatValue() : 0;
		if(leftStrength != rightStrength)
			return leftStrength > rightStrength;
		return lhs.getNum() < rhs.getNum();
	});
	combatHeroId = heroQueue.front();

	if(heroQueue.size() > 1)
	{
		for(const auto heroId : heroQueue)
		{
			const auto * roleHero = owner.cb ? owner.cb->getHero(heroId) : nullptr;
			if(roleHero)
				logGlobal->info("AHDBG HERO_ROLE hero=%s strength=%llu role=%s",
					roleHero->getNameTextID(), static_cast<unsigned long long>(roleHero->estimateHeroCombatValue()),
					heroId == *combatHeroId ? "combat_scout" : "collector_visitor");
		}
	}

	running = true;
	waitingForMovement = false;
	waitingForDialog = false;
	waitingForBattle = false;
	pendingQueryReplies = 0;
	querySettleTicks = 0;
	encounterGraceTicks = 0;
	battleCleanupTicks = 0;
	battleCleanupTarget.reset();
	waitingForTownUpgrade = false;
	townUpgradeDelayTicks = 0;
	mergeAfterTownUpgrade = false;
	endTurnAfterRun = endTurnWhenFinished;
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;
	activeAction.reset();
	activeBattleGuard.reset();
	activeCollectTreasureChest = false;
	activeSpellbookAcquisition = false;
	activeSpellbookTown.reset();
	waitingForSpellbook = false;
	spellbookWaitTicks = 0;
	spellbookKnownSpellsBefore = 0;
	attemptedSpellbookTowns.clear();
	attemptedLevelObjects.clear();
	activeLevelObject.reset();
	activeLevelDestination.reset();
	attemptedShrines.clear();
	activeShrineObject.reset();
	activeShrineDestination.reset();
	activeShrineSpell.reset();
	activeShrineKnownSpellsBefore = 0;
	pendingShrineObject.reset();
	pendingShrineSpell.reset();
	pendingShrineKnownSpellsBefore = 0;
	shrineVerifyTicks = 0;
	pendingUpgradeAudit.clear();
	diagnosticsActions = 0;
	diagnosticsMoves = 0;
	diagnosticsBattlesStarted = 0;
	diagnosticsBattlesFinished = 0;
	diagnosticsUpgradeRequests = 0;
	diagnosticsMergeRequests = 0;
	diagnosticsRecruitAmount = 0;
	diagnosticsAnomalies = 0;
	const int currentDay = owner.cb ? owner.cb->getCalendar().getCurrentDay() : -1;
	logGlobal->info("AHDBG RUN_START day=%d heroes=%d endTurn=%d", currentDay, static_cast<int>(heroQueue.size()), endTurnAfterRun ? 1 : 0);
	logGlobal->info("AutoHeroes v1.7: starting local controller for %d configured heroes%s", static_cast<int>(heroQueue.size()), endTurnAfterRun ? " before End Turn" : "");
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
	waitingForBattle = false;
	pendingQueryReplies = 0;
	querySettleTicks = 0;
	encounterGraceTicks = 0;
	battleCleanupTicks = 0;
	battleCleanupTarget.reset();
	waitingForTownUpgrade = false;
	townUpgradeDelayTicks = 0;
	mergeAfterTownUpgrade = false;
	endTurnAfterRun = false;
	heroQueue.clear();
	heroQueueIndex = 0;
	activeHeroId.reset();
	stepsForCurrentHero = 0;
	activeAction.reset();
	activeBattleGuard.reset();
	activeCollectTreasureChest = false;
	activeSpellbookAcquisition = false;
	activeSpellbookTown.reset();
	waitingForSpellbook = false;
	spellbookWaitTicks = 0;
	spellbookKnownSpellsBefore = 0;
	attemptedSpellbookTowns.clear();
	attemptedLevelObjects.clear();
	activeLevelObject.reset();
	activeLevelDestination.reset();
	attemptedShrines.clear();
	activeShrineObject.reset();
	activeShrineDestination.reset();
	activeShrineSpell.reset();
	activeShrineKnownSpellsBefore = 0;
	pendingShrineObject.reset();
	pendingShrineSpell.reset();
	pendingShrineKnownSpellsBefore = 0;
	shrineVerifyTicks = 0;
	pendingUpgradeAudit.clear();
}

void AutoHeroController::update()
{
	if(!running)
		return;

	if(waitingForBattle)
		return;

	if(pendingQueryReplies > 0)
		return;

	if(querySettleTicks > 0)
	{
		--querySettleTicks;
		if(querySettleTicks == 0)
		{
			owner.invalidatePaths();
			const auto * hero = activeHero();
			logGlobal->info("AHDBG QUERY_BARRIER state=SETTLED hero=%s action=%s",
				hero ? hero->getNameTextID() : "<none>", activeAction ? actionName(*activeAction) : "none");
		}
		return;
	}

	if(waitingForSpellbook)
	{
		const CGHeroInstance * hero = activeHero();
		if(!hero)
		{
			waitingForSpellbook = false;
			activeSpellbookAcquisition = false;
			activeSpellbookTown.reset();
			advanceHero();
			process();
			return;
		}

		if(hero->hasSpellbook())
		{
			const int knownAfter = static_cast<int>(hero->getSpellsInSpellbook().size());
			logGlobal->info("AHDBG SPELLBOOK_ACQUIRED hero=%s knownBefore=%d knownAfter=%d learnedFromGuild=%d",
				hero->getNameTextID(), spellbookKnownSpellsBefore, knownAfter, std::max(0, knownAfter - spellbookKnownSpellsBefore));
			waitingForSpellbook = false;
			spellbookWaitTicks = 0;
			activeSpellbookAcquisition = false;
			activeSpellbookTown.reset();
			owner.invalidatePaths();
			process();
			return;
		}

		if(spellbookWaitTicks > 0)
		{
			--spellbookWaitTicks;
			return;
		}

		++diagnosticsAnomalies;
		logGlobal->warn("AHDBG SPELLBOOK_ACQUIRED hero=%s result=FAILED reason=timeout", hero->getNameTextID());
		logGlobal->warn("AHDBG ANOMALY type=spellbook_purchase_not_observed hero=%s", hero->getNameTextID());
		waitingForSpellbook = false;
		activeSpellbookAcquisition = false;
		activeSpellbookTown.reset();
		process();
		return;
	}

	if(encounterGraceTicks > 0)
	{
		--encounterGraceTicks;
		if(encounterGraceTicks == 0)
		{
			owner.invalidatePaths();
			const auto * hero = activeHero();
			logGlobal->info("AHDBG ENCOUNTER_GRACE_END hero=%s mp=%d waitingDialog=%d",
				hero ? hero->getNameTextID() : "<none>", hero ? hero->movementPointsRemaining() : -1, waitingForDialog ? 1 : 0);
		}
		return;
	}

	if(battleCleanupTicks > 0)
	{
		bool defeatedNeutralStillPresent = false;
		if(battleCleanupTarget)
		{
			for(const auto * object : owner.cb->getVisitableObjs(*battleCleanupTarget))
			{
				if(object && object->ID == Obj::MONSTER)
				{
					defeatedNeutralStillPresent = true;
					break;
				}
			}
		}

		--battleCleanupTicks;
		if(defeatedNeutralStillPresent && battleCleanupTicks > 0)
			return;

		battleCleanupTicks = 0;
		battleCleanupTarget.reset();
		owner.invalidatePaths();
		const auto * hero = activeHero();
		logGlobal->info("AHDBG POST_BATTLE_RESUME hero=%s mp=%d", hero ? hero->getNameTextID() : "<none>", hero ? hero->movementPointsRemaining() : -1);
		logGlobal->info("AutoHeroes v1.7: post-battle state refreshed; resuming automation");
		return;
	}

	if(waitingForTownUpgrade)
	{
		if(townUpgradeDelayTicks > 0)
		{
			--townUpgradeDelayTicks;
			return;
		}

		const CGHeroInstance * hero = activeHero();
		if(!hero)
		{
			waitingForTownUpgrade = false;
			mergeAfterTownUpgrade = false;
			advanceHero();
			process();
			return;
		}

		if(mergeAfterTownUpgrade)
		{
			auditTownUpgradeResults(hero);
			logArmyState(hero, "post_upgrade");
			mergeAfterTownUpgrade = false;
			const int mergeRequests = mergeDuplicateArmyStacks(hero);
			if(mergeRequests > 0)
			{
				townUpgradeDelayTicks = 20;
				logGlobal->info("AutoHeroes v1.7: requested %d duplicate-stack merges after town upgrades; waiting for server state", mergeRequests);
				return;
			}
		}
		else
		{
			logArmyState(hero, "post_merge");
		}

		waitingForTownUpgrade = false;
		recruitAfterTownUpgrades(hero);
		if(owner.showingDialog->isBusy())
		{
			waitingForDialog = true;
			return;
		}

		process();
		return;
	}

	if(waitingForMovement)
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
	logGlobal->info("AHDBG MOVE_FINISH hero=%s action=%s from=%s to=%s mpBefore=%d mpAfter=%d moved=%d",
		hero->getNameTextID(), activeAction ? actionName(*activeAction) : "none", movementStartPosition.toString(),
		hero->visitablePos().toString(), movementStartPoints, hero->movementPointsRemaining(), moved ? 1 : 0);
	if(!moved)
	{
		++diagnosticsAnomalies;
		logGlobal->warn("AHDBG ANOMALY type=no_movement_progress hero=%s action=%s", hero->getNameTextID(), activeAction ? actionName(*activeAction) : "none");
		logGlobal->warn("AutoHeroes v0.8: hero %s did not make progress; skipping it to avoid a loop", hero->getNameTextID());
		advanceHero();
		return;
	}

	if(activeLevelObject && activeLevelDestination && hero->visitablePos() == *activeLevelDestination)
	{
		attemptedLevelObjects.insert(*activeLevelObject);
		logGlobal->info("AHDBG LEVEL_ATTEMPT hero=%s objectId=%d coord=%s result=ARRIVED",
			hero->getNameTextID(), activeLevelObject->getNum(), activeLevelDestination->toString());
		activeLevelObject.reset();
		activeLevelDestination.reset();
	}

	if(activeSpellbookAcquisition && activeSpellbookTown)
	{
		const auto * town = hero->getVisitedTown();
		if(town && town->id == *activeSpellbookTown)
		{
			if(requestSpellbookPurchase(hero, town))
				return;
			activeSpellbookAcquisition = false;
			activeSpellbookTown.reset();
		}
	}

	if(activeShrineObject && activeShrineDestination && hero->visitablePos() == *activeShrineDestination)
	{
		pendingShrineObject = activeShrineObject;
		pendingShrineSpell = activeShrineSpell;
		pendingShrineKnownSpellsBefore = activeShrineKnownSpellsBefore;
		shrineVerifyTicks = 8;
		logGlobal->info("AHDBG SHRINE_VERIFY_WAIT hero=%s objectId=%d spell=%d ticks=%d",
			hero->getNameTextID(), pendingShrineObject ? pendingShrineObject->getNum() : -1,
			pendingShrineSpell ? pendingShrineSpell->getNum() : -1, shrineVerifyTicks);
		activeShrineObject.reset();
		activeShrineDestination.reset();
		activeShrineSpell.reset();
		activeShrineKnownSpellsBefore = 0;
	}

	if(activeAction && *activeAction == AutoHeroes::Action::RECRUIT_CREATURES)
	{
		const int upgradeRequests = upgradeArmyInCurrentTown(hero);
		if(upgradeRequests > 0)
		{
			waitingForTownUpgrade = true;
			townUpgradeDelayTicks = 20;
			mergeAfterTownUpgrade = true;
			logGlobal->info("AutoHeroes v1.7: dispatched %d army upgrade requests before town recruitment; waiting for authoritative server state", upgradeRequests);
			return;
		}

		const int mergeRequests = mergeDuplicateArmyStacks(hero);
		if(mergeRequests > 0)
		{
			waitingForTownUpgrade = true;
			townUpgradeDelayTicks = 20;
			mergeAfterTownUpgrade = false;
			logGlobal->info("AutoHeroes v1.7: requested %d duplicate-stack merges before town recruitment; waiting for server state", mergeRequests);
			return;
		}

		recruitAfterTownUpgrades(hero);
	}

	const bool possibleEncounter = activeAction && (*activeAction == AutoHeroes::Action::FIGHT_NEUTRALS || activeBattleGuard.has_value());
	if(possibleEncounter)
	{
		encounterGraceTicks = 4;
		logGlobal->info("AHDBG ENCOUNTER_GRACE_START hero=%s action=%s guard=%s ticks=%d",
			hero->getNameTextID(), activeAction ? actionName(*activeAction) : "none",
			activeBattleGuard ? activeBattleGuard->toString() : "<none>", encounterGraceTicks);
	}

	if(owner.showingDialog->isBusy())
	{
		waitingForDialog = true;
		return;
	}

	if(possibleEncounter)
		return;

	process();
}

void AutoHeroController::finishRun()
{
	const bool shouldEndTurn = endTurnAfterRun;

	running = false;
	waitingForMovement = false;
	waitingForDialog = false;
	waitingForBattle = false;
	pendingQueryReplies = 0;
	querySettleTicks = 0;
	encounterGraceTicks = 0;
	battleCleanupTicks = 0;
	battleCleanupTarget.reset();
	waitingForTownUpgrade = false;
	townUpgradeDelayTicks = 0;
	mergeAfterTownUpgrade = false;
	endTurnAfterRun = false;
	heroQueue.clear();
	heroQueueIndex = 0;
	activeHeroId.reset();
	activeAction.reset();
	activeBattleGuard.reset();
	activeCollectTreasureChest = false;
	activeSpellbookAcquisition = false;
	activeSpellbookTown.reset();
	waitingForSpellbook = false;
	spellbookWaitTicks = 0;
	spellbookKnownSpellsBefore = 0;
	attemptedSpellbookTowns.clear();
	attemptedLevelObjects.clear();
	activeLevelObject.reset();
	activeLevelDestination.reset();
	attemptedShrines.clear();
	activeShrineObject.reset();
	activeShrineDestination.reset();
	activeShrineSpell.reset();
	activeShrineKnownSpellsBefore = 0;
	pendingShrineObject.reset();
	pendingShrineSpell.reset();
	pendingShrineKnownSpellsBefore = 0;
	shrineVerifyTicks = 0;
	stepsForCurrentHero = 0;
	pendingUpgradeAudit.clear();

	if(diagnosticsBattlesStarted != diagnosticsBattlesFinished)
	{
		++diagnosticsAnomalies;
		logGlobal->warn("AHDBG ANOMALY type=battle_count_mismatch started=%d finished=%d", diagnosticsBattlesStarted, diagnosticsBattlesFinished);
	}
	logGlobal->info("AHDBG RUN_SUMMARY actions=%d moves=%d battlesStarted=%d battlesFinished=%d upgradeRequests=%d mergeRequests=%d recruited=%d anomalies=%d",
		diagnosticsActions, diagnosticsMoves, diagnosticsBattlesStarted, diagnosticsBattlesFinished, diagnosticsUpgradeRequests, diagnosticsMergeRequests, diagnosticsRecruitAmount, diagnosticsAnomalies);
	logGlobal->info("AutoHeroes v1.7: local controller finished%s", shouldEndTurn ? "; continuing End Turn" : "; human turn remains active");
	owner.onAutoHeroesFinished(shouldEndTurn);
}

void AutoHeroController::advanceHero()
{
	activeHeroId.reset();
	activeAction.reset();
	activeBattleGuard.reset();
	activeCollectTreasureChest = false;
	activeSpellbookAcquisition = false;
	activeSpellbookTown.reset();
	waitingForSpellbook = false;
	spellbookWaitTicks = 0;
	spellbookKnownSpellsBefore = 0;
	attemptedSpellbookTowns.clear();
	attemptedLevelObjects.clear();
	activeLevelObject.reset();
	activeLevelDestination.reset();
	attemptedShrines.clear();
	activeShrineObject.reset();
	activeShrineDestination.reset();
	activeShrineSpell.reset();
	activeShrineKnownSpellsBefore = 0;
	pendingShrineObject.reset();
	pendingShrineSpell.reset();
	pendingShrineKnownSpellsBefore = 0;
	shrineVerifyTicks = 0;
	encounterGraceTicks = 0;
	pendingUpgradeAudit.clear();
	waitingForTownUpgrade = false;
	townUpgradeDelayTicks = 0;
	mergeAfterTownUpgrade = false;
	stepsForCurrentHero = 0;
	++heroQueueIndex;
}

void AutoHeroController::process()
{
	if(!running || waitingForMovement || waitingForBattle || waitingForSpellbook || pendingQueryReplies > 0 || querySettleTicks > 0 || battleCleanupTicks > 0 || waitingForTownUpgrade)
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
		{
			activeHeroId = heroQueue[heroQueueIndex];
			const CGHeroInstance * startingHero = activeHero();
			if(startingHero)
			{
				const auto cfg = AutoHeroes::readHeroConfig(startingHero->id);
				logGlobal->info("AHDBG HERO_START hero=%s id=%d pos=%s mp=%d radius=%d budget=%d recruitLocation=%d combat=%d decision=%d chest=%d secondarySkills=%d",
					startingHero->getNameTextID(), startingHero->id.getNum(), startingHero->visitablePos().toString(), startingHero->movementPointsRemaining(),
					cfg.movementRadius, AutoHeroes::recruitmentBudgetPercent(cfg.recruitmentBudget), static_cast<int>(cfg.recruitmentLocation), static_cast<int>(cfg.combatPolicy),
					static_cast<int>(cfg.decisionPolicy), static_cast<int>(cfg.treasureChestChoice), cfg.allowSecondarySkillLearning ? 1 : 0);
				logGlobal->info("AHDBG MAGIC_STATE hero=%s hasSpellbook=%d maxSpellLevel=%d knownSpells=%d",
					startingHero->getNameTextID(), startingHero->hasSpellbook() ? 1 : 0, startingHero->maxSpellLevel(),
					static_cast<int>(startingHero->getSpellsInSpellbook().size()));
				logGlobal->info("AHDBG PRIORITY hero=%s collect=%d level=%d recruit=%d explore=%d capture=%d fight=%d localRadius=%d",
					startingHero->getNameTextID(),
					cfg.priorityOf(AutoHeroes::Action::COLLECT_RESOURCES) + 1,
					cfg.priorityOf(AutoHeroes::Action::LEVEL_UP) + 1,
					cfg.priorityOf(AutoHeroes::Action::RECRUIT_CREATURES) + 1,
					cfg.priorityOf(AutoHeroes::Action::EXPLORE) + 1,
					cfg.priorityOf(AutoHeroes::Action::CAPTURE_OBJECTS) + 1,
					cfg.priorityOf(AutoHeroes::Action::FIGHT_NEUTRALS) + 1,
					LOCAL_OPPORTUNITY_RADIUS);
				logArmyState(startingHero, "hero_start");
			}
		}

		const CGHeroInstance * hero = activeHero();
		if(hero && pendingShrineObject)
		{
			if(shrineVerifyTicks > 0)
			{
				--shrineVerifyTicks;
				return;
			}
			evaluatePendingShrineResult(hero);
		}
		if(!hero || hero->isGarrisoned() || hero->movementPointsRemaining() <= 100 || !AutoHeroes::isHeroEnabled(hero->id))
		{
			advanceHero();
			continue;
		}

		// Safety valve for maps that keep producing the same revisitable target.
		if(stepsForCurrentHero >= 24)
		{
			++diagnosticsAnomalies;
			logGlobal->warn("AHDBG ANOMALY type=step_limit hero=%s steps=%d", hero->getNameTextID(), stepsForCurrentHero);
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
	const auto paths = owner.getPathsInfo(hero);
	const bool splitRoles = hasMultipleAutoHeroes();
	const bool combatRole = isCombatHero(hero);

	struct ActionCandidate
	{
		AutoHeroes::Action action = AutoHeroes::Action::COLLECT_RESOURCES;
		int priorityRank = -1;
		std::optional<int3> destination;
		bool allowDestinationBattle = false;
		std::optional<int3> allowedBattleGuard;
		bool spellbook = false;
		int turns = -1;
		float cost = -1.0f;
		int tileDistance = std::numeric_limits<int>::max();
	};

	auto fillMetrics = [&](ActionCandidate & candidate)
	{
		if(!candidate.destination)
			return;

		candidate.tileDistance = localTileDistance(hero->visitablePos(), *candidate.destination);
		if(*candidate.destination == hero->visitablePos())
		{
			candidate.turns = 0;
			candidate.cost = 0.0f;
			return;
		}

		if(paths)
		{
			if(const auto * node = paths->getPathInfo(*candidate.destination))
			{
				candidate.turns = node->turns;
				candidate.cost = node->cost;
			}
		}
	};

	auto destinationIsBetter = [&](const int3 & lhs, const int3 & rhs)
	{
		int lhsTurns = lhs == hero->visitablePos() ? 0 : std::numeric_limits<int>::max();
		int rhsTurns = rhs == hero->visitablePos() ? 0 : std::numeric_limits<int>::max();
		float lhsCost = lhs == hero->visitablePos() ? 0.0f : std::numeric_limits<float>::max();
		float rhsCost = rhs == hero->visitablePos() ? 0.0f : std::numeric_limits<float>::max();

		if(paths)
		{
			if(const auto * node = paths->getPathInfo(lhs))
			{
				lhsTurns = node->turns;
				lhsCost = node->cost;
			}
			if(const auto * node = paths->getPathInfo(rhs))
			{
				rhsTurns = node->turns;
				rhsCost = node->cost;
			}
		}

		if(lhsTurns != rhsTurns)
			return lhsTurns < rhsTurns;
		if(lhsCost != rhsCost)
			return lhsCost < rhsCost;
		return localTileDistance(hero->visitablePos(), lhs) < localTileDistance(hero->visitablePos(), rhs);
	};

	std::optional<ActionCandidate> firstPriorityFallback;
	std::optional<ActionCandidate> selected;

	for(size_t rank = 0; rank < config.priority.size(); ++rank)
	{
		const auto action = config.priority[rank];
		if(!config.allows(action))
			continue;

		if(splitRoles)
		{
			if(combatRole && (action == AutoHeroes::Action::COLLECT_RESOURCES || action == AutoHeroes::Action::LEVEL_UP))
				continue;
			if(!combatRole && (action == AutoHeroes::Action::RECRUIT_CREATURES
				|| action == AutoHeroes::Action::EXPLORE
				|| action == AutoHeroes::Action::CAPTURE_OBJECTS
				|| action == AutoHeroes::Action::FIGHT_NEUTRALS))
				continue;
		}

		ActionCandidate candidate;
		candidate.action = action;
		candidate.priorityRank = static_cast<int>(rank) + 1;

		switch(action)
		{
		case AutoHeroes::Action::COLLECT_RESOURCES:
			candidate.destination = findCollectTarget(hero, config);
			break;
		case AutoHeroes::Action::LEVEL_UP:
		{
			const auto levelTarget = findLevelTarget(hero, config);
			const auto keymasterTarget = findKeymasterTarget(hero, config);
			const auto spellbookTarget = findSpellbookTarget(hero, config);
			std::optional<int3> usefulTarget = levelTarget;
			if(keymasterTarget && (!usefulTarget || destinationIsBetter(*keymasterTarget, *usefulTarget)))
				usefulTarget = keymasterTarget;
			if(spellbookTarget && (!usefulTarget || destinationIsBetter(*spellbookTarget, *usefulTarget)))
			{
				candidate.destination = spellbookTarget;
				candidate.spellbook = true;
			}
			else
			{
				candidate.destination = usefulTarget;
			}
			break;
		}
		case AutoHeroes::Action::RECRUIT_CREATURES:
			candidate.destination = findRecruitTarget(hero, config);
			break;
		case AutoHeroes::Action::EXPLORE:
			candidate.destination = findExploreTarget(hero, config);
			break;
		case AutoHeroes::Action::CAPTURE_OBJECTS:
		{
			const auto captureTarget = findCaptureTarget(hero, config);
			const auto borderGuardTarget = findUnlockedBorderGuardTarget(hero, config);
			if(borderGuardTarget && (!captureTarget || destinationIsBetter(*borderGuardTarget, *captureTarget)))
				candidate.destination = borderGuardTarget;
			else
				candidate.destination = captureTarget;
			break;
		}
		case AutoHeroes::Action::FIGHT_NEUTRALS:
			candidate.destination = findFightTarget(hero, config);
			candidate.allowDestinationBattle = candidate.destination.has_value();
			if(candidate.destination)
				candidate.allowedBattleGuard = *candidate.destination;
			break;
		}

		fillMetrics(candidate);
		const bool isLocal = candidate.destination && candidate.tileDistance <= LOCAL_OPPORTUNITY_RADIUS;
		logGlobal->info("AHDBG ACTION_EVAL hero=%s action=%s priorityRank=%d result=%s target=%s turns=%d cost=%.1f tileDistance=%d local=%d spellbook=%d",
			hero->getNameTextID(), actionName(action), candidate.priorityRank,
			candidate.destination ? "TARGET" : "NONE", candidate.destination ? candidate.destination->toString() : "<none>",
			candidate.turns, candidate.cost, candidate.destination ? candidate.tileDistance : -1, isLocal ? 1 : 0, candidate.spellbook ? 1 : 0);

		if(!candidate.destination)
			continue;
		if(isLocal)
		{
			selected = candidate;
			break;
		}
		if(!firstPriorityFallback)
			firstPriorityFallback = candidate;
	}

	if(!selected)
		selected = firstPriorityFallback;
	if(!selected || !selected->destination)
		return false;

	ActionCandidate candidate = *selected;
	const int3 destination = *candidate.destination;
	const bool localSelection = candidate.tileDistance <= LOCAL_OPPORTUNITY_RADIUS;
	const char * selectionReason = localSelection ? "local_opportunity" : "priority_fallback";

	if(candidate.action == AutoHeroes::Action::CAPTURE_OBJECTS)
	{
		for(const auto * object : owner.cb->getVisitableObjs(destination))
		{
			if(isEmptyNeutralTown(object))
			{
				candidate.allowDestinationBattle = true;
				break;
			}
		}
	}

	activeAction = candidate.action;
	activeBattleGuard.reset();
	activeCollectTreasureChest = false;
	activeSpellbookAcquisition = false;
	activeSpellbookTown.reset();
	activeLevelObject.reset();
	activeLevelDestination.reset();
	activeShrineObject.reset();
	activeShrineDestination.reset();
	activeShrineSpell.reset();
	activeShrineKnownSpellsBefore = 0;

	if(candidate.action == AutoHeroes::Action::COLLECT_RESOURCES)
	{
		const int3 guardingCreature = owner.cb->guardingCreaturePosition(destination);
		if(guardingCreature != int3(-1, -1, -1))
		{
			candidate.allowedBattleGuard = guardingCreature;
			activeBattleGuard = guardingCreature;
			candidate.allowDestinationBattle = true;
		}

		for(const auto * object : owner.cb->getVisitableObjs(destination))
		{
			if(object && (object->ID == Obj::TREASURE_CHEST || object->ID == Obj::SEA_CHEST))
			{
				activeCollectTreasureChest = true;
				break;
			}
		}
	}
	if(candidate.action == AutoHeroes::Action::FIGHT_NEUTRALS && candidate.allowedBattleGuard)
		activeBattleGuard = candidate.allowedBattleGuard;

	const CGTownInstance * spellbookTown = nullptr;
	if(candidate.spellbook)
	{
		for(const auto * object : owner.cb->getVisitableObjs(destination))
		{
			const auto * town = dynamic_cast<const CGTownInstance *>(object);
			if(town && town->mageGuildLevel() > 0)
			{
				spellbookTown = town;
				break;
			}
		}

		if(!spellbookTown)
		{
			++diagnosticsAnomalies;
			logGlobal->warn("AHDBG ANOMALY type=spellbook_target_missing_town hero=%s target=%s", hero->getNameTextID(), destination.toString());
			activeAction.reset();
			return false;
		}

		activeSpellbookAcquisition = true;
		activeSpellbookTown = spellbookTown->id;
	}
	else if(candidate.action == AutoHeroes::Action::LEVEL_UP)
	{
		for(const auto * object : owner.cb->getVisitableObjs(destination))
		{
			if(!isLevelUpTarget(object, config.allowSecondarySkillLearning))
				continue;

			activeLevelObject = object->id;
			activeLevelDestination = destination;
			if(isMagicShrineTarget(object))
			{
				activeShrineObject = object->id;
				activeShrineDestination = destination;
				activeShrineSpell = magicShrineSpell(object);
				activeShrineKnownSpellsBefore = static_cast<int>(hero->getSpellsInSpellbook().size());
			}
			break;
		}
	}

	++diagnosticsActions;
	logGlobal->info("AHDBG ACTION_SELECT seq=%d hero=%s action=%s target=%s priorityRank=%d turns=%d cost=%.1f tileDistance=%d local=%d selectionReason=%s spellbook=%d guarded=%d guard=%s mp=%d",
		diagnosticsActions, hero->getNameTextID(), actionName(candidate.action), destination.toString(),
		candidate.priorityRank, candidate.turns, candidate.cost, candidate.tileDistance, localSelection ? 1 : 0, selectionReason,
		candidate.spellbook ? 1 : 0, activeBattleGuard ? 1 : 0,
		activeBattleGuard ? activeBattleGuard->toString() : "<none>", hero->movementPointsRemaining());
	logGlobal->info("AutoHeroes v1.7: hero %s selected action=%s target=%s reason=%s%s",
		hero->getNameTextID(), actionName(candidate.action), destination.toString(), selectionReason,
		candidate.spellbook ? " get-spellbook" : (activeBattleGuard ? " with guarded battle" : ""));

	if(candidate.spellbook && destination == hero->visitablePos())
	{
		if(requestSpellbookPurchase(hero, spellbookTown))
			return true;
		activeSpellbookAcquisition = false;
		activeSpellbookTown.reset();
		activeAction.reset();
		return false;
	}

	if(startMovement(hero, destination, candidate.allowDestinationBattle, candidate.allowedBattleGuard))
		return true;

	activeAction.reset();
	activeBattleGuard.reset();
	activeCollectTreasureChest = false;
	activeSpellbookAcquisition = false;
	activeSpellbookTown.reset();
	activeLevelObject.reset();
	activeLevelDestination.reset();
	activeShrineObject.reset();
	activeShrineDestination.reset();
	activeShrineSpell.reset();
	activeShrineKnownSpellsBefore = 0;
	return false;
}

bool AutoHeroController::startMovement(const CGHeroInstance * hero, const int3 & destination, bool allowDestinationBattle, const std::optional<int3> & allowedBattleGuard)
{
	if(!hero || destination == hero->visitablePos())
		return false;

	const auto paths = owner.getPathsInfo(hero);
	if(!paths)
		return false;

	CGPath path;
	if(!paths->getPath(path, destination, EPathfindingLayer::AUTO))
		return false;

	if(!path.hasNextNode() || path.nextNode().turns != 0 || !pathIsSafeForMvp(hero, path, destination, allowDestinationBattle, allowedBattleGuard))
		return false;

	const CGPathNode * destinationNode = paths->getPathInfo(destination);
	const int destinationTurns = destinationNode ? destinationNode->turns : -1;

	owner.localState->setPath(hero, path);
	owner.localState->setSelection(hero);

	movementStartPosition = hero->visitablePos();
	movementStartPoints = hero->movementPointsRemaining();
	++stepsForCurrentHero;
	++diagnosticsMoves;
	waitingForMovement = true;

	logGlobal->info("AHDBG MOVE_REQUEST seq=%d hero=%s action=%s from=%s target=%s mp=%d destinationTurns=%d allowBattle=%d guard=%s",
		diagnosticsMoves, hero->getNameTextID(), activeAction ? actionName(*activeAction) : "none", movementStartPosition.toString(), destination.toString(),
		movementStartPoints, destinationTurns, allowDestinationBattle ? 1 : 0, allowedBattleGuard ? allowedBattleGuard->toString() : "<none>");
	logGlobal->info("AutoHeroes v1.0: moving hero %s from %s to %s (MP=%d, destination turns=%d)", hero->getNameTextID(), movementStartPosition.toString(), destination.toString(), movementStartPoints, destinationTurns);
	owner.moveHero(hero, path);

	if(!owner.isHeroMoving())
	{
		waitingForMovement = false;
		++diagnosticsAnomalies;
		logGlobal->warn("AHDBG ANOMALY type=move_not_started hero=%s target=%s", hero->getNameTextID(), destination.toString());
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
	int bestTurns = std::numeric_limits<int>::max();
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

					const bool diagnoseResource = object->ID == Obj::RESOURCE || object->ID == Obj::RANDOM_RESOURCE;
					const int3 destination = object->visitablePos();
					auto logResourceReject = [&](const char * reason)
					{
						if(diagnoseResource)
							logGlobal->info("AutoHeroes v1.4 collect resource candidate subtype=%s coord=%s result=REJECT reason=%s",
								object->getSubtypeName(), destination.toString(), reason);
					};

					if(object->wasVisited(hero))
					{
						logResourceReject("visited");
						continue;
					}
					if(destination == hero->visitablePos())
					{
						logResourceReject("same_position");
						continue;
					}
					if(!withinConfiguredRadius(hero, config, destination))
					{
						logResourceReject("outside_radius");
						continue;
					}
					std::optional<int3> allowedBattleGuard;
					const int3 guardingCreature = owner.cb->guardingCreaturePosition(destination);
					if(guardingCreature != int3(-1, -1, -1))
					{
						if(hasMultipleAutoHeroes() && !isCombatHero(hero))
						{
							logResourceReject("guarded_reserved_for_combat_hero");
							continue;
						}
						if(config.combatPolicy == AutoHeroes::CombatPolicy::DISABLED)
						{
							logResourceReject("guarded_combat_disabled");
							continue;
						}

						const CArmedInstance * guardArmy = nullptr;
						for(const auto * guardObject : owner.cb->getVisitableObjs(guardingCreature))
						{
							if(guardObject && guardObject->ID == Obj::MONSTER)
							{
								guardArmy = dynamic_cast<const CArmedInstance *>(guardObject);
								if(guardArmy)
									break;
							}
						}

						if(!guardArmy)
						{
							logResourceReject("guarded_unknown_guard");
							continue;
						}

						const double heroStrength = static_cast<double>(std::max<ui64>(1, hero->estimateHeroCombatValue()));
						const double enemyStrength = static_cast<double>(std::max<ui64>(1, guardArmy->estimateCombatValue()));
						const double requiredRatio = config.combatPolicy == AutoHeroes::CombatPolicy::SAFE_ONLY ? 1.50 : 1.15;
						const double ratio = heroStrength / enemyStrength;
						if(ratio < requiredRatio)
						{
							if(diagnoseResource)
								logGlobal->info("AutoHeroes v1.5 guarded resource subtype=%s coord=%s guard=%s ratio=%.3f required=%.2f result=REJECT reason=guard_too_strong",
									object->getSubtypeName(), destination.toString(), guardingCreature.toString(), ratio, requiredRatio);
							continue;
						}

						allowedBattleGuard = guardingCreature;
						if(diagnoseResource)
							logGlobal->info("AutoHeroes v1.5 guarded resource subtype=%s coord=%s guard=%s ratio=%.3f required=%.2f result=ALLOW_BATTLE_ENTRY",
								object->getSubtypeName(), destination.toString(), guardingCreature.toString(), ratio, requiredRatio);
					}

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node)
					{
						logResourceReject("no_path_info");
						continue;
					}
					if(!node->reachable())
					{
						logResourceReject("unreachable");
						continue;
					}
					if(node->turns > bestTurns || (node->turns == bestTurns && node->cost >= bestCost))
					{
						logResourceReject("farther_than_current_best");
						continue;
					}

					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO))
					{
						logResourceReject("path_not_found");
						continue;
					}
					if(!pathIsSafeForMvp(hero, path, destination, allowedBattleGuard.has_value(), allowedBattleGuard))
					{
						logResourceReject("path_unsafe");
						continue;
					}

					if(diagnoseResource)
						logGlobal->info("AutoHeroes v1.4 collect resource candidate subtype=%s coord=%s turns=%d cost=%.1f result=ACCEPT",
							object->getSubtypeName(), destination.toString(), node->turns, node->cost);

					best = destination;
					bestTurns = node->turns;
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
					if(!isLevelUpTarget(object, config.allowSecondarySkillLearning))
						continue;

					if(attemptedLevelObjects.count(object->id))
					{
						logGlobal->info("AHDBG LEVEL_REJECT hero=%s object=%s objectId=%d coord=%s reason=already_attempted_run",
							hero->getNameTextID(), object->getSubtypeName(), object->id.getNum(), object->visitablePos().toString());
						continue;
					}

					std::optional<SpellID> shrineSpell;
					int shrineSpellLevel = -1;
					if(isMagicShrineTarget(object))
					{
						shrineSpell = magicShrineSpell(object);
						shrineSpellLevel = magicShrineLevel(object);

						if(attemptedShrines.count(object->id))
						{
							logGlobal->info("AHDBG SHRINE_REJECT hero=%s object=%s coord=%s spell=%d spellLevel=%d reason=already_attempted_run",
								hero->getNameTextID(), object->getSubtypeName(), object->visitablePos().toString(),
								shrineSpell ? shrineSpell->getNum() : -1, shrineSpellLevel);
							continue;
						}
						if(!hero->hasSpellbook())
						{
							logGlobal->info("AHDBG SHRINE_REJECT hero=%s object=%s coord=%s spell=%d spellLevel=%d reason=no_spellbook",
								hero->getNameTextID(), object->getSubtypeName(), object->visitablePos().toString(),
								shrineSpell ? shrineSpell->getNum() : -1, shrineSpellLevel);
							continue;
						}
						if(shrineSpell && hero->spellbookContainsSpell(*shrineSpell))
						{
							logGlobal->info("AHDBG SHRINE_REJECT hero=%s object=%s coord=%s spell=%d spellLevel=%d reason=already_known",
								hero->getNameTextID(), object->getSubtypeName(), object->visitablePos().toString(),
								shrineSpell->getNum(), shrineSpellLevel);
							continue;
						}
						if(shrineSpellLevel > hero->maxSpellLevel())
						{
							logGlobal->info("AHDBG SHRINE_REJECT hero=%s object=%s coord=%s spell=%d spellLevel=%d maxSpellLevel=%d reason=wisdom_too_low",
								hero->getNameTextID(), object->getSubtypeName(), object->visitablePos().toString(),
								shrineSpell ? shrineSpell->getNum() : -1, shrineSpellLevel, hero->maxSpellLevel());
							continue;
						}

						const auto * rewardable = dynamic_cast<const CRewardableObject *>(object);
						if(!rewardable)
						{
							logGlobal->warn("AHDBG SHRINE_REJECT hero=%s object=%s coord=%s reason=not_rewardable",
								hero->getNameTextID(), object->getSubtypeName(), object->visitablePos().toString());
							continue;
						}
						const auto available = rewardable->getAvailableRewards(hero, Rewardable::EEventType::EVENT_FIRST_VISIT);
						if(available.empty())
						{
							logGlobal->info("AHDBG SHRINE_REJECT hero=%s object=%s coord=%s spell=%d spellLevel=%d reason=no_available_reward",
								hero->getNameTextID(), object->getSubtypeName(), object->visitablePos().toString(),
								shrineSpell ? shrineSpell->getNum() : -1, shrineSpellLevel);
							continue;
						}
					}
					else if(object->wasVisited(hero))
					{
						continue;
					}

					if(object->ID == Obj::LIBRARY_OF_ENLIGHTENMENT)
					{
						const auto * rewardable = dynamic_cast<const CRewardableObject *>(object);
						if(!rewardable)
						{
							logGlobal->warn("AHDBG LEVEL_REJECT hero=%s object=%s objectId=%d coord=%s reason=library_not_rewardable",
								hero->getNameTextID(), object->getSubtypeName(), object->id.getNum(), object->visitablePos().toString());
							continue;
						}

						const auto available = rewardable->getAvailableRewards(hero, Rewardable::EEventType::EVENT_FIRST_VISIT);
						if(available.empty())
						{
							logGlobal->info("AHDBG LEVEL_REJECT hero=%s object=%s objectId=%d coord=%s reason=reward_limiter",
								hero->getNameTextID(), object->getSubtypeName(), object->id.getNum(), object->visitablePos().toString());
							continue;
						}
					}

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

					if(isMagicShrineTarget(object))
					{
						logGlobal->info("AHDBG SHRINE_CANDIDATE hero=%s object=%s coord=%s spell=%d spellLevel=%d maxSpellLevel=%d turns=%d cost=%.1f result=ACCEPT",
							hero->getNameTextID(), object->getSubtypeName(), destination.toString(),
							shrineSpell ? shrineSpell->getNum() : -1, shrineSpellLevel, hero->maxSpellLevel(), node->turns, node->cost);
					}

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

std::optional<int3> AutoHeroController::findKeymasterTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	const auto paths = owner.getPathsInfo(hero);
	const auto player = owner.cb ? owner.cb->getPlayerID() : std::optional<PlayerColor>{};
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
					const auto * tent = dynamic_cast<const KeymasterTent *>(object);
					if(!tent || tent->wasVisited(*player))
						continue;

					const int3 destination = tent->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;
					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;
					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO)
						|| !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					logGlobal->info("AHDBG KEYMASTER_CANDIDATE hero=%s objectId=%d color=%d coord=%s turns=%d cost=%.1f result=ACCEPT",
						hero->getNameTextID(), tent->id.getNum(), tent->subID.getNum(), destination.toString(), node->turns, node->cost);

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

std::optional<int3> AutoHeroController::findUnlockedBorderGuardTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
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
					const auto * guard = dynamic_cast<const QuestGuard *>(object);
					if(!guard)
						continue;
					const auto & quest = guard->getQuest();
					if(quest.mission.requiredKeys.empty() || quest.isCompleted || !guard->checkQuest(hero))
						continue;

					const int3 destination = guard->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;
					if(owner.cb->guardingCreaturePosition(destination) != int3(-1, -1, -1))
						continue;
					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;
					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO)
						|| !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					logGlobal->info("AHDBG BORDER_GUARD_CANDIDATE hero=%s objectId=%d coord=%s turns=%d cost=%.1f result=ACCEPT",
						hero->getNameTextID(), guard->id.getNum(), destination.toString(), node->turns, node->cost);

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

std::optional<int3> AutoHeroController::findSpellbookTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const
{
	if(!hero || !owner.cb || hero->hasSpellbook())
		return std::nullopt;

	const auto player = owner.cb->getPlayerID();
	const auto paths = owner.getPathsInfo(hero);
	if(!player || !paths)
		return std::nullopt;

	const int currentGold = owner.cb->getResourceAmount(EGameResID::GOLD);
	if(currentGold < GameConstants::SPELLBOOK_GOLD_COST)
	{
		logGlobal->info("AHDBG SPELLBOOK_TARGET hero=%s result=NONE reason=not_enough_gold gold=%d cost=%d",
			hero->getNameTextID(), currentGold, GameConstants::SPELLBOOK_GOLD_COST);
		return std::nullopt;
	}

	if(const auto * currentTown = hero->getVisitedTown())
	{
		if(currentTown->getOwner() == *player && currentTown->mageGuildLevel() > 0 && !attemptedSpellbookTowns.count(currentTown->id))
		{
			logGlobal->info("AHDBG SPELLBOOK_TARGET hero=%s town=%s mageGuildLevel=%d turns=0 cost=0.0 result=ACCEPT reason=current_town",
				hero->getNameTextID(), currentTown->visitablePos().toString(), currentTown->mageGuildLevel());
			return hero->visitablePos();
		}
	}

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
					const auto * town = dynamic_cast<const CGTownInstance *>(object);
					if(!town || town->getOwner() != *player || town->mageGuildLevel() <= 0)
						continue;
					if(attemptedSpellbookTowns.count(town->id))
						continue;

					const int3 destination = town->visitablePos();
					if(destination == hero->visitablePos() || !withinConfiguredRadius(hero, config, destination))
						continue;

					const CGPathNode * node = paths->getPathInfo(destination);
					if(!node || !node->reachable())
						continue;

					CGPath path;
					if(!paths->getPath(path, destination, EPathfindingLayer::AUTO) || !pathIsSafeForMvp(hero, path, destination, false))
						continue;

					logGlobal->info("AHDBG SPELLBOOK_TARGET hero=%s town=%s mageGuildLevel=%d turns=%d cost=%.1f result=ACCEPT",
						hero->getNameTextID(), destination.toString(), town->mageGuildLevel(), node->turns, node->cost);

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

bool AutoHeroController::requestSpellbookPurchase(const CGHeroInstance * hero, const CGTownInstance * town)
{
	if(!hero || !town || !owner.cb || hero->hasSpellbook())
		return false;

	const auto player = owner.cb->getPlayerID();
	if(!player || town->getOwner() != *player || town->mageGuildLevel() <= 0)
		return false;
	if(hero->getVisitedTown() != town)
	{
		logGlobal->warn("AHDBG SPELLBOOK_REQUEST hero=%s town=%s result=SKIP reason=hero_not_registered_in_town",
			hero->getNameTextID(), town->visitablePos().toString());
		attemptedSpellbookTowns.insert(town->id);
		return false;
	}

	const int currentGold = owner.cb->getResourceAmount(EGameResID::GOLD);
	if(currentGold < GameConstants::SPELLBOOK_GOLD_COST)
	{
		logGlobal->info("AHDBG SPELLBOOK_REQUEST hero=%s town=%s result=SKIP reason=not_enough_gold gold=%d cost=%d",
			hero->getNameTextID(), town->visitablePos().toString(), currentGold, GameConstants::SPELLBOOK_GOLD_COST);
		attemptedSpellbookTowns.insert(town->id);
		return false;
	}

	attemptedSpellbookTowns.insert(town->id);
	spellbookKnownSpellsBefore = static_cast<int>(hero->getSpellsInSpellbook().size());
	waitingForSpellbook = true;
	spellbookWaitTicks = 30;
	logGlobal->info("AHDBG SPELLBOOK_REQUEST hero=%s town=%s mageGuildLevel=%d gold=%d cost=%d knownBefore=%d status=DISPATCHED",
		hero->getNameTextID(), town->visitablePos().toString(), town->mageGuildLevel(), currentGold,
		GameConstants::SPELLBOOK_GOLD_COST, spellbookKnownSpellsBefore);
	owner.cb->buyArtifact(hero, ArtifactID::SPELLBOOK);
	return true;
}

void AutoHeroController::evaluatePendingShrineResult(const CGHeroInstance * hero)
{
	if(!hero || !pendingShrineObject)
		return;

	const int knownAfter = static_cast<int>(hero->getSpellsInSpellbook().size());
	const bool learnedExpectedSpell = pendingShrineSpell && hero->spellbookContainsSpell(*pendingShrineSpell);
	const bool gainedAnySpell = knownAfter > pendingShrineKnownSpellsBefore;
	const bool success = learnedExpectedSpell || gainedAnySpell;

	logGlobal->info("AHDBG SHRINE_VISIT_RESULT hero=%s objectId=%d spell=%d knownBefore=%d knownAfter=%d result=%s",
		hero->getNameTextID(), pendingShrineObject->getNum(), pendingShrineSpell ? pendingShrineSpell->getNum() : -1,
		pendingShrineKnownSpellsBefore, knownAfter, success ? "SUCCESS" : "NO_GAIN");

	if(!success)
	{
		attemptedShrines.insert(*pendingShrineObject);
		++diagnosticsAnomalies;
		logGlobal->warn("AHDBG ANOMALY type=shrine_no_gain hero=%s objectId=%d spell=%d",
			hero->getNameTextID(), pendingShrineObject->getNum(), pendingShrineSpell ? pendingShrineSpell->getNum() : -1);
	}

	pendingShrineObject.reset();
	pendingShrineSpell.reset();
	pendingShrineKnownSpellsBefore = 0;
	shrineVerifyTicks = 0;
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

int AutoHeroController::creatureAmountInArmy(const CGHeroInstance * hero, int creatureId) const
{
	if(!hero)
		return 0;

	int amount = 0;
	for(const auto & stackEntry : hero->Slots())
	{
		if(!stackEntry.second || !stackEntry.second->getCreature())
			continue;
		if(stackEntry.second->getCreatureID().getNum() == creatureId)
			amount += static_cast<int>(stackEntry.second->getCount());
	}
	return amount;
}

void AutoHeroController::logArmyState(const CGHeroInstance * hero, const char * stage) const
{
	if(!hero)
	{
		logGlobal->warn("AHDBG ARMY_STATE stage=%s hero=<none>", stage);
		return;
	}

	logGlobal->info("AHDBG ARMY_STATE stage=%s hero=%s slots=%d mp=%d",
		stage, hero->getNameTextID(), static_cast<int>(hero->Slots().size()), hero->movementPointsRemaining());
	int ordinal = 0;
	for(const auto & stackEntry : hero->Slots())
	{
		if(!stackEntry.second || !stackEntry.second->getCreature())
			continue;
		logGlobal->info("AHDBG ARMY_STACK stage=%s hero=%s n=%d creature=%d count=%d aiValue=%d",
			stage, hero->getNameTextID(), ordinal++, stackEntry.second->getCreatureID().getNum(),
			static_cast<int>(stackEntry.second->getCount()), stackEntry.second->getCreature()->getAIValue());
	}
}

void AutoHeroController::auditTownUpgradeResults(const CGHeroInstance * hero)
{
	for(const auto & audit : pendingUpgradeAudit)
	{
		const int fromAfter = creatureAmountInArmy(hero, audit.fromCreature);
		const int toAfter = creatureAmountInArmy(hero, audit.toCreature);
		const bool observed = fromAfter < audit.fromAmountBefore || toAfter > audit.toAmountBefore;
		logGlobal->info("AHDBG UPGRADE_VERIFY hero=%s from=%d to=%d fromBefore=%d fromAfter=%d toBefore=%d toAfter=%d result=%s",
			hero ? hero->getNameTextID() : "<none>", audit.fromCreature, audit.toCreature,
			audit.fromAmountBefore, fromAfter, audit.toAmountBefore, toAfter, observed ? "OBSERVED" : "NOT_OBSERVED");
		if(!observed)
		{
			++diagnosticsAnomalies;
			logGlobal->warn("AHDBG ANOMALY type=upgrade_not_observed hero=%s from=%d to=%d",
				hero ? hero->getNameTextID() : "<none>", audit.fromCreature, audit.toCreature);
		}
	}
	pendingUpgradeAudit.clear();
}

int AutoHeroController::upgradeArmyInCurrentTown(const CGHeroInstance * hero)
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
		if(town->getVisitingHero() != hero && town->getGarrisonHero() != hero)
			return 0;

		pendingUpgradeAudit.clear();
		logArmyState(hero, "pre_upgrade");
		const auto resourcesBefore = owner.cb->getResourceAmount();
		logGlobal->info("AHDBG TOWN_PREP_START hero=%s town=%s gold=%d",
			hero->getNameTextID(), town->visitablePos().toString(), resourcesBefore[EGameResID::GOLD]);

		int requested = 0;
		for(const auto & stackEntry : hero->Slots())
		{
			const auto * currentCreature = stackEntry.second->getCreature();
			if(!currentCreature || !currentCreature->hasUpgrades())
				continue;

			std::optional<CreatureID> bestUpgrade;
			int bestAIValue = -1;
			for(const auto & level : town->creatures)
			{
				for(const CreatureID candidate : level.second)
				{
					const auto * candidateCreature = candidate.toCreature();
					if(!candidateCreature || !currentCreature->isMyDirectUpgrade(candidateCreature))
						continue;

					const int aiValue = candidateCreature->getAIValue();
					if(!bestUpgrade || aiValue > bestAIValue)
					{
						bestUpgrade = candidate;
						bestAIValue = aiValue;
					}
				}
			}

			if(!bestUpgrade)
			{
				logGlobal->info("AHDBG UPGRADE_CANDIDATE hero=%s creature=%d result=SKIP reason=no_town_upgrade",
					hero->getNameTextID(), stackEntry.second->getCreatureID().getNum());
				continue;
			}

			const int fromId = stackEntry.second->getCreatureID().getNum();
			const int toId = bestUpgrade->getNum();
			const bool alreadyAudited = std::any_of(pendingUpgradeAudit.begin(), pendingUpgradeAudit.end(), [&](const UpgradeAudit & item)
			{
				return item.fromCreature == fromId && item.toCreature == toId;
			});
			if(!alreadyAudited)
			{
				pendingUpgradeAudit.push_back({fromId, toId, creatureAmountInArmy(hero, fromId), creatureAmountInArmy(hero, toId)});
			}

			// CCallback::upgradeCreature is asynchronous with respect to the authoritative
			// server state. In practice its immediate bool is not a reliable indication
			// that the upgrade was or was not applied. Dispatch the command, wait for the
			// network state update, then verify the army before merge/recruitment.
			const bool immediateResult = owner.cb->upgradeCreature(hero, stackEntry.first, *bestUpgrade);
			++requested;
			++diagnosticsUpgradeRequests;
			logGlobal->info("AHDBG UPGRADE_REQUEST hero=%s from=%d to=%d count=%d immediateReturn=%d status=DISPATCHED",
				hero->getNameTextID(), fromId, toId, static_cast<int>(stackEntry.second->getCount()), immediateResult ? 1 : 0);
		}

		logGlobal->info("AHDBG TOWN_UPGRADE_BATCH hero=%s town=%s requests=%d",
			hero->getNameTextID(), town->visitablePos().toString(), requested);
		return requested;
	}

	return 0;
}

int AutoHeroController::mergeDuplicateArmyStacks(const CGHeroInstance * hero)
{
	if(!hero || !owner.cb)
		return 0;

	std::vector<std::pair<CreatureID, SlotID>> canonicalStacks;
	int requested = 0;

	for(const auto & stackEntry : hero->Slots())
	{
		if(!stackEntry.second || !stackEntry.second->getCreature())
			continue;

		const CreatureID creature = stackEntry.second->getCreatureID();
		std::optional<SlotID> destinationSlot;
		for(const auto & canonical : canonicalStacks)
		{
			if(canonical.first == creature)
			{
				destinationSlot = canonical.second;
				break;
			}
		}

		if(!destinationSlot)
		{
			canonicalStacks.emplace_back(creature, stackEntry.first);
			continue;
		}

		const int immediateResult = owner.cb->mergeStacks(hero, hero, stackEntry.first, *destinationSlot);
		++requested;
		++diagnosticsMergeRequests;
		logGlobal->info("AHDBG MERGE_REQUEST hero=%s creature=%d immediateReturn=%d status=DISPATCHED",
			hero->getNameTextID(), creature.getNum(), immediateResult);
		logGlobal->info("AutoHeroes v1.7: duplicate army stack merge requested hero=%s creature=%d",
			hero->getNameTextID(), creature.getNum());
	}

	return requested;
}

void AutoHeroController::recruitAfterTownUpgrades(const CGHeroInstance * hero)
{
	if(!hero)
		return;

	const auto config = AutoHeroes::readHeroConfig(hero->id);
	logArmyState(hero, "pre_recruit");
	const int recruited = recruitFromCurrentTown(hero, config);
	if(recruited > 0)
	{
		diagnosticsRecruitAmount += recruited;
		logGlobal->info("AHDBG RECRUIT_BATCH hero=%s intendedAmount=%d", hero->getNameTextID(), recruited);
		logGlobal->info("AutoHeroes v1.7: recruited %d creatures after town-upgrade phase for hero %s", recruited, hero->getNameTextID());
		owner.closeAllDialogs();
	}
}

int AutoHeroController::recruitFromCurrentTown(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config)
{
	if(!hero || !owner.cb)
		return 0;
	if(config.recruitmentLocation == AutoHeroes::RecruitmentLocation::FIELD_ONLY)
	{
		logGlobal->info("AHDBG RECRUIT_LOCATION_SKIP hero=%s source=town mode=field_only", hero->getNameTextID());
		return 0;
	}

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

		logGlobal->info("AHDBG RECRUIT_START hero=%s town=%s slots=%d totalGold=%d budgetPercent=%d budgetGold=%d",
			hero->getNameTextID(), town->visitablePos().toString(), static_cast<int>(hero->Slots().size()), totalGold, budgetPercent, budgetGold);
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

			logGlobal->info("AHDBG RECRUIT_REQUEST hero=%s town=%s level=%d creature=%d available=%d count=%d goldCost=%d budgetGoldRemaining=%d",
				hero->getNameTextID(), town->visitablePos().toString(), levelIndex, creature.getNum(), available, count, goldCost, resources[EGameResID::GOLD]);
			logGlobal->info("AutoHeroes v1.2 recruit town=%s level=%d creature=%d available=%d count=%d goldCost=%d budgetGoldRemaining=%d result=BUY",
				town->visitablePos().toString(), levelIndex, creature.getNum(), available, count, goldCost, resources[EGameResID::GOLD]);
		}

		if(recruitedTotal > 0)
			lockTownRecruit(hero, town);

		logGlobal->info("AHDBG RECRUIT_END hero=%s town=%s intendedAmount=%d budgetGoldRemaining=%d weeklyLock=%d",
			hero->getNameTextID(), town->visitablePos().toString(), recruitedTotal, resources[EGameResID::GOLD], recruitedTotal > 0 ? 1 : 0);
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
						if(config.recruitmentLocation == AutoHeroes::RecruitmentLocation::FIELD_ONLY)
							continue;
						if(town->getOwner() != *player || attemptedRecruitTowns.count(town->id) || townRecruitLocked(hero, town))
							continue;
						if(!dwellingHasUsefulRecruit(town, hero, config))
							continue;
					}
					else
					{
						if(config.recruitmentLocation == AutoHeroes::RecruitmentLocation::TOWN_ONLY)
							continue;
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
					const bool emptyNeutralTown = isEmptyNeutralTown(object);
					const bool samePosition = destination == hero->visitablePos();
					const bool withinRadius = withinConfiguredRadius(hero, config, destination);
					const int3 guardingCreature = owner.cb->guardingCreaturePosition(destination);
					const bool externallyGuarded = guardingCreature != int3(-1, -1, -1);
					const CGPathNode * node = paths->getPathInfo(destination);
					const bool reachable = node && node->reachable();

					CGPath path;
					const bool pathFound = reachable && paths->getPath(path, destination, EPathfindingLayer::AUTO);
					// Empty neutral towns can have a destination node marked as BATTLE by
					// pathfinding even though there is no army to fight. Permit only that
					// final node; unrelated battles on the route remain forbidden.
					const bool pathSafe = pathFound
						&& pathIsSafeForMvp(hero, path, destination, emptyNeutralTown);

					const bool accepted = !samePosition && withinRadius && !externallyGuarded
						&& reachable && pathFound && pathSafe;

					if(emptyNeutralTown)
					{
						const char * reason = "accepted";
						if(samePosition)
							reason = "same_position";
						else if(!withinRadius)
							reason = "radius";
						else if(externallyGuarded)
							reason = "external_guard";
						else if(!node)
							reason = "no_path_node";
						else if(!reachable)
							reason = "unreachable";
						else if(!pathFound)
							reason = "path_not_found";
						else if(!pathSafe)
							reason = "path_unsafe";

						logGlobal->info(
							"AHDBG CAPTURE_CANDIDATE type=neutral_town coord=%s withinRadius=%d externalGuard=%d reachable=%d pathFound=%d pathSafe=%d result=%s reason=%s",
							destination.toString(), withinRadius ? 1 : 0, externallyGuarded ? 1 : 0,
							reachable ? 1 : 0, pathFound ? 1 : 0, pathSafe ? 1 : 0,
							accepted ? "ACCEPT" : "REJECT", reason);
					}

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
					const bool pathSafe = pathFound && pathIsSafeForMvp(hero, path, destination, true, std::optional<int3>{destination});

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

bool AutoHeroController::pathIsSafeForMvp(const CGHeroInstance * hero, const CGPath & path, const int3 & destination, bool allowDestinationBattle, const std::optional<int3> & allowedBattleGuard) const
{
	if(path.nodes.size() < 2)
		return false;

	for(const auto & node : path.nodes)
	{
		const bool isDestination = node.coord == destination;
		const int3 guardingCreature = owner.cb->guardingCreaturePosition(node.coord);
		const bool isAllowedGuard = allowedBattleGuard && node.coord == *allowedBattleGuard;
		const bool guardedByAllowedGuard = allowedBattleGuard && guardingCreature == *allowedBattleGuard;
		const bool allowedCombatNode = allowDestinationBattle && (isDestination || isAllowedGuard || guardedByAllowedGuard);

		if(isTeleportAction(node.action))
			return false;

		// VCMI can place BATTLE/GUARDED on an approach tile. For a direct
		// neutral fight the allowed guard is the selected monster; for guarded
		// collection it is the monster guarding the selected resource tile.
		// Any unrelated battle on the route remains forbidden.
		if(node.accessible == EPathAccessibility::GUARDED || isBattleAction(node.action))
		{
			if(!allowedCombatNode)
				return false;
		}

		if(node.coord != hero->visitablePos() && !isDestination)
		{
			if(node.action != EPathNodeAction::NORMAL && node.action != EPathNodeAction::UNKNOWN)
			{
				if(!(isBattleAction(node.action) && (isAllowedGuard || guardedByAllowedGuard)))
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
