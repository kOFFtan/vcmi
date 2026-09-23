/*
 * CAutoHeroWindow.cpp, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#include "StdInc.h"
#include "CAutoHeroWindow.h"

#include "../GameInstance.h"
#include "../CPlayerInterface.h"
#include "../PlayerLocalState.h"
#include "../widgets/Buttons.h"
#include "../widgets/TextControls.h"
#include "../gui/Shortcut.h"
#include "render/Colors.h"

#include "../../lib/GameLibrary.h"
#include "../../lib/mapObjects/CGHeroInstance.h"
#include "../../lib/texts/CGeneralTextHandler.h"

namespace
{
const std::array<AutoHeroes::Action, 6> ACTIONS = {
	AutoHeroes::Action::COLLECT_RESOURCES,
	AutoHeroes::Action::LEVEL_UP,
	AutoHeroes::Action::RECRUIT_CREATURES,
	AutoHeroes::Action::EXPLORE,
	AutoHeroes::Action::CAPTURE_OBJECTS,
	AutoHeroes::Action::FIGHT_NEUTRALS
};
}

CAutoHeroWindow::CAutoHeroWindow(const CGHeroInstance * hero_)
	: CWindowObject(PLAYER_COLORED | BORDERED, ImagePath::builtin("questDialog"))
	, hero(hero_)
	, draft(AutoHeroes::readHeroConfig(hero_->id))
{
	OBJECT_CONSTRUCTION;

	auto tr = [](const std::string & key)
	{
		return LIBRARY->generaltexth->translate(key);
	};

	auto keep = [this](auto widget)
	{
		controls.push_back(widget);
		return widget;
	};

	std::string title = tr("vcmi.autoHeroes.title") + ": " + GAME->translator().translate(hero->getNameTextID());
	keep(std::make_shared<CLabel>(300, 24, FONT_MEDIUM, ETextAlignment::CENTER, Colors::YELLOW, title, 520));

	auto enabled = keep(std::make_shared<CToggleButton>(
		Point(40, 48),
		AnimationPath::builtin("sysopchk.def"),
		CButton::tooltip(tr("vcmi.autoHeroes.enabled")),
		[this](bool selected){ draft.enabled = selected; }));
	enabled->setSelectedSilent(draft.enabled);
	keep(std::make_shared<CLabel>(78, 55, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE, tr("vcmi.autoHeroes.enabled")));

	keep(std::make_shared<CLabel>(78, 86, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.actions")));
	keep(std::make_shared<CLabel>(332, 86, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, tr("vcmi.autoHeroes.priority")));

	priorityLabels.reserve(ACTIONS.size());
	for(size_t i = 0; i < ACTIONS.size(); ++i)
	{
		const auto action = ACTIONS[i];
		const int y = 106 + static_cast<int>(i) * 34;

		auto toggle = keep(std::make_shared<CToggleButton>(
			Point(40, y - 7),
			AnimationPath::builtin("sysopchk.def"),
			CButton::tooltip(actionLabel(action)),
			[this, action](bool selected)
			{
				if(selected)
					draft.actions.insert(action);
				else
					draft.actions.erase(action);
			}));
		toggle->setSelectedSilent(draft.actions.contains(action));

		keep(std::make_shared<CLabel>(78, y, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE, actionLabel(action), 220));

		auto rank = std::make_shared<CLabel>(332, y, FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE, "", 40);
		priorityLabels.push_back(rank);

		auto up = keep(std::make_shared<CButton>(Point(374, y - 10), AnimationPath::builtin("settingsWindow/button32"), CButton::tooltip(tr("vcmi.autoHeroes.moveUp")), [this, action](){ movePriority(action, -1); }));
		up->setTextOverlay("↑", FONT_MEDIUM, Colors::YELLOW);

		auto down = keep(std::make_shared<CButton>(Point(410, y - 10), AnimationPath::builtin("settingsWindow/button32"), CButton::tooltip(tr("vcmi.autoHeroes.moveDown")), [this, action](){ movePriority(action, +1); }));
		down->setTextOverlay("↓", FONT_MEDIUM, Colors::YELLOW);
	}

	updatePriorityLabels();

	keep(std::make_shared<CLabel>(40, 315, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.recruitment")));
	recruitmentButton = std::make_shared<CButton>(Point(40, 334), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.recruitmentHelp")), [this]()
	{
		draft.recruitmentScope = draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY
			? AutoHeroes::RecruitmentScope::UNRESTRICTED
			: AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY;
		if(draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY)
			draft.maxForeignFactionSlots = 0;
		else if(draft.maxForeignFactionSlots == 0)
			draft.maxForeignFactionSlots = -1;
		updateRecruitmentButton();
	});
	updateRecruitmentButton();

	keep(std::make_shared<CLabel>(260, 315, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.combat")));
	combatButton = std::make_shared<CButton>(Point(260, 334), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.combatHelp")), [this]()
	{
		switch(draft.combatPolicy)
		{
		case AutoHeroes::CombatPolicy::DISABLED:
			draft.combatPolicy = AutoHeroes::CombatPolicy::SAFE_ONLY;
			break;
		case AutoHeroes::CombatPolicy::SAFE_ONLY:
			draft.combatPolicy = AutoHeroes::CombatPolicy::ALLOW_SMALL_LOSSES;
			break;
		case AutoHeroes::CombatPolicy::ALLOW_SMALL_LOSSES:
			draft.combatPolicy = AutoHeroes::CombatPolicy::DISABLED;
			break;
		}
		updateCombatButton();
	});
	updateCombatButton();

	auto ok = keep(std::make_shared<CButton>(Point(490, 385), AnimationPath::builtin("IOKAY.DEF"), CButton::tooltip(tr("vcmi.autoHeroes.save")), [this](){ saveAndClose(); }, EShortcut::GLOBAL_ACCEPT));
	auto cancel = keep(std::make_shared<CButton>(Point(418, 385), AnimationPath::builtin("ICANCEL.DEF"), CButton::tooltip(tr("vcmi.autoHeroes.cancel")), [this](){ close(); }, EShortcut::GLOBAL_CANCEL));
	(void)ok;
	(void)cancel;
}

std::string CAutoHeroWindow::actionLabel(AutoHeroes::Action action) const
{
	std::string key;
	switch(action)
	{
	case AutoHeroes::Action::COLLECT_RESOURCES: key = "vcmi.autoHeroes.action.collect"; break;
	case AutoHeroes::Action::LEVEL_UP: key = "vcmi.autoHeroes.action.level"; break;
	case AutoHeroes::Action::RECRUIT_CREATURES: key = "vcmi.autoHeroes.action.recruit"; break;
	case AutoHeroes::Action::EXPLORE: key = "vcmi.autoHeroes.action.explore"; break;
	case AutoHeroes::Action::CAPTURE_OBJECTS: key = "vcmi.autoHeroes.action.capture"; break;
	case AutoHeroes::Action::FIGHT_NEUTRALS: key = "vcmi.autoHeroes.action.fight"; break;
	}
	return LIBRARY->generaltexth->translate(key);
}

void CAutoHeroWindow::updatePriorityLabels()
{
	for(size_t i = 0; i < ACTIONS.size(); ++i)
	{
		const int rank = draft.priorityOf(ACTIONS[i]) + 1;
		priorityLabels[i]->setText(std::to_string(rank));
	}
}

void CAutoHeroWindow::movePriority(AutoHeroes::Action action, int delta)
{
	auto it = std::find(draft.priority.begin(), draft.priority.end(), action);
	if(it == draft.priority.end())
		return;

	const int current = static_cast<int>(std::distance(draft.priority.begin(), it));
	const int target = std::clamp(current + delta, 0, static_cast<int>(draft.priority.size()) - 1);
	if(current == target)
		return;

	std::iter_swap(draft.priority.begin() + current, draft.priority.begin() + target);
	updatePriorityLabels();
}

void CAutoHeroWindow::updateRecruitmentButton()
{
	const std::string key = draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY
		? "vcmi.autoHeroes.recruitOwn"
		: "vcmi.autoHeroes.recruitAny";
	recruitmentButton->setTextOverlay(LIBRARY->generaltexth->translate(key), FONT_SMALL, Colors::YELLOW);
}

void CAutoHeroWindow::updateCombatButton()
{
	std::string key;
	switch(draft.combatPolicy)
	{
	case AutoHeroes::CombatPolicy::DISABLED: key = "vcmi.autoHeroes.combatOff"; break;
	case AutoHeroes::CombatPolicy::SAFE_ONLY: key = "vcmi.autoHeroes.combatSafe"; break;
	case AutoHeroes::CombatPolicy::ALLOW_SMALL_LOSSES: key = "vcmi.autoHeroes.combatLosses"; break;
	}
	combatButton->setTextOverlay(LIBRARY->generaltexth->translate(key), FONT_SMALL, Colors::YELLOW);
}

void CAutoHeroWindow::saveAndClose()
{
	AutoHeroes::writeHeroConfig(hero->id, draft);
	if(GAME->interface() && GAME->interface()->localState)
		GAME->interface()->localState->saveState();
	close();
}
