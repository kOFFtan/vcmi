from pathlib import Path
import re, sys

ROOT = Path('.')

def read(p):
    path = ROOT / p
    if not path.exists():
        raise SystemExit(f'MISSING: {p}')
    return path, path.read_text()

def write(path, text):
    path.write_text(text)

# 1) Force the correct AutoHeroes turn handoff in NetPacksClient.cpp.
p, s = read('client/NetPacksClient.cpp')
start_re = re.compile(r'''void ApplyClientNetPackVisitor::visitPlayerStartsTurn\(PlayerStartsTurn & pack\)\n\{.*?\n\}\n\n(?=void ApplyClientNetPackVisitor::visitPlayerEndsTurn)''', re.S)
start_fn = '''void ApplyClientNetPackVisitor::visitPlayerStartsTurn(PlayerStartsTurn & pack)\n{\n\tlogNetwork->debug("Server gives turn to %s", pack.player.toString());\n\n\t// AutoHeroes synthesizes PlayerStartsTurn locally to start Nullkiller2 for\n\t// the remainder of the current human turn. Never restore the human interface\n\t// here, otherwise the AI is destroyed before AIGateway::yourTurn() runs.\n\tif(AutoHeroes::isPhaseActive() && AutoHeroes::phasePlayer() == pack.player)\n\t\tlogGlobal->info("AutoHeroes v0.5: PlayerStartsTurn -> Nullkiller2 (human UI stays detached)");\n\n\tcallAllInterfaces(cl, &IGameEventsReceiver::playerStartsTurn, pack.player);\n\tcallOnlyThatInterface(cl, pack.player, &CGameInterface::yourTurn, pack.queryID);\n}\n\n'''
s2, n = start_re.subn(start_fn, s, count=1)
if n != 1:
    raise SystemExit('Could not rewrite visitPlayerStartsTurn')
s = s2

end_re = re.compile(r'''void ApplyClientNetPackVisitor::visitPlayerEndsTurn\(PlayerEndsTurn & pack\)\n\{.*?\n\}\n\n(?=void ApplyClientNetPackVisitor::visitTurnTimeUpdate)''', re.S)
m = end_re.search(s)
if not m:
    raise SystemExit('Could not locate visitPlayerEndsTurn')
old_end = m.group(0)
# Preserve the upstream aiSolo body, but force AutoHeroes restore immediately after callAllInterfaces.
body = old_end
# Remove any previous AutoHeroes restore block/comments between callAllInterfaces and aiSolo check.
body = re.sub(
    r'''(\tcallAllInterfaces\(cl, &IGameEventsReceiver::playerEndsTurn, pack.player\);\n)(?:\n|\t//.*\n|\tif\(AutoHeroes::isPhaseActive\(\).*?\n(?:\t\{.*?\n\t\}|\t\tcl\.finishAutoHeroesPhase\(pack.player\);)\n)*?(\n\tif\(!settings\["session"\]\["aiSolo"\])''',
    r'''\1\n\t// Restore the human interface only after Nullkiller2 has received the real\n\t// PlayerEndsTurn and dropped its haveTurn state.\n\tif(AutoHeroes::isPhaseActive() && AutoHeroes::phasePlayer() == pack.player)\n\t{\n\t\tlogGlobal->info("AutoHeroes v0.5: PlayerEndsTurn -> restore human interface");\n\t\tcl.finishAutoHeroesPhase(pack.player);\n\t}\n\2''',
    body,
    flags=re.S
)
s = s[:m.start()] + body + s[m.end():]
write(p, s)

# 2) Add visible/log build marker around handoff in Client.cpp.
p, s = read('client/Client.cpp')
s = s.replace('logGlobal->info("AutoHeroes phase for player %s will be handled by %s", color.toString(), aiName);',
              'logGlobal->info("AutoHeroes v0.5: phase for player %s will be handled by %s", color.toString(), aiName);')
s = s.replace('logGlobal->info("AutoHeroes phase for player %s finished; restoring human interface", color.toString());',
              'logGlobal->info("AutoHeroes v0.5: phase for player %s finished; restoring human interface", color.toString());')
needle = '\tinstallNewPlayerInterface(AIFactory::createAdventureAI(aiName), color);\n\tgiveTurnLocally(color);'
replacement = '\tinstallNewPlayerInterface(AIFactory::createAdventureAI(aiName), color);\n\tlogGlobal->info("AutoHeroes v0.5: Nullkiller2 installed for %s; dispatching synthetic turn start", color.toString());\n\tgiveTurnLocally(color);'
if needle in s:
    s = s.replace(needle, replacement, 1)
elif 'AutoHeroes v0.5: Nullkiller2 installed' not in s:
    raise SystemExit('Could not add Client.cpp handoff marker')
write(p, s)

# 3) Make "Run auto turn" safe and add visible v0.5 marker to the window.
p, s = read('client/windows/CAutoHeroWindow.cpp')
s = s.replace('std::string title = tr("vcmi.autoHeroes.title") + ": " + GAME->translator().translate(hero->getNameTextID());',
              'std::string title = tr("vcmi.autoHeroes.title") + " v0.5: " + GAME->translator().translate(hero->getNameTextID());')
run_re = re.compile(r'''void CAutoHeroWindow::saveAndRun\(\)\n\{.*?\n\}\n''', re.S)
run_fn = '''void CAutoHeroWindow::saveAndRun()\n{\n\tpersistSettings();\n\n\t// Never use the run-now button as a disguised End Turn button. If persistent\n\t// AutoHeroes is disabled (or no actions are allowed), just save and close.\n\tif(!draft.enabled || draft.actions.empty())\n\t{\n\t\tlogGlobal->warn("AutoHeroes v0.5: run-now ignored for hero %s because automation is disabled or no actions are selected", hero->getNameTextID());\n\t\tclose();\n\t\treturn;\n\t}\n\n\tclose();\n\tENGINE->dispatchMainThread([]()\n\t{\n\t\tif(adventureInt)\n\t\t\tadventureInt->hotkeyEndingTurn();\n\t});\n}\n'''
s2, n = run_re.subn(run_fn, s, count=1)
if n != 1:
    raise SystemExit('Could not rewrite saveAndRun')
write(p, s2)

print('AutoHeroes Stage 5 source fix applied successfully.')
