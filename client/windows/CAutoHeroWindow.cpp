/*
 * CAutoHeroWindow.cpp, part of VCMI AutoHeroes extension
 *
 * License: GNU General Public License v2.0 or later
 */
#include "StdInc.h"
#include "CAutoHeroWindow.h"

#include "../GameInstance.h"
#include "../GameEngine.h"
#include "../adventureMap/AdventureMapInterface.h"
#include "../CPlayerInterface.h"
#include "../PlayerLocalState.h"
#include "../widgets/Buttons.h"
#include "../widgets/GraphicalPrimitiveCanvas.h"
#include "../widgets/Images.h"
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
	: CWindowObject(PLAYER_COLORED | BORDERED)
	, hero(hero_)
	, draft(AutoHeroes::readHeroConfig(hero_->id))
{
	OBJECT_CONSTRUCTION;

	constexpr int WIN_W = 760;
	constexpr int WIN_H = 610;

	pos = Rect(pos.x, pos.y, WIN_W, WIN_H);
	updateShadow();
	center();

	auto tr = [](const std::string & key)
	{
		return LIBRARY->generaltexth->translate(key);
	};

	auto keep = [this](auto widget)
	{
		controls.push_back(widget);
		return widget;
	};

	// Neutral tiled background instead of questDialog: that image contains baked-in
	// black quest text boxes which overlapped the AutoHeroes controls on Android.
	keep(std::make_shared<CFilledTexture>(ImagePath::builtin("DIBOXBCK"), Rect(0, 0, WIN_W, WIN_H)));
	keep(std::make_shared<TransparentFilledRectangle>(Rect(20, 94, 720, 246), ColorRGBA(0, 0, 0, 54), ColorRGBA(120, 92, 48, 220), 1));
	keep(std::make_shared<TransparentFilledRectangle>(Rect(20, 350, 720, 172), ColorRGBA(0, 0, 0, 54), ColorRGBA(120, 92, 48, 220), 1));

	std::string title = tr("vcmi.autoHeroes.title") + " v1.1: " + GAME->translator().translate(hero->getNameTextID());
	keep(std::make_shared<CLabel>(WIN_W / 2, 24, FONT_MEDIUM, ETextAlignment::CENTER, Colors::YELLOW, title, 700));

	auto enabled = keep(std::make_shared<CToggleButton>(
		Point(30, 50),
		AnimationPath::builtin("sysopchk.def"),
		CButton::tooltip(tr("vcmi.autoHeroes.enabled")),
		[this](bool selected){ draft.enabled = selected; }));
	enabled->setSelectedSilent(draft.enabled);
	keep(std::make_shared<CLabel>(68, 57, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE, tr("vcmi.autoHeroes.enabled")));
	keep(std::make_shared<CLabel>(735, 57, FONT_SMALL, ETextAlignment::TOPRIGHT, Colors::YELLOW, tr("vcmi.autoHeroes.runHint"), 520));

	keep(std::make_shared<CLabel>(68, 108, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.actions")));
	keep(std::make_shared<CLabel>(515, 108, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, tr("vcmi.autoHeroes.priority")));
	keep(std::make_shared<CLabel>(575, 108, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, tr("vcmi.autoHeroes.higher"), 70));
	keep(std::make_shared<CLabel>(665, 108, FONT_SMALL, ETextAlignment::CENTER, Colors::YELLOW, tr("vcmi.autoHeroes.lower"), 70));

	priorityLabels.reserve(ACTIONS.size());
	for(size_t i = 0; i < ACTIONS.size(); ++i)
	{
		const auto action = ACTIONS[i];
		const int y = 138 + static_cast<int>(i) * 32;

		auto toggle = keep(std::make_shared<CToggleButton>(
			Point(30, y - 7),
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

		keep(std::make_shared<CLabel>(68, y, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::WHITE, actionLabel(action), 370));

		auto rank = std::make_shared<CLabel>(515, y, FONT_SMALL, ETextAlignment::CENTER, Colors::WHITE, "", 36);
		priorityLabels.push_back(rank);

		auto up = keep(std::make_shared<CButton>(Point(540, y - 11), AnimationPath::builtin("settingsWindow/button80"), CButton::tooltip(tr("vcmi.autoHeroes.moveUp")), [this, action](){ movePriority(action, -1); }));
		up->setTextOverlay(tr("vcmi.autoHeroes.higher"), FONT_SMALL, Colors::YELLOW);

		auto down = keep(std::make_shared<CButton>(Point(630, y - 11), AnimationPath::builtin("settingsWindow/button80"), CButton::tooltip(tr("vcmi.autoHeroes.moveDown")), [this, action](){ movePriority(action, +1); }));
		down->setTextOverlay(tr("vcmi.autoHeroes.lower"), FONT_SMALL, Colors::YELLOW);
	}

	updatePriorityLabels();

	keep(std::make_shared<CLabel>(30, 362, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.recruitment")));
	recruitmentButton = keep(std::make_shared<CButton>(Point(30, 382), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.recruitmentHelp")), [this]()
	{
		draft.recruitmentScope = draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY
			? AutoHeroes::RecruitmentScope::UNRESTRICTED
			: AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY;
		if(draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY)
			draft.maxForeignFactionSlots = 0;
		else if(draft.maxForeignFactionSlots == 0)
			draft.maxForeignFactionSlots = -1;
		updateRecruitmentButton();
		updateForeignSlotsButton();
	}));
	updateRecruitmentButton();

	keep(std::make_shared<CLabel>(230, 362, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.foreignSlots")));
	foreignSlotsButton = keep(std::make_shared<CButton>(Point(230, 382), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.foreignSlotsHelp")), [this]()
	{
		if(draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY)
			return;
		if(draft.maxForeignFactionSlots < 0 || draft.maxForeignFactionSlots >= 7)
			draft.maxForeignFactionSlots = 1;
		else
			++draft.maxForeignFactionSlots;
		updateForeignSlotsButton();
	}));
	updateForeignSlotsButton();

	keep(std::make_shared<CLabel>(430, 362, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.combat")));
	combatButton = keep(std::make_shared<CButton>(Point(430, 382), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.combatHelp")), [this]()
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
	}));
	updateCombatButton();

	keep(std::make_shared<CLabel>(30, 432, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.decisions")));
	decisionButton = keep(std::make_shared<CButton>(Point(30, 452), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.decisionsHelp")), [this]()
	{
		draft.decisionPolicy = draft.decisionPolicy == AutoHeroes::DecisionPolicy::ASK_HUMAN
			? AutoHeroes::DecisionPolicy::AUTO_ACCEPT
			: AutoHeroes::DecisionPolicy::ASK_HUMAN;
		updateDecisionButton();
	}));
	updateDecisionButton();

	keep(std::make_shared<CLabel>(300, 432, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.skillLearning")));
	skillLearningButton = keep(std::make_shared<CButton>(Point(300, 452), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.skillLearningHelp")), [this]()
	{
		draft.allowSecondarySkillLearning = !draft.allowSecondarySkillLearning;
		updateSkillLearningButton();
	}));
	updateSkillLearningButton();

	keep(std::make_shared<CLabel>(30, 500, FONT_SMALL, ETextAlignment::TOPLEFT, Colors::YELLOW, tr("vcmi.autoHeroes.infoAuto"), 700));

	auto run = keep(std::make_shared<CButton>(Point(30, 552), AnimationPath::builtin("settingsWindow/button190"), CButton::tooltip(tr("vcmi.autoHeroes.runWarning")), [this](){ saveAndRun(); }));
	run->setTextOverlay(tr("vcmi.autoHeroes.runNow"), FONT_SMALL, Colors::YELLOW);

	auto cancel = keep(std::make_shared<CButton>(Point(500, 552), AnimationPath::builtin("settingsWindow/button80"), CButton::tooltip(tr("vcmi.autoHeroes.cancel")), [this](){ close(); }, EShortcut::GLOBAL_CANCEL));
	cancel->setTextOverlay(tr("vcmi.autoHeroes.cancelShort"), FONT_SMALL, Colors::YELLOW);

	auto save = keep(std::make_shared<CButton>(Point(590, 552), AnimationPath::builtin("settingsWindow/button80"), CButton::tooltip(tr("vcmi.autoHeroes.save")), [this](){ saveAndClose(); }, EShortcut::GLOBAL_ACCEPT));
	save->setTextOverlay(tr("vcmi.autoHeroes.saveShort"), FONT_SMALL, Colors::YELLOW);
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

void CAutoHeroWindow::updateForeignSlotsButton()
{
	if(draft.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY)
	{
		foreignSlotsButton->setTextOverlay(LIBRARY->generaltexth->translate("vcmi.autoHeroes.foreignSlotsNone"), FONT_SMALL, Colors::YELLOW);
		foreignSlotsButton->block(true);
		return;
	}

	foreignSlotsButton->block(false);
	if(draft.maxForeignFactionSlots < 0)
	{
		foreignSlotsButton->setTextOverlay(LIBRARY->generaltexth->translate("vcmi.autoHeroes.foreignSlotsUnlimited"), FONT_SMALL, Colors::YELLOW);
		return;
	}

	foreignSlotsButton->setTextOverlay(std::to_string(std::clamp(draft.maxForeignFactionSlots, 1, 7)), FONT_SMALL, Colors::YELLOW);
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

void CAutoHeroWindow::updateDecisionButton()
{
	const std::string key = draft.decisionPolicy == AutoHeroes::DecisionPolicy::AUTO_ACCEPT
		? "vcmi.autoHeroes.decisionsAuto"
		: "vcmi.autoHeroes.decisionsHuman";
	decisionButton->setTextOverlay(LIBRARY->generaltexth->translate(key), FONT_SMALL, Colors::YELLOW);
}

void CAutoHeroWindow::updateSkillLearningButton()
{
	const std::string key = draft.allowSecondarySkillLearning
		? "vcmi.autoHeroes.skillLearningAllow"
		: "vcmi.autoHeroes.skillLearningBlock";
	skillLearningButton->setTextOverlay(LIBRARY->generaltexth->translate(key), FONT_SMALL, Colors::YELLOW);
}

void CAutoHeroWindow::persistSettings()
{
	AutoHeroes::writeHeroConfig(hero->id, draft);
	if(GAME->interface() && GAME->interface()->localState)
		GAME->interface()->localState->saveState();
}

void CAutoHeroWindow::saveAndClose()
{
	persistSettings();
	close();
}

void CAutoHeroWindow::saveAndRun()
{
    persistSettings();

    if(!draft.enabled || draft.actions.empty())
    {
        logGlobal->warn("AutoHeroes v1.0: run-now ignored because automation is disabled or has no actions");
        close();
        return;
    }

    close();

	// Stage 6 keeps CPlayerInterface alive. Run configured heroes locally and
	// return control to the player when they finish; never end the turn here.
	ENGINE->dispatchMainThread([]()
	{
		if(GAME->interface())
			GAME->interface()->runAutoHeroesNow();
	});
}
