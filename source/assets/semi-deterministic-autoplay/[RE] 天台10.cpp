/*
MIT License

Copyright (c) 2025 Reisen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <avz.h>
#include <dsl/shorthand.h>
#include <mod/mod.h>
#include <ShowWavelength/ShowWavelength.h>

using namespace std;

class : AStateHook {
private:
    int cnt;

    void _BeforeScript() override {
        cnt = 0;
        OnWave(1_20) [this] {
            cnt += AObjSelector(&AZombie::Type, AGIGA_GARGANTUAR, &AZombie::ExistTime, 0).Count();
        };
    }

public:
    operator int() const {
        return cnt;
    }
} gigaCount;

int GetCobReadyTime(int nCobs = 1) {
    auto cobs = aCobManager.GetRecoverList();
    return cobs[nCobs - 1].recoverTime + (aFieldInfo.isRoof ? 387 : 373);
}

unordered_map<string, ATimeline> states;
string lastState, currentState;

AOnBeforeScript(
    lastState = "none";
    currentState = "none";
)

struct _TransitionKey {
    variant<string, set<int>> key;

    pair<_TransitionKey, string> operator=(std::string_view value) const {
        return {*this, string(value)};
    }
} activate{"activate"}, delay{"delay"}, nogiga{"nogiga"}, final{set<int>{9, 19}};

_TransitionKey WaveIs(std::convertible_to<int> auto... waves) {
    return {set<int>{waves...}};
}

void _ParseTransition(unordered_map<string, string>& out1, unordered_map<int, string>& out2, pair<_TransitionKey, string> trans, auto... args) {
    if (auto value = std::get_if<std::string>(&trans.first.key))
        out1[*value] = trans.second;
    else if (auto value = std::get_if<set<int>>(&trans.first.key))
        for (int wave : *value)
            out2[wave] = trans.second;
    if constexpr (sizeof...(args) > 0)
        _ParseTransition(out1, out2, args...);
}

ATimeline Transition(pair<int, int> wl, auto... args) {
    unordered_map<string, string> dict1;
    unordered_map<int, string> dict2;
    _ParseTransition(dict1, dict2, args...);
    bool hasDelay = dict1.contains("delay");
    auto checkRefresh = [=](int wave) -> ACoroutine {
        if (ANowTime(wave + 1) > -200) {
            int realWl = ANowTime(wave) - ANowTime(wave + 1);
            aLogger->Error("第 {} 波提前刷新，预期波长 >= {}cs，实际波长 {}cs", wave, wl.first, realWl);
            co_return;
        }
        if (wl.first != wl.second)
            co_await (wl.second - wl.first);
        if (ANowWave(true) == wave && !hasDelay)
            aLogger->Error("转移 delay 未指定");
    };
    auto transition = [=]() -> ACoroutine {
        int wave = ANowWave(false);
        if (hasDelay)
            co_await [=] { return ANowWave(true) != wave || ANowTime(wave) >= wl.second - 200; };
        else if (wl.first != wl.second)
            co_await [=] { return ANowWave(true) != wave; };
        int nextWave = hasDelay ? ANowWave(true) : wave + 1;
        string nextStateKey, nextState;
        if (wave == nextWave) { // 未刷新下一波
            nextStateKey = "delay";
            nextState = dict1.at("delay");
        } else if (dict2.contains(nextWave)) { // 下一波指定了转移
            nextStateKey = std::format("wave{}", nextWave);
            nextState = dict2.at(nextWave);
        } else if (gigaCount >= 50 && dict1.contains("nogiga")) { // 变速
            nextStateKey = "nogiga";
            nextState = dict1.at("nogiga");
        } else {
            nextStateKey = "activate";
            if (!dict1.contains("activate")) {
                aLogger->Error("转移 activate 未指定");
                co_return;
            }
            nextState = dict1.at("activate");
        }
        if (!states.contains(nextState)) {
            aLogger->Error("状态 {} 不存在", nextState);
            co_return;
        }

        aLogger->Debug("{} --{}--> {}", currentState, nextStateKey, nextState);
        lastState = currentState;
        currentState = nextState;
        OnWave(nextWave) states[nextState];
    };
    return [=](ATime t) {
        if (t.time != 0)
            return;
        AConnect(t + wl.first - 200, bind(checkRefresh, t.wave));
        if (hasDelay || wl.first != wl.second)
            AConnect(t + wl.first - 200, transition);
        else {
            if (!ARangeIn(t.wave, {0, 9, 19, 20}))
                AAssumeWavelength(t.wave, wl.first);
            if (ANowTime(t.wave) > 1)
                ACoLaunch(transition);
            else
                AConnect(t + 1, transition);
        }
    };
}

ATimeline Transition(int wl, auto... args) {
    return Transition({wl, wl}, args...);
}

void StartTransition(int wave, const string& state) {
    int offset = -200;
    if (wave == 0)
        offset = 1;
    else if (wave == 1)
        offset = -599;
    else if (wave % 10 == 0)
        offset = -945;
    At(ATime(wave, offset)) Do {
        if (!states.contains(state)) {
            aLogger->Error("状态 {} 不存在", state);
            return;
        }
        aLogger->Debug("{} --start--> {}", currentState, state);
        lastState = currentState;
        currentState = state;
        OnWave(wave) states[state];
    };
}

ATimeline EndingHelper(const vector<int>& times, const vector<ATimeline>& ops, int withdrawThreshold = 0) {
    if (ops.size() != 1 && ops.size() != times.size()) {
        aLogger->Error("EndingHelper 操作数目与时间点数目不匹配");
        return {};
    }
    for (const auto& op : ops) {
        if (op.GetMinOffset().wave != 0) {
            aLogger->Error("EndingHelper 不支持跨波 Timeline");
            return {};
        }
    }
    vector<int> startTimes;
    for (int i = 0; i < times.size(); ++i)
        startTimes.push_back(times[i] + ops[i % ops.size()].GetMinOffset().time);
    vector<int> sortedIndices(times.size());
    iota(sortedIndices.begin(), sortedIndices.end(), 0);
    sort(sortedIndices.begin(), sortedIndices.end(), [&](int a, int b) {
        return startTimes[a] < startTimes[b];
    });
    return CoDo {
        int wave = ANowWave();
        int cnt = 0;
        for (auto idx : sortedIndices) {
            int t = startTimes[idx];
            auto& op = ops[idx % ops.size()];
            ATimeOffset offset = op.GetMinOffset();
            co_await (ATime(wave, t));
            if (ANowTime(wave + 1) > withdrawThreshold + offset.time)
                break;
            bool found = false;
            for (auto& zombie : aAliveZombieFilter) {
                if (zombie.MRef<bool>(0xb8) || zombie.Type() == AIMP || zombie.Type() == ABACKUP_DANCER)
                    continue;
                if (zombie.Type() == ADANCING_ZOMBIE && AMaidCheats::Phase() == AMaidCheats::MC_DANCING)
                    continue;
                found = true;
                break;
            }
            if (found) {
                At(now - offset) op;
                cnt++;
            }
        }
        aLogger->Debug("EndingHelper 执行了 {} 个操作", cnt);
    };
}

ATimeline EndingHelper(const vector<int>& times, const ATimeline& op, int withdrawThreshold = 0) {
    return EndingHelper(times, vector<ATimeline>{op}, withdrawThreshold);
}

// ----------------------------------------------------------------

#define LOG

#ifdef LOG
constexpr auto RELOAD_MODE = AReloadMode::MAIN_UI_OR_FIGHT_UI;
constexpr float GAME_SPEED = 10.0;
constexpr int SELECT_CARDS_INTERVAL = 0;
constexpr bool SKIP_TICK = true;
#elifdef TEST
constexpr auto RELOAD_MODE = AReloadMode::MAIN_UI_OR_FIGHT_UI;
constexpr float GAME_SPEED = 10.0;
constexpr int SELECT_CARDS_INTERVAL = 0;
constexpr bool SKIP_TICK = false;
#elifdef DEMO
constexpr auto RELOAD_MODE = AReloadMode::MAIN_UI;
constexpr float GAME_SPEED = 1.0;
constexpr int SELECT_CARDS_INTERVAL = 0;
constexpr bool SKIP_TICK = false;
#else
constexpr auto RELOAD_MODE = AReloadMode::MAIN_UI_OR_FIGHT_UI;
constexpr float GAME_SPEED = 2.0;
constexpr int SELECT_CARDS_INTERVAL = 0;
constexpr bool SKIP_TICK = false;
#endif

#if defined (LOG) || defined (TEST)
AOnExitFight(
    if (AGetPvzBase()->GameUi() == AAsm::ZOMBIES_WON) {
        ABackToMain(false);
        AEnterGame(AAsm::SURVIVAL_ENDLESS_STAGE_5);
    }
)
#endif

#ifdef LOG
ALogger<AFile> logger("D:\\tt10.log");
#else
ALogger<AConsole> logger;
#endif

// LI4/bIyUZCe5mPln42JUDXRU9tWozTx2cP9VVIgtcJlQ
void AScript() {
    ASetInternalLogger(logger);
    logger.SetHeaderStyle("[{flag}f, {wave}, {time}][{level}] ");
    #ifdef LOG
    logger.SetLevel({ALogLevel::DEBUG, ALogLevel::WARNING, ALogLevel::ERROR});
    #endif
    ASetReloadMode(RELOAD_MODE);
    AConnect('Q', []{ ATerminate(); });
    ASetGameSpeed(GAME_SPEED);
    EnableModsScoped(SaveDataReadOnly, CobFixedDelay, DisableItemDrop);
    //ASetZombies(ACreateRandomTypeList({AGIGA_GARGANTUAR}), ASetZombieMode::INTERNAL);
    ASelectCards("INK", {AFLOWER_POT, AM_FLOWER_POT}, SELECT_CARDS_INTERVAL);
    if (SKIP_TICK) {
        ASkipTick(AAlwaysTrue());
        EnableModsScoped(AccelerateGame);
    }
    if (!SKIP_TICK)
        ShowWavelength(true);
    #ifdef LOG
    At(now) CoDo {
        vector<AGrid> cobPos;
        for (auto& plant : AObjSelector(&APlant::Type, ACOB_CANNON))
            cobPos.emplace_back(plant.Row() + 1, plant.Col() + 1);
        while (true) {
            co_await 100;
            auto cobs = AGetPlantPtrs(cobPos, ACOB_CANNON);
            for (int i = 0; i < cobPos.size(); ++i)
                if (cobs[i] == nullptr)
                    aLogger->Error("位于 {} 的炮不存在", cobPos[i]);
                else if (cobs[i]->Hp() < 100) {
                    aLogger->Error("位于 {} 的炮血量低：{}", cobPos[i], cobs[i]->Hp());
                    cobs[i]->Hp() = 300;
                }
        }
    };
    OnWave(1_21) At(-200) Do {
        int wave = ANowWave(false);
        aLogger->Debug("w{} 波长: {}cs", wave, ANowTime(wave) + 200);
    };
    #endif

    ATimeline pp = P(slope, 2, 9, flat, 4, 9);

    states["PP"] = {
        Transition({695, 9999}, activate = "PP", final = "final"),
        At(495) pp,
    };
    states["w10"] = {
        Transition({601, 9999}, activate = "PP"),
        At(300) N(3, 9),
        At(398) pp,
    };
    states["final"] = {
        At(495) pp,
        EndingHelper({1200, 1900, 2600}, pp, 398 - 695),
    };

    StartTransition(1, "PP");
    StartTransition(10, AGetZombieTypeList()[AGIGA_GARGANTUAR] ? "w10" : "PP");
    OnWave(20) {
        At(0) I(3, 8) & Shovel(3, 8),
        At(495) pp,
        EndingHelper({1500, 2200, 2900}, pp),
    };
}
