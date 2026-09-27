/*
 * AutoHeroController.h, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#pragma once

#include "../lib/constants/EntityIdentifiers.h"
#include "../lib/int3.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

class CPlayerInterface;
class CGHeroInstance;
class CGDwelling;
class CGTownInstance;
struct CGPath;

namespace AutoHeroes
{

enum class Action;
enum class DecisionPolicy;
enum class TreasureChestChoice;
enum class RecruitmentScope;
enum class RecruitmentLocation;
enum class RecruitmentBudget;
struct HeroConfig;

}

/// Runs selected heroes through the normal human player interface.
///
/// Stage 6 deliberately does NOT replace CPlayerInterface with an AI player.
/// This keeps fog of war, map rendering and manual controls alive while an
/// automated hero uses the same movement pipeline as a manually moved hero.
class AutoHeroController
{
	CPlayerInterface & owner;
	bool running = false;
	bool waitingForMovement = false;
	bool waitingForDialog = false;
	bool waitingForBattle = false;
	int pendingQueryReplies = 0;
	int querySettleTicks = 0;
	int encounterGraceTicks = 0;
	int battleCleanupTicks = 0;
	std::optional<int3> battleCleanupTarget;
	bool waitingForTownUpgrade = false;
	int townUpgradeDelayTicks = 0;
	bool mergeAfterTownUpgrade = false;
	bool endTurnAfterRun = false;
	int transportSettleTicks = 0;
	bool transportTownPrep = false;
	bool pendingTransportDelivery = false;
	int64_t transportCarrierUnitsBefore = 0;
	int64_t transportRecipientUnitsBefore = 0;
	std::optional<ObjectInstanceID> pendingTransportRecipient;
	std::vector<ObjectInstanceID> heroQueue;
	size_t heroQueueIndex = 0;
	std::optional<ObjectInstanceID> activeHeroId;
	std::optional<ObjectInstanceID> combatHeroId;
	std::optional<AutoHeroes::Action> activeAction;
	std::optional<int3> activeBattleGuard;
	bool activeCollectTreasureChest = false;
	bool activeSpellbookAcquisition = false;
	std::optional<ObjectInstanceID> activeSpellbookTown;
	bool waitingForSpellbook = false;
	int spellbookWaitTicks = 0;
	int spellbookKnownSpellsBefore = 0;
	std::set<ObjectInstanceID> attemptedSpellbookTowns;
	std::set<ObjectInstanceID> attemptedLevelObjects;
	std::set<ObjectInstanceID> attemptedPortalEntrances;
	std::optional<ObjectInstanceID> activeLevelObject;
	std::optional<int3> activeLevelDestination;
	std::set<ObjectInstanceID> attemptedShrines;
	std::optional<ObjectInstanceID> activeShrineObject;
	std::optional<int3> activeShrineDestination;
	std::optional<SpellID> activeShrineSpell;
	int activeShrineKnownSpellsBefore = 0;
	std::optional<ObjectInstanceID> pendingShrineObject;
	std::optional<SpellID> pendingShrineSpell;
	int pendingShrineKnownSpellsBefore = 0;
	int shrineVerifyTicks = 0;
	int3 movementStartPosition = int3(-1, -1, -1);
	int movementStartPoints = -1;
	int stepsForCurrentHero = 0;
	std::set<ObjectInstanceID> attemptedRecruitTowns;
	std::set<std::pair<int, int>> weeklyRecruitTowns;

	struct UpgradeAudit
	{
		int fromCreature = -1;
		int toCreature = -1;
		int fromAmountBefore = 0;
		int toAmountBefore = 0;
	};
	std::vector<UpgradeAudit> pendingUpgradeAudit;

	int diagnosticsActions = 0;
	int diagnosticsMoves = 0;
	int diagnosticsBattlesStarted = 0;
	int diagnosticsBattlesFinished = 0;
	int diagnosticsUpgradeRequests = 0;
	int diagnosticsMergeRequests = 0;
	int diagnosticsRecruitAmount = 0;
	int diagnosticsAnomalies = 0;

	const CGHeroInstance * activeHero() const;
	bool hasMultipleAutoHeroes() const;
	bool isCombatHero(const CGHeroInstance * hero) const;
	void process();
	void finishRun();
	void advanceHero();
	bool startNextAction(const CGHeroInstance * hero);
	bool startTransporterAction(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config);
	std::optional<int3> findTransportSourceTown(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	bool loadTransporterAtCurrentTown(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config);
	bool transporterHasCargo(const CGHeroInstance * hero) const;
	int64_t armyUnitCount(const CGHeroInstance * hero) const;
	bool recipientCanAcceptTransportArmy(const CGHeroInstance * carrier, const CGHeroInstance * recipient) const;
	void setTransporterNeedsSource(const CGHeroInstance * hero, bool needsSource);
	void verifyPendingTransportDelivery();
	SlotID transporterReserveSlot(const CGHeroInstance * hero) const;
	bool dispatchTransportArmy(const CGHeroInstance * carrier, const CGHeroInstance * recipient);
	bool startMovement(const CGHeroInstance * hero, const int3 & destination, bool allowDestinationBattle = false, const std::optional<int3> & allowedBattleGuard = std::nullopt);
	std::optional<int3> findCollectTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findLevelTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findKeymasterTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findUnlockedBorderGuardTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findSpellbookTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	bool requestSpellbookPurchase(const CGHeroInstance * hero, const CGTownInstance * town);
	void evaluatePendingShrineResult(const CGHeroInstance * hero);
	std::optional<int3> findRecruitTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	bool dwellingHasUsefulRecruit(const CGDwelling * dwelling, const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	bool townRecruitLocked(const CGHeroInstance * hero, const CGTownInstance * town) const;
	void lockTownRecruit(const CGHeroInstance * hero, const CGTownInstance * town);
	int upgradeArmyInCurrentTown(const CGHeroInstance * hero);
	void auditTownUpgradeResults(const CGHeroInstance * hero);
	void logArmyState(const CGHeroInstance * hero, const char * stage) const;
	int creatureAmountInArmy(const CGHeroInstance * hero, int creatureId) const;
	int mergeDuplicateArmyStacks(const CGHeroInstance * hero);
	int recruitAfterTownUpgrades(const CGHeroInstance * hero);
	int recruitFromCurrentTown(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config);
	std::optional<int3> findExploreTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findCaptureTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findFightTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	bool pathIsSafeForMvp(const CGHeroInstance * hero, const CGPath & path, const int3 & destination, bool allowDestinationBattle, const std::optional<int3> & allowedBattleGuard = std::nullopt) const;
	bool withinConfiguredRadius(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config, const int3 & destination) const;

public:
	explicit AutoHeroController(CPlayerInterface & owner_);

	bool start(bool endTurnWhenFinished = false);
	void cancel();
	void update();
	void onHeroMovementFinished(const CGHeroInstance * hero);
	void onDialogResolved();
	void onQueryOpened();
	void onQueryReplyApplied();
	bool shouldAutoAcceptDwellingRecruit() const;
	std::optional<int> autoCreatureEncounterReply() const;
	bool shouldAutoAcceptBorderGuard() const;
	bool handleTransportHeroExchange(ObjectInstanceID hero1, ObjectInstanceID hero2, QueryID query);
	bool isRunning() const { return running; }
	AutoHeroes::DecisionPolicy decisionPolicy() const;
	AutoHeroes::TreasureChestChoice treasureChestChoice() const;
	bool isTreasureChestInteraction() const;
	bool allowsSecondarySkillLearning() const;
	bool allowsAction(AutoHeroes::Action action) const;
	const CGHeroInstance * currentHero() const;
	int recruitmentBudgetPercent() const;
	AutoHeroes::RecruitmentScope recruitmentScope() const;
	AutoHeroes::RecruitmentLocation recruitmentLocation() const;
	int maxForeignFactionSlots() const;
	bool shouldAutoFight(const CGHeroInstance * hero) const;
	bool isAutoBattleActive() const;
	bool isWaitingForBattle() const { return waitingForBattle; }
	void onBattleStarted();
	void onBattleFinished();
	void onNewTurn();
};
