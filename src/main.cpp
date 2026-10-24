#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <string>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Noclip Accuracy
//  - Noclip: no mueres al chocar (salvo el "pico anti-trampas" del juego).
//  - Precision = % de ticks de fisica SIN colision en el intento / el nivel.
//  - Golpes = numero de choques (choques en ticks seguidos cuentan como uno).
//  - Precision minima opcional: si baja de ese valor, mueres de verdad.
// ---------------------------------------------------------------------------

struct Counter {
    long long total = 0;  // ticks contados
    long long bad = 0;    // ticks con colision
    int hits = 0;         // golpes
    void reset() { *this = Counter{}; }
    double accuracy() const {
        return total > 0 ? 100.0 * static_cast<double>(total - bad) / static_cast<double>(total) : 100.0;
    }
};

// Ajustes en memoria (se actualizan solos al cambiarlos).
struct Cfg {
    bool enabled = false;
    bool p1 = true;
    bool p2 = true;
    bool showIndicator = true;
    bool showHits = true;
    bool colorByAcc = true;
    bool showResult = true;
    bool perAttempt = true;
    bool debugNotify = true;  // avisos de diagnostico (para comprobar que el mod funciona)
    double minAccuracy = 0.0;
    int grace = 240;
    int decimals = 2;
    int opacity = 60;
    int corner = 0;  // 0 arriba-izq, 1 arriba-der, 2 abajo-izq, 3 abajo-der

    void refresh();
};

struct State {
    static State& get() {
        static State s;
        return s;
    }
    Counter attempt;            // intento actual
    Counter level;              // todo el nivel
    bool hitThisStep = false;   // hubo colision en el tick en curso
    bool prevHit = false;       // hubo colision en el tick anterior
    bool usedNoclip = false;    // se uso noclip en este nivel
    bool notifiedHit = false;   // diagnostico: ya avisamos del primer golpe del intento
    bool dirty = true;
    float labelTimer = 0.f;
    CCLabelBMFont* label = nullptr;
    PlayLayer* owner = nullptr;
};

static Cfg& cfg() {
    static Cfg c;
    return c;
}

void Cfg::refresh() {
    auto mod = Mod::get();
    enabled = mod->getSettingValue<bool>("enabled");
    p1 = mod->getSettingValue<bool>("noclip-p1");
    p2 = mod->getSettingValue<bool>("noclip-p2");
    showIndicator = mod->getSettingValue<bool>("show-indicator");
    showHits = mod->getSettingValue<bool>("show-hits");
    colorByAcc = mod->getSettingValue<bool>("color-by-accuracy");
    showResult = mod->getSettingValue<bool>("show-result");
    perAttempt = mod->getSettingValue<std::string>("accuracy-mode") != "Whole level";
    debugNotify = mod->getSettingValue<bool>("debug-notify");
    minAccuracy = std::clamp(mod->getSettingValue<double>("min-accuracy"), 0.0, 100.0);
    grace = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("grace-ticks"), 0, 2400));
    decimals = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("decimals"), 0, 3));
    opacity = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("indicator-opacity"), 10, 100));

    auto cornerName = mod->getSettingValue<std::string>("indicator-corner");
    if (cornerName == "Top-right") corner = 1;
    else if (cornerName == "Bottom-left") corner = 2;
    else if (cornerName == "Bottom-right") corner = 3;
    else corner = 0;

    State::get().dirty = true;
}

$execute {
    cfg().refresh();

    geode::listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        cfg().refresh();
    });

    // Atajo: activar/desactivar el noclip.
    geode::listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            auto mod = Mod::get();
            mod->setSettingValue<bool>("enabled", !mod->getSettingValue<bool>("enabled"));
        }
        return false;
    });

    // Atajo: reiniciar los contadores.
    geode::listenForKeybindSettingPresses("reset-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            auto& s = State::get();
            s.attempt.reset();
            s.level.reset();
            s.dirty = true;
        }
        return false;
    });
}

namespace {
    void placeLabel(CCLabelBMFont* label, int corner) {
        auto size = CCDirector::get()->getWinSize();
        bool right = (corner == 1 || corner == 3);
        bool bottom = (corner >= 2);
        label->setAnchorPoint({right ? 1.f : 0.f, bottom ? 0.f : 1.f});
        label->setPosition({right ? size.width - 8.f : 8.f, bottom ? 8.f : size.height - 8.f});
    }

    // Prepara el indicador y los contadores de una partida.
    // Se llama al cargar el nivel y, por seguridad, tambien desde postUpdate.
    void prepare(PlayLayer* pl, bool resetCounters) {
        auto& s = State::get();
        cfg().refresh();  // lee los ajustes otra vez, por si algun cambio no llego por evento

        if (resetCounters) {
            s.attempt.reset();
            s.level.reset();
            s.hitThisStep = false;
            s.prevHit = false;
            s.usedNoclip = false;
            s.notifiedHit = false;
        }
        if (!pl->m_uiLayer) return;

        if (s.owner != pl || !s.label) {
            s.owner = pl;
            auto label = CCLabelBMFont::create("", "bigFont.fnt");
            label->setID("noclip-accuracy-indicator"_spr);
            label->setScale(0.4f);
            pl->m_uiLayer->addChild(label, 100);
            s.label = label;

            if (cfg().debugNotify) {
                std::string text = fmt::format("Noclip Accuracy cargado | noclip {}", cfg().enabled ? "ON" : "OFF");
                Notification::create(text.c_str(), NotificationIcon::Info, 3.f)->show();
            }
        }
        s.dirty = true;
    }
}

// Cuenta ticks: se llama una vez por cada paso de fisica del juego.
class $modify(NoclipBGL, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (isHalfTick) return;  // un choque en medio tick se anota en el tick completo

        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;

        auto& s = State::get();
        auto& c = cfg();
        if (!c.enabled || m_levelEndAnimationStarted) {
            s.hitThisStep = false;
            return;
        }

        s.usedNoclip = true;
        bool hit = s.hitThisStep;
        s.hitThisStep = false;

        for (Counter* ctr : {&s.attempt, &s.level}) {
            ++ctr->total;
            if (hit) {
                ++ctr->bad;
                if (!s.prevHit) ++ctr->hits;
            }
        }
        s.prevHit = hit;
        s.dirty = true;
    }
};

class $modify(NoclipPlay, PlayLayer) {
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        prepare(this, true);
    }

    void resetLevel() {
        auto& s = State::get();
        cfg().refresh();
        s.attempt.reset();
        s.hitThisStep = false;
        s.prevHit = false;
        s.notifiedHit = false;
        s.dirty = true;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        auto& s = State::get();
        s.label = nullptr;
        s.owner = nullptr;
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& s = State::get();
        auto& c = cfg();

        // El "pico anti-trampas" del juego siempre funciona normal.
        if ((object && object == m_anticheatSpike) || !c.enabled) {
            return PlayLayer::destroyPlayer(player, object);
        }
        // Solo los jugadores reales (por ejemplo, ignora jugadores "fantasma" de otros mods).
        bool isP1 = (player == m_player1);
        bool isP2 = (player == m_player2);
        if (!isP1 && !isP2) return PlayLayer::destroyPlayer(player, object);
        if ((isP1 && !c.p1) || (isP2 && !c.p2)) {
            return PlayLayer::destroyPlayer(player, object);
        }

        s.hitThisStep = true;  // anotamos el choque; se cuenta al cerrar el tick

        // Precision minima: si el intento baja de ella, mueres de verdad.
        if (c.minAccuracy > 0.0 && s.attempt.total >= c.grace && s.attempt.accuracy() < c.minAccuracy) {
            return PlayLayer::destroyPlayer(player, object);
        }
        // Si no: NO morimos (esto es el noclip).
        if (c.debugNotify && !s.notifiedHit) {
            s.notifiedHit = true;
            Notification::create("Noclip: golpe absorbido", NotificationIcon::Success, 1.5f)->show();
        }
    }

    void levelComplete() {
        auto& s = State::get();
        auto& c = cfg();
        if (c.enabled && c.showResult && s.usedNoclip) {
            std::string text = fmt::format(
                "Noclip: precision {:.{}f}% (nivel {:.{}f}%), golpes {}",
                s.attempt.accuracy(), c.decimals, s.level.accuracy(), c.decimals, s.attempt.hits
            );
            Notification::create(text.c_str(), NotificationIcon::Info, 5.f)->show();
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        auto& s = State::get();
        auto& c = cfg();
        if (!m_uiLayer) return;
        if (s.owner != this || !s.label) prepare(this, s.owner != this);  // red de seguridad

        s.labelTimer += dt;
        if (!s.dirty || s.labelTimer < 0.1f) return;  // como mucho 10 veces por segundo
        s.labelTimer = 0.f;
        s.dirty = false;

        bool show = c.enabled && c.showIndicator;
        s.label->setVisible(show);
        if (!show) return;

        Counter const& ctr = c.perAttempt ? s.attempt : s.level;
        double acc = ctr.accuracy();

        std::string text = fmt::format("Acc {:.{}f}%", acc, c.decimals);
        if (c.showHits) text += fmt::format("  hits {}", ctr.hits);

        s.label->setString(text.c_str());
        placeLabel(s.label, c.corner);
        s.label->setOpacity(static_cast<unsigned char>(c.opacity * 255 / 100));
        if (!c.colorByAcc) s.label->setColor(ccc3(255, 255, 255));
        else if (acc >= 95.0) s.label->setColor(ccc3(120, 255, 150));
        else if (acc >= 80.0) s.label->setColor(ccc3(255, 230, 110));
        else s.label->setColor(ccc3(255, 110, 110));
    }
};
