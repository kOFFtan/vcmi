/*
 * CAutoHeroWindow.h, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#pragma once

#include "CWindowObject.h"
#include "../../lib/autoheroes/AutoHeroConfig.h"

class CGHeroInstance;
class CButton;
class CLabel;
class CToggleButton;

class CAutoHeroWindow : public CWindowObject
{
	const CGHeroInstance * hero;
	AutoHeroes::HeroConfig draft;

	std::vector<std::shared_ptr<CIntObject>> controls;
	std::vector<std::shared_ptr<CLabel>> priorityLabels;
	std::shared_ptr<CButton> recruitmentButton;
	std::shared_ptr<CButton> foreignSlotsButton;
	std::shared_ptr<CButton> recruitmentBudgetButton;
	std::shared_ptr<CButton> combatButton;
	std::shared_ptr<CButton> decisionButton;
	std::shared_ptr<CButton> skillLearningButton;

	void updatePriorityLabels();
	void movePriority(AutoHeroes::Action action, int delta);
	void updateRecruitmentButton();
	void updateForeignSlotsButton();
	void updateRecruitmentBudgetButton();
	void updateCombatButton();
	void updateDecisionButton();
	void updateSkillLearningButton();
	void persistSettings();
	void saveAndClose();
	void saveAndRun();

	std::string actionLabel(AutoHeroes::Action action) const;

public:
	explicit CAutoHeroWindow(const CGHeroInstance * hero);
};
