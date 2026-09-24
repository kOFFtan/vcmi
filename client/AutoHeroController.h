/*
 * AutoHeroController.h, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#pragma once

#include "../lib/constants/EntityIdentifiers.h"
#include "../lib/int3.h"

#include <cstddef>
#include <optional>
#include <vector>

class CPlayerInterface;
class CGHeroInstance;
struct CGPath;

namespace AutoHeroes
{

enum class Action;
enum class DecisionPolicy;
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
	bool endTurnAfterRun = false;
	std::vector<ObjectInstanceID> heroQueue;
	size_t heroQueueIndex = 0;
	std::optional<ObjectInstanceID> activeHeroId;
	int3 movementStartPosition = int3(-1, -1, -1);
	int movementStartPoints = -1;
	int stepsForCurrentHero = 0;

	const CGHeroInstance * activeHero() const;
	void process();
	void finishRun();
	void advanceHero();
	bool startNextAction(const CGHeroInstance * hero);
	bool startMovement(const CGHeroInstance * hero, const int3 & destination);
	std::optional<int3> findCollectTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	std::optional<int3> findExploreTarget(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config) const;
	bool pathIsSafeForMvp(const CGHeroInstance * hero, const CGPath & path, const int3 & destination) const;
	bool withinConfiguredRadius(const CGHeroInstance * hero, const AutoHeroes::HeroConfig & config, const int3 & destination) const;

public:
	explicit AutoHeroController(CPlayerInterface & owner_);

	bool start(bool endTurnWhenFinished = false);
	void cancel();
	void update();
	void onHeroMovementFinished(const CGHeroInstance * hero);
	void onDialogResolved();
	bool isRunning() const { return running; }
	AutoHeroes::DecisionPolicy decisionPolicy() const;
	bool allowsSecondarySkillLearning() const;
};
