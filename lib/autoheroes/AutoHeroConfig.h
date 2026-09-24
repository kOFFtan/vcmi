/*
 * AutoHeroConfig.h, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#pragma once

#include "../constants/EntityIdentifiers.h"

#include <set>
#include <string>
#include <vector>

class JsonNode;

namespace AutoHeroes
{

enum class Action
{
	COLLECT_RESOURCES,
	LEVEL_UP,
	RECRUIT_CREATURES,
	EXPLORE,
	CAPTURE_OBJECTS,
	FIGHT_NEUTRALS
};

enum class RecruitmentScope
{
	HERO_FACTION_ONLY,
	UNRESTRICTED
};

enum class RecruitmentBudget
{
	PERCENT_25 = 25,
	PERCENT_50 = 50,
	PERCENT_75 = 75,
	PERCENT_100 = 100
};

enum class CombatPolicy
{
	DISABLED,
	SAFE_ONLY,
	ALLOW_SMALL_LOSSES
};

enum class DecisionPolicy
{
	ASK_HUMAN,
	AUTO_ACCEPT
};

enum class TreasureChestChoice
{
	GOLD,
	EXPERIENCE
};

struct HeroConfig
{
	bool enabled = false;
	std::set<Action> actions;
	std::vector<Action> priority;
	RecruitmentScope recruitmentScope = RecruitmentScope::HERO_FACTION_ONLY;
	RecruitmentBudget recruitmentBudget = RecruitmentBudget::PERCENT_100;
	/// -1 means unlimited. Otherwise valid range is 0..7.
	int maxForeignFactionSlots = 0;
	CombatPolicy combatPolicy = CombatPolicy::SAFE_ONLY;
	DecisionPolicy decisionPolicy = DecisionPolicy::ASK_HUMAN;
	TreasureChestChoice treasureChestChoice = TreasureChestChoice::GOLD;
	/// If false, AutoHeroes must not intentionally visit map objects that teach secondary skills.
	bool allowSecondarySkillLearning = false;
	/// Legacy reserve kept for non-recruitment paid map objects. Recruitment uses recruitmentBudget.
	int goldReserve = 10000;
	/// 0 means unlimited.
	int movementRadius = 0;

	bool allows(Action action) const;
	int priorityOf(Action action) const;
};

/// Runtime/session data. Game-save persistence is handled through PlayerLocalState.
HeroConfig readHeroConfig(ObjectInstanceID heroId);
bool hasHeroConfig(ObjectInstanceID heroId);
void writeHeroConfig(ObjectInstanceID heroId, const HeroConfig & config);
void eraseHeroConfig(ObjectInstanceID heroId);
void clearAllHeroConfigs();

/// Stable JSON representation used by PlayerLocalState save/load.
JsonNode serializeHeroConfig(const HeroConfig & config);
HeroConfig deserializeHeroConfig(const JsonNode & node);

bool hasAnyEnabledHero();
bool isHeroEnabled(ObjectInstanceID heroId);
std::vector<ObjectInstanceID> enabledHeroIds();

bool isPhaseActive();
void setPhaseActive(bool active);
PlayerColor phasePlayer();
void setPhasePlayer(PlayerColor player);
void clearRuntimePhase();

std::string actionToString(Action action);
Action actionFromString(const std::string & value);
int recruitmentBudgetPercent(RecruitmentBudget budget);

}
