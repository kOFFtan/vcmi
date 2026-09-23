/*
 * AutoHeroConfig.cpp, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#include "StdInc.h"

#include "AutoHeroConfig.h"

#include "../CConfigHandler.h"
#include "../json/JsonNode.h"

namespace AutoHeroes
{
namespace
{
const char * SESSION_KEY = "autoHeroes";
const char * HEROES_KEY = "heroes";

const JsonNode * findHeroNode(ObjectInstanceID heroId)
{
	const JsonNode & root = ::settings["session"][SESSION_KEY];
	if(!root.isStruct())
		return nullptr;

	const JsonNode & heroes = root[HEROES_KEY];
	if(!heroes.isStruct())
		return nullptr;

	const std::string heroKey = std::to_string(heroId.getNum());
	auto it = heroes.Struct().find(heroKey);
	if(it == heroes.Struct().end())
		return nullptr;

	return &it->second;
}

bool readBool(const JsonNode & node, const std::string & key, bool fallback)
{
	const JsonNode & value = node[key];
	return value.isBool() ? value.Bool() : fallback;
}

int readInt(const JsonNode & node, const std::string & key, int fallback)
{
	const JsonNode & value = node[key];
	return value.isNumber() ? static_cast<int>(value.Integer()) : fallback;
}

std::string readString(const JsonNode & node, const std::string & key, const std::string & fallback)
{
	const JsonNode & value = node[key];
	return value.isString() ? value.String() : fallback;
}

std::vector<Action> defaultPriority()
{
	return {
		Action::LEVEL_UP,
		Action::RECRUIT_CREATURES,
		Action::COLLECT_RESOURCES,
		Action::EXPLORE,
		Action::CAPTURE_OBJECTS,
		Action::FIGHT_NEUTRALS
	};
}

std::set<Action> defaultActions()
{
	return {
		Action::COLLECT_RESOURCES,
		Action::LEVEL_UP,
		Action::RECRUIT_CREATURES,
		Action::EXPLORE
	};
}
}

bool HeroConfig::allows(Action action) const
{
	return actions.contains(action);
}

int HeroConfig::priorityOf(Action action) const
{
	auto it = std::find(priority.begin(), priority.end(), action);
	if(it == priority.end())
		return static_cast<int>(priority.size());
	return static_cast<int>(std::distance(priority.begin(), it));
}

std::string actionToString(Action action)
{
	switch(action)
	{
	case Action::COLLECT_RESOURCES: return "collectResources";
	case Action::LEVEL_UP: return "levelUp";
	case Action::RECRUIT_CREATURES: return "recruitCreatures";
	case Action::EXPLORE: return "explore";
	case Action::CAPTURE_OBJECTS: return "captureObjects";
	case Action::FIGHT_NEUTRALS: return "fightNeutrals";
	}
	return "collectResources";
}

Action actionFromString(const std::string & value)
{
	if(value == "levelUp") return Action::LEVEL_UP;
	if(value == "recruitCreatures") return Action::RECRUIT_CREATURES;
	if(value == "explore") return Action::EXPLORE;
	if(value == "captureObjects") return Action::CAPTURE_OBJECTS;
	if(value == "fightNeutrals") return Action::FIGHT_NEUTRALS;
	return Action::COLLECT_RESOURCES;
}

JsonNode serializeHeroConfig(const HeroConfig & config)
{
	JsonNode node;
	node["enabled"].Bool() = config.enabled;
	node["goldReserve"].Integer() = std::max(0, config.goldReserve);
	node["movementRadius"].Integer() = std::max(0, config.movementRadius);
	node["maxForeignFactionSlots"].Integer() = std::clamp(config.maxForeignFactionSlots, -1, 7);
	node["recruitmentScope"].String() = config.recruitmentScope == RecruitmentScope::UNRESTRICTED ? "unrestricted" : "heroFactionOnly";

	switch(config.combatPolicy)
	{
	case CombatPolicy::DISABLED: node["combatPolicy"].String() = "disabled"; break;
	case CombatPolicy::ALLOW_SMALL_LOSSES: node["combatPolicy"].String() = "allowSmallLosses"; break;
	case CombatPolicy::SAFE_ONLY: node["combatPolicy"].String() = "safeOnly"; break;
	}

	for(Action action : defaultPriority())
		node["actions"][actionToString(action)].Bool() = config.actions.contains(action);

	for(Action action : config.priority)
		node["priority"].Vector().emplace_back(actionToString(action));

	return node;
}

HeroConfig deserializeHeroConfig(const JsonNode & node)
{
	HeroConfig result;
	result.actions = defaultActions();
	result.priority = defaultPriority();

	if(!node.isStruct())
		return result;

	result.enabled = readBool(node, "enabled", false);
	result.goldReserve = std::max(0, readInt(node, "goldReserve", 10000));
	result.movementRadius = std::max(0, readInt(node, "movementRadius", 0));
	result.maxForeignFactionSlots = std::clamp(readInt(node, "maxForeignFactionSlots", 0), -1, 7);

	const std::string recruitment = readString(node, "recruitmentScope", "heroFactionOnly");
	result.recruitmentScope = recruitment == "unrestricted"
		? RecruitmentScope::UNRESTRICTED
		: RecruitmentScope::HERO_FACTION_ONLY;

	const std::string combat = readString(node, "combatPolicy", "safeOnly");
	if(combat == "disabled")
		result.combatPolicy = CombatPolicy::DISABLED;
	else if(combat == "allowSmallLosses")
		result.combatPolicy = CombatPolicy::ALLOW_SMALL_LOSSES;
	else
		result.combatPolicy = CombatPolicy::SAFE_ONLY;

	const JsonNode & actionNode = node["actions"];
	if(actionNode.isStruct())
	{
		result.actions.clear();
		for(Action action : defaultPriority())
		{
			const JsonNode & value = actionNode[actionToString(action)];
			if(value.isBool() && value.Bool())
				result.actions.insert(action);
		}
	}

	const JsonNode & priorityNode = node["priority"];
	if(priorityNode.isVector())
	{
		result.priority.clear();
		for(const JsonNode & entry : priorityNode.Vector())
		{
			if(!entry.isString())
				continue;
			Action action = actionFromString(entry.String());
			if(std::find(result.priority.begin(), result.priority.end(), action) == result.priority.end())
				result.priority.push_back(action);
		}

		for(Action action : defaultPriority())
			if(std::find(result.priority.begin(), result.priority.end(), action) == result.priority.end())
				result.priority.push_back(action);
	}

	return result;
}

HeroConfig readHeroConfig(ObjectInstanceID heroId)
{
	const JsonNode * node = findHeroNode(heroId);
	if(!node)
		return deserializeHeroConfig(JsonNode());

	return deserializeHeroConfig(*node);
}

bool hasHeroConfig(ObjectInstanceID heroId)
{
	return findHeroNode(heroId) != nullptr;
}

void writeHeroConfig(ObjectInstanceID heroId, const HeroConfig & config)
{
	Settings node = ::settings.write["session"][SESSION_KEY][HEROES_KEY][std::to_string(heroId.getNum())];
	node["enabled"].Bool() = config.enabled;
	node["goldReserve"].Integer() = std::max(0, config.goldReserve);
	node["movementRadius"].Integer() = std::max(0, config.movementRadius);
	node["maxForeignFactionSlots"].Integer() = std::clamp(config.maxForeignFactionSlots, -1, 7);
	node["recruitmentScope"].String() = config.recruitmentScope == RecruitmentScope::UNRESTRICTED ? "unrestricted" : "heroFactionOnly";

	switch(config.combatPolicy)
	{
	case CombatPolicy::DISABLED: node["combatPolicy"].String() = "disabled"; break;
	case CombatPolicy::ALLOW_SMALL_LOSSES: node["combatPolicy"].String() = "allowSmallLosses"; break;
	case CombatPolicy::SAFE_ONLY: node["combatPolicy"].String() = "safeOnly"; break;
	}

	JsonNode & actions = node["actions"];
	for(Action action : defaultPriority())
		actions[actionToString(action)].Bool() = config.actions.contains(action);

	JsonNode & priority = node["priority"];
	priority.Vector().clear();
	for(Action action : config.priority)
		priority.Vector().emplace_back(actionToString(action));
}

void eraseHeroConfig(ObjectInstanceID heroId)
{
	Settings heroes = ::settings.write["session"][SESSION_KEY][HEROES_KEY];
	if(heroes->isStruct())
		heroes->Struct().erase(std::to_string(heroId.getNum()));
}

void clearAllHeroConfigs()
{
	Settings root = ::settings.write["session"][SESSION_KEY];
	root[HEROES_KEY].clear();
}

std::vector<ObjectInstanceID> enabledHeroIds()
{
	std::vector<ObjectInstanceID> result;
	const JsonNode & root = ::settings["session"][SESSION_KEY];
	if(!root.isStruct())
		return result;

	const JsonNode & heroes = root[HEROES_KEY];
	if(!heroes.isStruct())
		return result;

	for(const auto & [key, node] : heroes.Struct())
	{
		if(!node.isStruct() || !readBool(node, "enabled", false))
			continue;

		try
		{
			result.emplace_back(std::stoi(key));
		}
		catch(const std::exception &)
		{
			// Ignore stale/malformed runtime entries.
		}
	}
	return result;
}

bool hasAnyEnabledHero()
{
	return !enabledHeroIds().empty();
}

bool isHeroEnabled(ObjectInstanceID heroId)
{
	return readHeroConfig(heroId).enabled;
}

bool isPhaseActive()
{
	const JsonNode & value = ::settings["session"][SESSION_KEY]["phaseActive"];
	return value.isBool() && value.Bool();
}

void setPhaseActive(bool active)
{
	Settings phase = ::settings.write["session"][SESSION_KEY];
	phase["phaseActive"].Bool() = active;
}

PlayerColor phasePlayer()
{
	const JsonNode & value = ::settings["session"][SESSION_KEY]["phasePlayer"];
	return value.isNumber() ? PlayerColor(static_cast<int>(value.Integer())) : PlayerColor::CANNOT_DETERMINE;
}

void setPhasePlayer(PlayerColor player)
{
	Settings phase = ::settings.write["session"][SESSION_KEY];
	phase["phasePlayer"].Integer() = player.getNum();
}

void clearRuntimePhase()
{
	setPhaseActive(false);
	setPhasePlayer(PlayerColor::CANNOT_DETERMINE);
}

}
