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

#define RECORD

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
#elifdef RECORD
constexpr auto RELOAD_MODE = AReloadMode::MAIN_UI_OR_FIGHT_UI;
constexpr float GAME_SPEED = 5.0;
constexpr int SELECT_CARDS_INTERVAL = 0;
constexpr bool SKIP_TICK = false;
int recordRounds = 100;
#elifdef DEMO
constexpr auto RELOAD_MODE = AReloadMode::MAIN_UI;
constexpr float GAME_SPEED = 1.0;
constexpr int SELECT_CARDS_INTERVAL = 17;
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
        AEnterGame(AAsm::SURVIVAL_ENDLESS_STAGE_3);
    }
)
#endif

#ifdef LOG
ALogger<AFile> logger("D:\\sb16.log");
#else
ALogger<AConsole> logger;
#endif

// LI435MXInVRWU/XM1ZUaJvmwHjBF09dafEzPXPJ6UljHVfuKQS1W
void AScript() {
    #ifdef RECORD
    if (--recordRounds < 0)
        ATerminate();
    #endif
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
    if (AGetMainObject()->CompletedRounds() == 1000)
        ASetZombies({AZOMBIE, APOLE_VAULTING_ZOMBIE, ADANCING_ZOMBIE, ADOLPHIN_RIDER_ZOMBIE, ABALLOON_ZOMBIE, APOGO_ZOMBIE, ABUNGEE_ZOMBIE, ALADDER_ZOMBIE, ACATAPULT_ZOMBIE, AGARGANTUAR, AGIGA_GARGANTUAR}, ASetZombieMode::INTERNAL);
    #ifdef DEMO
    ASetWavelength({{5, 1739}, {8, 659}});
    #endif
    if (AGetZombieTypeList()[AGIGA_GARGANTUAR])
        ASelectCards("IINKLCCCC", {}, SELECT_CARDS_INTERVAL);
    else
        ASelectCards("INJWKCCC", {AM_PUFF_SHROOM}, SELECT_CARDS_INTERVAL);
    if (SKIP_TICK) {
        ASkipTick(AAlwaysTrue());
        EnableModsScoped(AccelerateGame);
    }
    if (!SKIP_TICK)
        ShowWavelength(true);
    #ifdef LOG
    OnWave(1_8, 10_18) Do {
        double refreshRatio = aRandom.Choice({0.5, 0.65});
        AGetMainObject()->ZombieRefreshHp() = AGetMainObject()->MRef<int>(0x5598) * refreshRatio;
    };
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
        }
    };
    #endif

    // 配置存冰和女仆
    aIceFiller.Start({{1, 1}, {2, 1}, {5, 1}, {6, 1}});
    aIceFiller.SetPriorityMode(AIceFiller::HP);
    AMaidCheats::Dancing();
    OnWave(1) Do { AMaidCheats::Stop(); };
    OnWave(9, 19, 20) {
        At(0) Do { AMaidCheats::Dancing(); },
        At(next_wave - 1) Do { AMaidCheats::Stop(); },
    };

    // 红眼关起手：NDD 601
    states["hb_NDD"] = {
        Transition(601, activate = "hb_PPDD", delay = "hb_(NDD-)PP", nogiga = "b_PPDD"),
        At(292) N({{3, 9}, {4, 9}}) & DD<106>(9),
    };
    // NDD延迟：NDD-PP 1092
    states["hb_(NDD-)PP"] = {
        Transition(1092, activate = "hb_IPP"),
        At(892) PP(), // 892 = 601 + 291
    };

    // 冰波：IPP-PP 1248
    states["hb_IPP"] = {
        Transition(659, delay = "hb_(IPP-)PP", activate = "hb_(IPP|)cPPI", WaveIs(8, 18) = "hb_(IPP|)cPPI_w8", nogiga = "trans_cPP", final = "hb_final"),
        At(1) I(),
        At(459) P(15, 8.325),
        At(1048) PP(8.75), // 1048 = 659 + 389
    };
    states["hb_(IPP-)PP"] = {
        Transition(1248, activate = "hb_PPDD", delay = "hb_(IPP-PP-)cPP", nogiga = "b_PPDD", final = "hb_final"),
    };
    // 加速波：PPDD 601
    states["hb_PPDD"] = {
        Transition(601, activate = "hb_IPP", nogiga = "trans_PPI", final = "hb_final"),
        At(291) PP() & DD<107>(9),
    };
    // 冰波延迟：IPP-PP-cPP 1739
    states["hb_(IPP-PP-)cPP"] = {
        Transition(1739, activate = "hb_IPP", nogiga = "b_PPDD", final = "hb_final"),
        At(1300) C.TriggerBy(AGIGA_GARGANTUAR & CURR_WAVE)(266),
        At(1539) PP(), // 1539 = 1248 + 291
    };
    // 冰波意外刷新：IPP|cPPI 659|601
    states["hb_(IPP|)cPPI"] = {
        Transition(601, activate = "hb_PPDD", nogiga = "b_PPDD", final = "hb_final"),
        At(195) C.TriggerBy(ADANCING_ZOMBIE, ALADDER_ZOMBIE)(40),
        At(390) I(),
    };
    // 如果cPPI波出现在w8，需要调整为cPP|PPIc
    states["hb_(IPP|)cPPI_w8"] = {
        Transition(601, final = "hb_final"),
        At(195) C.TriggerBy(ADANCING_ZOMBIE, ALADDER_ZOMBIE)(40),
        At(next_wave + 401) C.TriggerBy(AGIGA_GARGANTUAR)(800),
    };

    // 收尾
    states["hb_final"] = At(-200) CoDo {
        // 发本波的激活炮
        ATime thisWave = now + 200;
        if (lastState == "hb_IPP") {
            // cPPI波的处理和其他波不同
            At(thisWave + 195) C.TriggerBy(ADANCING_ZOMBIE, ALADDER_ZOMBIE)(40);
            At(thisWave + 390) I();
        } else {
            At(thisWave + 291) PP();
            At(thisWave + 360) I();
        }

        co_await (thisWave + 401);
        if (ANowTime(true).time < 0) {
            // 如果收尾波直接刷了（波长1346）
            if (lastState == "hb_IPP") {
                // w8红还剩3血，w8撑杆还在；一炮收掉撑杆，红眼交给w10
                At(thisWave + 900) PP();
            } else if (lastState == "hb_(IPP-)PP" || lastState == "trans_cPP") {
                // w8红还剩2血，1510砸炮；垫一下红眼就行
                At(thisWave + 401) C.TriggerBy(AGIGA_GARGANTUAR)(800);
            } else if (lastState == "hb_PPDD" || lastState == "hb_(IPP-PP-)cPP") {
                // w8红还剩1血，最快1161砸炮；用炮收掉
                At(thisWave + 1161) PP();
            }
        } else {
            // 随便炸炸
            At(now) EndingHelper({1000, 1500, 2300}, PP());
        }
    };

    // 白眼关&转白过渡
    // PPDD|cPP|PPc
    states["trans_PPI"] = {
        Transition(601, activate = "b_PPc", final = "b_final"),
        At(318) PP(),
        At(360) I(),
    };
    states["trans_cPP"] = {
        Transition(601, activate = "b_PPDD", final = "hb_final"),
        At(195) C.TriggerBy(ADANCING_ZOMBIE, ALADDER_ZOMBIE)(40),
    };
    states["b_PPDD"] = {
        Transition(601, activate = "b_cPP", delay = "b_(PPDD)-PP", final = "b_final"),
        // 这个270是紧复用：200+601*5+270=3475
        // I[PP]-PP-cPP|PPDD|cPP|PPc|PP[DD] -> DD最早392
        At(270) PP() & DD<122>(9),
    };
    states["b_(PPDD)-PP"] = {
        // 无红有铁桶的w10可能延迟
        Transition(1202, activate = "b_PPc", final = "b_final"),
        At(1002) PP(),
    };
    states["b_cPP"] = {
        Transition(601, activate = "b_PPc", final = "b_final"),
        At(195) C.TriggerBy(ADANCING_ZOMBIE, ALADDER_ZOMBIE)(40),
        At(389) PP(8.75),
    };
    states["b_PPc"] = {
        Transition(601, activate = "b_PPDD", final = "b_final"),
        At(318) PP(),
        At(599) C.TriggerBy(APOLE_VAULTING_ZOMBIE)(1),
    };

    // 白眼关收尾
    states["b_final"] = At(-200) Do {
        ATime thisWave = now + 200;
        if (GetCobReadyTime(4) <= 988) {
            // PPDD收尾，DD于788极限全收撑杆
            At(thisWave + 270) PP();
            At(thisWave) EndingHelper({788}, PP());
        } else {
            // cPP-PP收尾
            At(thisWave + 195) C.TriggerBy(ADANCING_ZOMBIE, ALADDER_ZOMBIE)(40);
            At(thisWave + 389) PP(8.75);
            At(thisWave) EndingHelper({1150}, PP());
        }
    };

    auto initialState = AGetZombieTypeList()[AGIGA_GARGANTUAR] ? "hb_NDD" : "b_PPDD";
    StartTransition(1, initialState);
    StartTransition(10, initialState);

    OnWave(20) {
        // 为了展示容错，我们冰消珊瑚
        At(96) I(),
        At(380) P(15, 9), // 热过渡
        At(953) PP(), // 全伤巨人
        At(1220) PP(), // 全伤撑杆
        EndingHelper({1600, 2300}, PP()),
    };
}
