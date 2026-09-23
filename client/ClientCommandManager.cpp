/*
 * ClientCommandManager.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"
#include "ClientCommandManager.h"

#include "Client.h"
#include "adventureMap/CInGameConsole.h"
#include "CPlayerInterface.h"
#include "PlayerLocalState.h"
#include "CServerHandler.h"
#include "GameEngine.h"
#include "GameInstance.h"
#include "battle/BattleFieldController.h"
#include "battle/BattleInterface.h"
#include "battle/BattleObstacleController.h"
#include "battle/CObstacleInstance.h"
#include "gui/WindowHandler.h"
#include "render/CanvasImage.h"
#include "render/IRenderHandler.h"
#include "ClientNetPackVisitors.h"
#include "../lib/callback/CCallback.h"
#include "../lib/callback/CGlobalAI.h"
#include "../lib/callback/AIFactory.h"
#include "../lib/CConfigHandler.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/CPlayerState.h"
#include "../lib/constants/StringConstants.h"
#include "../lib/callback/EditorCallback.h"
#include "../lib/campaign/CampaignHandler.h"
#include "../lib/mapping/CMapService.h"
#include "../lib/mapping/CMap.h"
#include "windows/CCastleInterface.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "render/CAnimation.h"
#include "../lib/texts/CGeneralTextHandler.h"
#include "../lib/filesystem/Filesystem.h"
#include "../lib/modding/CModHandler.h"
#include "../lib/modding/ContentTypeHandler.h"
#include "../lib/modding/ModUtility.h"
#include "../lib/serializer/GameConnection.h"
#include "../lib/VCMIDirs.h"
#include "../lib/texts/TextOperations.h"
#include "../lib/ObstacleHandler.h"
#include "../lib/logging/VisualLogger.h"
#include "../lib/autoheroes/AutoHeroConfig.h"

void ClientCommandManager::handleQuitCommand()
{
		throw GameShutdownException();
}

void ClientCommandManager::handleSaveCommand(std::istringstream & singleWordBuffer)
{
	if(!GAME->server().client)
	{
		printCommandMessage("Game is not in playing state");
		return;
	}

	std::string saveFilename;
	singleWordBuffer >> saveFilename;
	GAME->interface()->cb->save(saveFilename, false);
	printCommandMessage("Game saved as: " + saveFilename);
}

void ClientCommandManager::handleLoadCommand(std::istringstream& singleWordBuffer)
{
	// TODO: this code should end the running game and manage to call startGame instead
	//std::string fname;
	//singleWordBuffer >> fname;
	//GAME->server().client->loadGame(fname);
}

void ClientCommandManager::handleAutoskipCommand()
{
		Settings session = settings.write["session"];
		session["autoSkip"].Bool() = !session["autoSkip"].Bool();
}

void ClientCommandManager::handleControlaiCommand(std::istringstream& singleWordBuffer)
{
	std::string colorName;
	singleWordBuffer >> colorName;
	boost::to_lower(colorName);

	std::scoped_lock interfaceLock(ENGINE->interfaceMutex);

	if(!GAME->server().client)
	{
		printCommandMessage("Game is not in playing state");
		return;
	}

	PlayerColor color;
	if(GAME->interface())
		color = GAME->interface()->playerID;

	for(auto & elem : GAME->server().client->gameState().players)
	{
		if(!elem.first.isValidPlayer()
			|| elem.second.human
			|| (colorName.length() && elem.first.getNum() != vstd::find_pos(GameConstants::PLAYER_COLOR_NAMES, colorName)))
		{
			continue;
		}

		GAME->server().client->removeGUI();
		GAME->server().client->installNewPlayerInterface(std::make_shared<CPlayerInterface>(elem.first), elem.first);
	}

	ENGINE->windows().totalRedraw();
	if(color != PlayerColor::NEUTRAL)
		giveTurn(color);
}

void ClientCommandManager::handleSetBattleAICommand(std::istringstream& singleWordBuffer)
{
	std::string aiName;
	singleWordBuffer >> aiName;

	printCommandMessage("Will try loading that AI to see if it is correct name...\n");
	try
	{
		if(auto ai = AIFactory::createBattleAI(aiName)) //test that given AI is indeed available... heavy but it is easy to make a typo and break the game
		{
			Settings neutralAI = settings.write["ai"]["combatNeutralAI"];
			neutralAI->String() = aiName;
			printCommandMessage("Setting changed, from now the battle ai will be " + aiName + "!\n");
		}
	}
	catch(std::exception &e)
	{
		printCommandMessage("Failed opening " + aiName + ": " + e.what(), ELogLevel::WARN);
		printCommandMessage("Setting not changed, AI not found or invalid!", ELogLevel::WARN);
	}
}

void ClientCommandManager::handleRedrawCommand()
{
	ENGINE->windows().totalRedraw();
}

void ClientCommandManager::handleTranslateGameCommand(bool onlyMissing)
{
	std::map<std::string, ExportedStrings> textsByMod;
	LIBRARY->generaltexth->exportAllTexts(textsByMod, onlyMissing);

	const boost::filesystem::path outPath = VCMIDirs::get().userExtractedPath() / ( onlyMissing ? "translationMissing" : "translation");
	boost::filesystem::create_directories(outPath);

	for(const auto & modEntry : textsByMod)
	{
		JsonNode output;

		for(const auto & stringEntry : modEntry.second.strings)
		{
			if(boost::algorithm::starts_with(stringEntry.first, "map."))
				continue;
			if(boost::algorithm::starts_with(stringEntry.first, "campaign."))
				continue;

			output[stringEntry.first].String() = stringEntry.second;
		}

		if (!output.isNull())
		{
			std::string filename = modEntry.first;
			std::ranges::replace(filename, '.', '_');
			const boost::filesystem::path filePath = outPath / (filename + ".json");
			std::ofstream file(filePath.c_str());
			file << output.toString();
		}
	}

	printCommandMessage("Translation export complete");
	printCommandMessage("Extracted files can be found in " + TextOperations::filesystemPathToUtf8(outPath) + " directory\n");
}

void ClientCommandManager::handleTranslateMapsCommand()
{
	CMapService mapService;

	printCommandMessage("Searching for available maps");
	std::unordered_set<ResourcePath> mapList = CResourceHandler::get()->getFilteredFiles([&](const ResourcePath & ident)
	{
		return ident.getType() == EResType::MAP;
	});

	std::vector<std::unique_ptr<CMap>> loadedMaps;
	std::vector<std::shared_ptr<CampaignState>> loadedCampaigns;

	printCommandMessage("Loading maps for export");
	for (auto const & mapName : mapList)
	{
		try
		{
			EditorCallback cb(nullptr);
			// load and drop loaded map - we only need loader to run over all maps
			loadedMaps.push_back(mapService.loadMap(mapName, &cb));
		}
		catch(std::exception & e)
		{
			logGlobal->warn("Map %s is invalid. Message: %s", mapName.getName(), e.what());
		}
	}

	printCommandMessage("Searching for available campaigns");
	std::unordered_set<ResourcePath> campaignList = CResourceHandler::get()->getFilteredFiles([&](const ResourcePath & ident)
	{
		return ident.getType() == EResType::CAMPAIGN;
	});

	logGlobal->info("Loading campaigns for export");
	for (auto const & campaignName : campaignList)
	{
		try
		{
			loadedCampaigns.push_back(CampaignHandler::getCampaign(campaignName.getName()));
			for (auto const & part : loadedCampaigns.back()->allScenarios())
			{
				EditorCallback cb(nullptr);
				loadedCampaigns.back()->getMap(part, &cb);
			}
		}
		catch(std::exception & e)
		{
			logGlobal->warn("Campaign %s is invalid. Message: %s", campaignName.getName(), e.what());
		}
	}

	std::map<std::string, ExportedStrings> textsByMod;
	LIBRARY->generaltexth->exportAllTexts(textsByMod, false);

	const boost::filesystem::path outPath = VCMIDirs::get().userExtractedPath() / "translation";
	boost::filesystem::create_directories(outPath);

	for(const auto & modEntry : textsByMod)
	{
		JsonNode output;

		for(const auto & stringEntry : modEntry.second.strings)
		{
			if(boost::algorithm::starts_with(stringEntry.first, "map."))
				output[stringEntry.first].String() = stringEntry.second;

			if(boost::algorithm::starts_with(stringEntry.first, "campaign."))
				output[stringEntry.first].String() = stringEntry.second;
		}

		if (!output.isNull())
		{
			const boost::filesystem::path filePath = outPath / (modEntry.first + ".json");
			std::ofstream file(filePath.c_str());
			file << output.toString();
		}
	}

	printCommandMessage("Translation export complete");
	printCommandMessage("Extracted files can be found in " + TextOperations::filesystemPathToUtf8(outPath) + " directory\n");

}

void ClientCommandManager::handleGetConfigCommand()
{
	printCommandMessage("Command accepted.\t");

	const boost::filesystem::path outPath = VCMIDirs::get().userExtractedPath() / "configuration";

	boost::filesystem::create_directories(outPath);

	const std::vector<std::string> contentNames = { "heroClasses", "artifacts", "creatures", "factions", "objects", "heroes", "spells", "skills" };

	for(auto contentName : contentNames)
	{
		auto const & handler = *LIBRARY->modh->content;
		auto const & content = handler[contentName];

		auto contentOutPath = outPath / contentName;
		boost::filesystem::create_directories(contentOutPath);

		for(auto& iter : content.modData)
		{
			const JsonNode& modData = iter.second.modData;

			for(auto& nameAndObject : modData.Struct())
			{
				const JsonNode& object = nameAndObject.second;

				std::string name = ModUtility::makeFullIdentifier(object.getModScope(), contentName, nameAndObject.first);

				boost::algorithm::replace_all(name, ":", "_");

				const boost::filesystem::path filePath = contentOutPath / (name + ".json");
				std::ofstream file(filePath.c_str());
				file << object.toString();
			}
		}
	}

	printCommandMessage("\rExtracting done :)\n");
	printCommandMessage("Extracted files can be found in " + TextOperations::filesystemPathToUtf8(outPath) + " directory\n");
}

void ClientCommandManager::handleAntilagCommand(std::istringstream& singleWordBuffer)
{
	std::string commandName;
	singleWordBuffer >> commandName;

	if (commandName == "on")
	{
		GAME->server().enableLagCompensation(true);
		printCommandMessage("Network lag compensation is now enabled.\n");
	}
	else if (commandName == "off")
	{
		GAME->server().enableLagCompensation(true);
		printCommandMessage("Network lag compensation is now disabled.\n");
	}
	else
	{
		printCommandMessage("Unexpected syntax. Supported forms:\n");
		printCommandMessage("'antilag on'\n");
		printCommandMessage("'antilag off'\n");
	}
}

void ClientCommandManager::handleGetTextCommand()
{
	printCommandMessage("Command accepted.\t");

	const boost::filesystem::path outPath =
			VCMIDirs::get().userExtractedPath();

	auto list =
			CResourceHandler::get()->getFilteredFiles([](const ResourcePath & ident)
			{
				return ident.getType() == EResType::TEXT && boost::algorithm::starts_with(ident.getName(), "DATA/");
			});

	for (auto & filename : list)
	{
		const boost::filesystem::path filePath = outPath / (filename.getName() + ".TXT");

		boost::filesystem::create_directories(filePath.parent_path());

		std::ofstream file(filePath.c_str(), std::ios::binary);
		auto text = CResourceHandler::get()->load(filename)->readAll();

		file.write((char*)text.first.get(), text.second);
	}

	printCommandMessage("\rExtracting done :)\n");
	printCommandMessage("Extracted files can be found in " + TextOperations::filesystemPathToUtf8(outPath) + " directory\n");
}

void ClientCommandManager::handleDef2bmpCommand(std::istringstream& singleWordBuffer)
{
	std::string URI;
	singleWordBuffer >> URI;
	auto anim = ENGINE->renderHandler().loadAnimation(AnimationPath::builtin(URI), EImageBlitMode::SIMPLE);
	anim->exportBitmaps(VCMIDirs::get().userExtractedPath());
}

void ClientCommandManager::handleExtractCommand(std::istringstream& singleWordBuffer)
{
	std::string URI;
	singleWordBuffer >> URI;

	if(CResourceHandler::get()->existsResource(ResourcePath(URI)))
	{
		const boost::filesystem::path outPath = VCMIDirs::get().userExtractedPath() / URI;

		auto data = CResourceHandler::get()->load(ResourcePath(URI))->readAll();

		boost::filesystem::create_directories(outPath.parent_path());
		std::ofstream outFile(outPath.c_str(), std::ofstream::binary);
		outFile.write((char*)data.first.get(), data.second);
	}
	else
		printCommandMessage("File not found!", ELogLevel::ERROR);
}

void ClientCommandManager::handleObstaclesDebugCommand()
{
	auto cellBorder = ENGINE->renderHandler().loadImage(ImagePath::builtin("CCELLGRD.BMP"), EImageBlitMode::COLORKEY);
	auto cellShade = ENGINE->renderHandler().loadImage(ImagePath::builtin("CCELLSHD.BMP"), EImageBlitMode::SIMPLE);
	auto & battleInt = GAME->interface()->battleInt;
	if (!battleInt)
		return;

	auto & obstacleController = battleInt->obstacleController;

	for (const auto & obstacle : LIBRARY->obstacleHandler->objects)
	{
		if (obstacle->isAbsoluteObstacle)
		{
			continue; // TODO?
		}
		else
		{
			BattleHex position(0, obstacle->height - 1);
			BattleHex bottomRightHex(obstacle->width, obstacle->height);
			CObstacleInstance testObstacle;
			testObstacle.obstacleType = CObstacleInstance::USUAL;
			testObstacle.pos = position;
			testObstacle.ID = obstacle->getIndex();
			obstacleController->loadObstacleImage(testObstacle);

			Point bottomRightCorner = battleInt->fieldController->hexPositionLocal(bottomRightHex).bottomRight();
			CanvasImage canvas(bottomRightCorner, CanvasScalingPolicy::IGNORE);
			auto image = obstacleController->getObstacleImage(testObstacle);

			if (!image)
				continue;

			canvas.getCanvas().drawColor(Rect(Point(), canvas.dimensions()), ColorRGBA(0,255,255,255));
			canvas.getCanvas().draw(image, obstacleController->getObstaclePosition(image, testObstacle));
			canvas.getCanvas().drawBorder(Rect(obstacleController->getObstaclePosition(image, testObstacle), image->dimensions()), ColorRGBA(255, 0, 0, 255));

			for (int y = 0; y < obstacle->height; ++y)
				for (int x = 0; x < obstacle->width; ++x)
					canvas.getCanvas().draw(cellBorder, battleInt->fieldController->hexPositionLocal(BattleHex(x,y)).topLeft());

			for (const auto & blockedHex : obstacle->getBlocked(position))
				canvas.getCanvas().draw(cellShade, battleInt->fieldController->hexPositionLocal(blockedHex).topLeft());

			std::string modID = obstacle->getModScope();
			std::string obstacleID = obstacle->identifier;
			auto fullPath = VCMIDirs::get().userExtractedPath() / "obstacles" / modID;
			boost::filesystem::create_directories(fullPath);
			canvas.exportBitmap(fullPath / (obstacleID + ".png"));
		}
	}
}

void ClientCommandManager::handleBonusesCommand(std::istringstream & singleWordBuffer)
{
	if(currentCallFromIngameConsole)
	{
		printCommandMessage("Output for this command is too large for ingame chat! Please run it from client console.\n");
		return;
	}

	std::string outputFormat;
	singleWordBuffer >> outputFormat;

	auto format = [outputFormat](const BonusList & b) -> std::string
	{
		if(outputFormat == "json")
			return b.toJsonNode().toCompactString();

		std::ostringstream ss;
		ss << b;
		return ss.str();
	};
		printCommandMessage("Bonuses of " + GAME->interface()->localState->getCurrentArmy()->getObjectName().toString(&GAME->translator()) + "\n");
		printCommandMessage(format(*GAME->interface()->localState->getCurrentArmy()->getAllBonuses(Selector::all)) + "\n");

	printCommandMessage("\nInherited bonuses:\n");
	TCNodes parents;
		GAME->interface()->localState->getCurrentArmy()->getDirectParents(parents);
	for(const CBonusSystemNode *parent : parents)
	{
		printCommandMessage(std::string("\nBonuses from ") + typeid(*parent).name() + "\n" + format(*parent->getAllBonuses(Selector::all)) + "\n");
	}
}

void ClientCommandManager::handleTellCommand(std::istringstream& singleWordBuffer)
{
	std::string what;
	int id1;
	int id2;
	singleWordBuffer >> what >> id1 >> id2;

	if(what == "hs")
	{
		for(const CGHeroInstance* h : GAME->interface()->cb->getHeroesInfo())
			if(h->getHeroTypeID().getNum() == id1)
				if(const CArtifactInstance* a = h->getArt(ArtifactPosition(id2)))
					printCommandMessage(a->nodeName());
	}
}

void ClientCommandManager::handleMpCommand()
{
	if(const CGHeroInstance* h = GAME->interface()->localState->getCurrentHero())
		printCommandMessage(std::to_string(h->movementPointsRemaining()) + "; max: " + std::to_string(h->movementPointsLimit()) + "\n");
}

void ClientCommandManager::handleSetCommand(std::istringstream& singleWordBuffer)
{
	std::string what;
	std::string value;
	singleWordBuffer >> what;

	Settings config = settings.write["session"][what];

	singleWordBuffer >> value;

	if(value == "on")
	{
		config->Bool() = true;
		printCommandMessage("Option " + what + " enabled!", ELogLevel::INFO);
	}
	else if(value == "off")
	{
		config->Bool() = false;
		printCommandMessage("Option " + what + " disabled!", ELogLevel::INFO);
	}
}

void ClientCommandManager::handleCrashCommand()
{
	int* ptr = nullptr;
	*ptr = 666;
	//disaster!
}

void ClientCommandManager::handleVsLog(std::istringstream & singleWordBuffer)
{
	std::string key;
	singleWordBuffer >> key;

	logVisual->setKey(key);
}

void ClientCommandManager::handleWhoIsTheBossCommand(std::istringstream & singleWordBuffer)
{
	std::string value;
	singleWordBuffer >> value;

	Settings session = settings.write["session"];
	if(value == "on")
		session["showAiHeroOverlay"].Bool() = true;
	else if(value == "off")
		session["showAiHeroOverlay"].Bool() = false;
	else
		printCommandMessage("Unexpected syntax. Supported forms (case insensitive):\n/whoIsTheBoss on\n/whoIsTheBoss off\n");

	ENGINE->windows().totalRedraw();
}

void ClientCommandManager::handleAutoHeroCommand(std::istringstream & singleWordBuffer)
{
	if(!GAME->interface() || !GAME->interface()->localState)
	{
		printCommandMessage("AutoHeroes: no active human interface", ELogLevel::WARN);
		return;
	}

	const CGHeroInstance * hero = GAME->interface()->localState->getCurrentHero();
	if(!hero)
	{
		printCommandMessage("AutoHeroes: select a hero first", ELogLevel::WARN);
		return;
	}

	auto config = AutoHeroes::readHeroConfig(hero->id);
	auto persistConfig = [&]()
	{
		AutoHeroes::writeHeroConfig(hero->id, config);
		GAME->interface()->localState->saveState();
	};

	std::string subcommand;
	singleWordBuffer >> subcommand;
	boost::to_lower(subcommand);

	auto printStatus = [&]()
	{
		std::vector<std::string> actions;
		for(const auto action : config.priority)
			if(config.actions.contains(action))
				actions.push_back(AutoHeroes::actionToString(action));

		std::vector<std::string> priority;
		for(const auto action : config.priority)
			priority.push_back(AutoHeroes::actionToString(action));

		std::ostringstream out;
		out << "AutoHeroes " << hero->getNameTextID()
			<< ": " << (config.enabled ? "ON" : "OFF")
			<< "; actions=" << boost::algorithm::join(actions, ",")
			<< "; priority=" << boost::algorithm::join(priority, ">")
			<< "; recruit=" << (config.recruitmentScope == AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY ? "own" : "any")
			<< "; foreignSlots=" << config.maxForeignFactionSlots
			<< "; goldReserve=" << config.goldReserve
			<< "; radius=" << config.movementRadius;
		printCommandMessage(out.str(), ELogLevel::INFO);
	};

	if(subcommand.empty() || subcommand == "status")
	{
		printStatus();
		return;
	}

	if(subcommand == "on")
	{
		config.enabled = true;
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "off")
	{
		config.enabled = false;
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "actions")
	{
		config.actions.clear();
		std::string action;
		while(singleWordBuffer >> action)
		{
			boost::to_lower(action);
			if(action == "collect") config.actions.insert(AutoHeroes::Action::COLLECT_RESOURCES);
			else if(action == "level") config.actions.insert(AutoHeroes::Action::LEVEL_UP);
			else if(action == "recruit") config.actions.insert(AutoHeroes::Action::RECRUIT_CREATURES);
			else if(action == "explore") config.actions.insert(AutoHeroes::Action::EXPLORE);
			else if(action == "capture") config.actions.insert(AutoHeroes::Action::CAPTURE_OBJECTS);
			else if(action == "fight") config.actions.insert(AutoHeroes::Action::FIGHT_NEUTRALS);
		}
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "priority")
	{
		std::vector<AutoHeroes::Action> newPriority;
		std::string action;
		while(singleWordBuffer >> action)
		{
			boost::to_lower(action);
			std::optional<AutoHeroes::Action> parsed;
			if(action == "collect") parsed = AutoHeroes::Action::COLLECT_RESOURCES;
			else if(action == "level") parsed = AutoHeroes::Action::LEVEL_UP;
			else if(action == "recruit") parsed = AutoHeroes::Action::RECRUIT_CREATURES;
			else if(action == "explore") parsed = AutoHeroes::Action::EXPLORE;
			else if(action == "capture") parsed = AutoHeroes::Action::CAPTURE_OBJECTS;
			else if(action == "fight") parsed = AutoHeroes::Action::FIGHT_NEUTRALS;

			if(parsed && std::find(newPriority.begin(), newPriority.end(), *parsed) == newPriority.end())
				newPriority.push_back(*parsed);
		}

		if(newPriority.empty())
		{
			printCommandMessage("Usage: /autohero priority level recruit collect explore capture fight", ELogLevel::WARN);
			return;
		}

		for(const auto existing : config.priority)
			if(std::find(newPriority.begin(), newPriority.end(), existing) == newPriority.end())
				newPriority.push_back(existing);

		config.priority = std::move(newPriority);
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "radius")
	{
		int radius = 0;
		if(!(singleWordBuffer >> radius))
		{
			printCommandMessage("Usage: /autohero radius <tiles>; 0 = unlimited", ELogLevel::WARN);
			return;
		}
		config.movementRadius = std::max(0, radius);
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "recruit")
	{
		std::string mode;
		singleWordBuffer >> mode;
		boost::to_lower(mode);
		if(mode == "own")
		{
			config.recruitmentScope = AutoHeroes::RecruitmentScope::HERO_FACTION_ONLY;
			config.maxForeignFactionSlots = 0;
		}
		else if(mode == "any")
		{
			config.recruitmentScope = AutoHeroes::RecruitmentScope::UNRESTRICTED;
			int slots = -1;
			if(singleWordBuffer >> slots)
				config.maxForeignFactionSlots = std::clamp(slots, -1, 7);
			else
				config.maxForeignFactionSlots = -1;
		}
		else
		{
			printCommandMessage("Usage: /autohero recruit own | any [0..7]", ELogLevel::WARN);
			return;
		}
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "gold")
	{
		int reserve = 0;
		if(!(singleWordBuffer >> reserve))
		{
			printCommandMessage("Usage: /autohero gold <amount>", ELogLevel::WARN);
			return;
		}
		config.goldReserve = std::max(0, reserve);
		persistConfig();
		printStatus();
		return;
	}

	if(subcommand == "combat")
	{
		std::string mode;
		singleWordBuffer >> mode;
		boost::to_lower(mode);
		if(mode == "off") config.combatPolicy = AutoHeroes::CombatPolicy::DISABLED;
		else if(mode == "safe") config.combatPolicy = AutoHeroes::CombatPolicy::SAFE_ONLY;
		else if(mode == "losses") config.combatPolicy = AutoHeroes::CombatPolicy::ALLOW_SMALL_LOSSES;
		else
		{
			printCommandMessage("Usage: /autohero combat off | safe | losses", ELogLevel::WARN);
			return;
		}
		persistConfig();
		printStatus();
		return;
	}

	printCommandMessage(
		"AutoHeroes commands: /autohero on|off|status; /autohero actions collect level recruit explore capture fight; "
		"/autohero priority <ordered actions>; /autohero recruit own|any [0..7]; /autohero gold <amount>; "
		"/autohero radius <tiles>; /autohero combat off|safe|losses",
		ELogLevel::INFO);
}

void ClientCommandManager::handleGenerateAssets()
{
	ENGINE->renderHandler().exportGeneratedAssets();
	printCommandMessage("All assets generated");
}

void ClientCommandManager::printCommandMessage(const std::string &commandMessage, ELogLevel::ELogLevel messageType)
{
	switch(messageType)
	{
		case ELogLevel::NOT_SET:
			std::cout << commandMessage;
			break;
		case ELogLevel::TRACE:
			logGlobal->trace(commandMessage);
			break;
		case ELogLevel::DEBUG:
			logGlobal->debug(commandMessage);
			break;
		case ELogLevel::INFO:
			logGlobal->info(commandMessage);
			break;
		case ELogLevel::WARN:
			logGlobal->warn(commandMessage);
			break;
		case ELogLevel::ERROR:
			logGlobal->error(commandMessage);
			break;
		default:
			std::cout << commandMessage;
			break;
	}

	if(currentCallFromIngameConsole)
	{
		// commands run with ENGINE->interfaceMutex already locked, so the message is handed
		// over to the main thread instead of locking the non-recursive mutex a second time
		ENGINE->dispatchMainThread([commandMessage]()
		{
			if(GAME->interface() && GAME->interface()->cingconsole)
				GAME->interface()->cingconsole->addMessage("", "System", commandMessage);
		});
	}
}

void ClientCommandManager::giveTurn(const PlayerColor &colorIdentifier)
{
	GAME->server().client->giveTurnLocally(colorIdentifier);
}

void ClientCommandManager::processCommand(const std::string & message, bool calledFromIngameConsole)
{
	// split the message into individual words
	std::istringstream singleWordBuffer;
	singleWordBuffer.str(message);

	// get command name, to be used for single word commands
	std::string commandName;
	singleWordBuffer >> commandName;

	currentCallFromIngameConsole = calledFromIngameConsole;

	if(message == std::string("die, fool"))
		handleQuitCommand();

	else if(commandName == "save")
		handleSaveCommand(singleWordBuffer);

	else if(commandName=="load")
		handleLoadCommand(singleWordBuffer); // not implemented

	else if(commandName == "autoskip")
		handleAutoskipCommand();

	else if(commandName == "controlai")
		handleControlaiCommand(singleWordBuffer);

	else if(commandName == "setBattleAI")
		handleSetBattleAICommand(singleWordBuffer);

	else if(commandName == "antilag")
		handleAntilagCommand(singleWordBuffer);

	else if(commandName == "redraw")
		handleRedrawCommand();

	else if(message=="translate" || message=="translate game")
		handleTranslateGameCommand(false);

	else if(message=="translate missing")
		handleTranslateGameCommand(true);

	else if(message=="translate maps")
		handleTranslateMapsCommand();

	else if(message=="get config")
		handleGetConfigCommand();

	else if(message=="get txt")
		handleGetTextCommand();

	else if(commandName == "def2bmp")
		handleDef2bmpCommand(singleWordBuffer);

	else if(commandName == "extract")
		handleExtractCommand(singleWordBuffer);

	else if(commandName == "bonuses")
		handleBonusesCommand(singleWordBuffer);

	else if(message == "obstacles debug")
		handleObstaclesDebugCommand();

	else if(commandName == "tell")
		handleTellCommand(singleWordBuffer);

	else if(commandName == "mp" && GAME->interface())
		handleMpCommand();

	else if (commandName == "set")
		handleSetCommand(singleWordBuffer);

	else if(commandName == "crash")
		handleCrashCommand();

	else if(commandName == "vslog")
		handleVsLog(singleWordBuffer);

	else if(boost::iequals(commandName, "whoistheboss"))
		handleWhoIsTheBossCommand(singleWordBuffer);

	else if(boost::iequals(commandName, "autohero"))
		handleAutoHeroCommand(singleWordBuffer);

	else if(message=="generate assets")
		handleGenerateAssets();

	else
	{
		if (!commandName.empty() && !vstd::iswithin(commandName[0], 0, ' ')) // filter-out debugger/IDE noise
			printCommandMessage("Command not found :(", ELogLevel::ERROR);
	}
}
