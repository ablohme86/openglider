/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: WowMemoryStateProvider.h
 *
 *  Desc:
 *  Declarations and interface for WowMemoryStateProvider (WoW client
 *  memory reading, process inspection, and layout definitions).
 *
 *
 */
#pragma once

#include "og/actions/ActionBinding.h"
#include "og/common/Clock.h"
#include "og/common/Log.h"
#include "og/state/IGameStateProvider.h"
#include "og/wowclient/ClientLayout.h"
#include "og/wowclient/ClientVersion.h"
#include "og/wowclient/FactionTemplate.h"
#include "og/wowclient/LuaExecutor.h"
#include "og/wowclient/ModernClientReader.h"
#include "og/wowclient/ProcessMemory.h"
#include "og/wowclient/SpellNames.h"
#include "og/wowclient/SpellTooltip.h"
#include "og/wowclient/WowBindings.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <deque>
#include <unordered_set>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace og
{
class ActionMap;
}

namespace og::wowclient
{

struct WowProviderConfig
{
    int pid = 0;             // 0 = find automatically
    std::string processName; // matched against /proc/<pid>/comm; empty: WoW.exe, WoWClassic.exe or WowB.exe
    // "auto": pick the layout from WoW.exe's version resource when attaching.
    // Otherwise a layout name ("3.3.5a", "1.12.1", "2.5.6") or build number ("5875").
    std::string client = "auto";
    // Ignore objects farther away. The server sends herbs and ore out to about
    // 215 yd (1.60.1, Elwynn): the game's minimap shows them that far, so the
    // radar should too. What the bot goes for is limited by the pull distance.
    float visibilityRange = 250.0f;
    // The client's install directory (the one holding WTF), where a client
    // without Lua has its key bindings. Empty: the client's working directory.
    std::string installDirectory;
    Duration reattachInterval{2000};
    // The 64-bit clients (2.5.6 and whatever follows) need a registered copy.
    // The desktop UI turns this off until the license server vouches for the key.
    bool allowModernClients = true;
};

// A running WoW client, as far as it can be told without a layout for it.
struct RunningClient
{
    int pid = 0;
    std::uint32_t build = 0;
    std::string version; // "2.5.6.69795"
    bool modern = false; // a 64-bit client
};

// One filled action bar slot.
struct ActionBarSlot
{
    int slot = 0;                // 1-120
    bool item = false;           // otherwise a spell
    std::uint32_t id = 0;        // spell or item id
    std::string name;            // the client's (possibly localized) name; empty when unknown
    std::optional<KeyChord> key; // the game's key binding for the button showing the slot
    Duration castTime{0};        // from the client, when it reported one
    ActionRange range{};
};

// Builds GameSnapshots by reading a WoW client's object manager. The client
// build decides the memory layout (see ClientLayout); everything else is
// shared. Target selection and interaction write to memory, and only on
// clients whose layout has the addresses for it. A read-only layout (2.5.6)
// is never written to.
class WowMemoryStateProvider final : public IGameStateProvider
{
public:
    WowMemoryStateProvider(WowProviderConfig config, Logger& log, const IClock& clock);
    // Reads from `memory` instead of a real process (tests).
    WowMemoryStateProvider(WowProviderConfig config, Logger& log, const IClock& clock, std::unique_ptr<IProcessMemory> memory);

    std::string name() const override
    {
        return "wow " + std::string(layout_->name);
    }

    std::optional<GameSnapshot> capture() override;
    bool setTarget(Guid guid) override;
    // The 64-bit clients' Tab reaches about 40 yards (targetNearestDistance).
    float tabTargetReach() const override
    {
        return modern_ ? 40.0f : 20.0f;
    }
    std::optional<ScreenPoint> worldToScreen(const Vec3& worldPos) override;
    // The 64-bit clients whose layout knows the UI; the others use Lua instead.
    std::optional<UiFrame> uiFrame(std::string_view name) override;
    bool supportsUi() const override;
    bool supportsUiBagSlots() const override;
    std::vector<UiBagSlot> uiBagSlots() override;
    std::optional<UiItemTooltip> uiItemTooltip() override;
    std::vector<UiItemTooltip> uiComparisonTooltips() override;
    std::optional<Guid> mouseoverGuid() override;
    bool targetsByName() const override
    {
        return layout_->readOnly && layout_->modern;
    }

    std::optional<std::string> uiText(std::string_view frame, std::string_view fontString) override;
    std::optional<UiFrame> uiRegion(std::string_view frame, std::string_view region) override;
    std::optional<ActionCooldown> actionCooldown(int slot) override;
    std::optional<UiFrame> uiFrameWithText(std::string_view under, std::string_view text) override;
    std::vector<UiFrame> uiFramesWithText(std::string_view under, std::string_view text) override;
    std::optional<Camera> camera() const override
    {
        return readCamera();
    }

    std::optional<Camera> readCamera() const;
    bool interactUnit(Guid guid, const Vec3& position) override;
    bool skinUnit(Guid guid) override;
    bool interactNpc(Guid guid, const Vec3& position) override;
    void cancelInteraction() override;
    bool supportsObjectInteraction() const override;
    bool interactObject(Guid guid, const Vec3& position) override;
    void cancelObjectInteraction() override;
    bool supportsLua() const override;
    bool executeLua(std::string_view script) override;
    bool luaIdle() const override;
    bool vanillaLua() const override
    {
        return layout_->vanillaLuaApi;
    }

    bool attach();
    // The client attach() would pick, read without choosing a layout: so the
    // license server can be asked for that build's offsets before attaching.
    // Read-only, and leaves the provider detached. nullopt: no client running.
    std::optional<RunningClient> peekClient();
    bool attached() const
    {
        return memory_->attached();
    }

    // Process id of the attached client, or 0. Used to locate its install.
    int clientPid() const
    {
        return memory_->attached() ? memory_->pid() : 0;
    }

    bool isInGame() const;
    SpellNames& spellNames()
    {
        return spellNames_;
    }

    const ClientLayout& layout() const
    {
        return *layout_;
    }

    // "WoW 1.12.1 (build 5875)" after attaching; empty before. Thread-safe.
    std::string clientDescription() const;
    // Why the last attach failed because of the client itself (e.g. an
    // unsupported build); empty when WoW was simply not found. Thread-safe.
    std::string attachProblem() const;
    // The last attach stopped at a 64-bit client because allowModernClients is off.
    bool licenseRequired() const
    {
        return licenseRequired_;
    }

    std::vector<std::uint32_t> readKnownSpells() const;
    std::vector<std::uint32_t> readActionBarSlots() const;
    // Every filled action bar slot. With Lua this is all bars, with the game's
    // own names, key bindings and cast times, and spellNames() learns the whole
    // spellbook; without it, Bar 1 on its default keys. `refresh` asks the client
    // to scan and waits briefly for it (the bot does this when it starts);
    // otherwise the last scan is read as it is.
    std::vector<ActionBarSlot> readActionBars(bool refresh);
    // Whether Lua runs untainted, so UseAction() can press a slot with no key.
    bool luaCanUseActions() const
    {
        return luaCanUseActions_;
    }

    void autoDetectActionBindings(ActionMap& actionMap, bool refresh = true);
    // A client that tells no spell names (the read-only 64-bit ones): a spell
    // on the bars that the built-in table does not know is nameless, and no
    // rule can use it. Its button's tooltip says what it is: `hover` puts the
    // mouse on a frame (false: it could not), and the names read are kept,
    // the bars detected again with them. Takes about half a second a spell,
    // so it is done once when attaching. Returns how many names were learned.
    int learnSpellNamesFromTooltips(ActionMap& actionMap, const std::function<bool(const UiFrame&)>& hover);
    // The same client tells no cast times either, and actions.yaml's are the
    // untalented ones: Corruption, taken for instant, was pressed on the move
    // and did not go off. Each spell's button is hovered and its tooltip's
    // cast time and range taken over what actions.yaml says, the bars detected
    // again with them. Read afresh every time, as talents change them. Returns
    // how many spells were read.
    int readSpellDataFromTooltips(ActionMap& actionMap, const std::function<bool(const UiFrame&)>& hover);
    // Where names learned from tooltips are kept from one session to the
    // next, so each spell is looked at only once; read in at once.
    void setLearnedSpellsFile(std::filesystem::path file);
    // Whether the last detection gave slots keys from the BarKeys
    // addon, which the bot then has to claim (see bindingsaddon). Thread-safe.
    std::vector<ChatMessage> takeChat() override
    {
        return std::exchange(chatIn_, {});
    }

    bool bindingsAddonUsable() const
    {
        return bindingsAddonUsable_;
    }

    bool hasClientInteractBinding() const
    {
        return modernInteractKey_.has_value();
    }

    // Human readable dump of what the provider currently sees (for a probe).
    std::string describe();

    // For a fishing probe, which looks for object fields the layout
    // does not have yet. Both only read.
    struct RawObjectInfo
    {
        std::uint64_t base = 0;
        std::vector<std::uint8_t> guid; // as the client stores it: 8 bytes, or 16 on the 64-bit clients
    };
    std::optional<RawObjectInfo> rawObject(Guid guid) const;
    // Empty when the memory cannot be read.
    std::vector<std::uint8_t> readRaw(std::uint64_t address, std::size_t size) const;

    // The 64-bit client's reader, for probing; null on the older clients.
    const ModernClientReader* modernReader() const
    {
        return modern_.get();
    }

private:
    void ensureLootHook();
    bool clickToMove(Guid guid, const Vec3& position, std::uint32_t action);
    Guid interactingObject_ = 0; // Only cancel CTM commands issued by gathering.
    bool lootHookInstalled_ = false;
    struct RawObject
    {
        std::uint32_t base = 0;
        std::uint32_t type = 0;
        Guid guid = 0;
        std::uint32_t descriptors = 0;
    };

    std::vector<ProcessMemory::ProcessInfo> findClientProcesses() const;
    // Picks the layout for the attached process; false if the build is not supported.
    bool selectLayout();
    std::optional<GameSnapshot> captureModern(TimePoint now);
    // Out of the world with another client running: attach to that one if it is
    // in the world. True when it switched. At most every reattachInterval.
    bool switchToClientInWorld(TimePoint now);
    // Fills s.fishing from the bobbers among s.objects.
    static void summarizeFishing(GameSnapshot& s, bool biteKnown);
    // The 64-bit client's action bars, with the keys its WTF bindings give them.
    std::vector<ActionBarSlot> readModernActionBars();
    std::filesystem::path installDirectory() const;

public:
    // The client's language, from its WTF/Config.wtf (textLocale): enUS where
    // it cannot be read.
    std::string gameLocale() const;

private:
    std::uint32_t objectManager() const;
    std::vector<RawObject> enumerateObjects(Guid& localGuid) const;
    bool readUnit(const RawObject& obj, UnitState& unit) const;
    void readPlayerExtras(const RawObject& obj, PlayerState& player) const;
    // Backpack and bag contents, plus the number of free slots.
    void readBags(const std::vector<RawObject>& objects, const RawObject& self, PlayerState& player) const;
    std::optional<Vec3> readCorpseObject(const std::vector<RawObject>& objects, Guid owner) const;
    bool readGameObject(const RawObject& obj, GameObjectState& go) const;
    // Installs the render-thread stub on first use; Lua and client function calls share it.
    bool ensureExecutor();
    // Queues the client's right-click on the unit with this GUID.
    bool rightClickUnit(Guid guid, bool autoLoot);
    void readAuras(const RawObject& obj, const std::uint8_t* descriptors, std::vector<Aura>& auras) const;
    std::string unitName(std::uint32_t base) const;
    // Logs chat lines added since the last call (category CHAT).
    void readChat();
    void ensureChatHook();
    // Counts the client's refused spell presses into s.spellNotReadyErrors and
    // s.spellOutOfReachErrors.
    void readSpellErrors(GameSnapshot& s);
    void ensureSpellErrorHook();
    std::uint32_t findCVar(std::string_view name) const;
    // A CVar's value, re-resolving `address` when it no longer names `name`.
    std::optional<std::string> readCVar(std::string_view name, std::uint32_t& address) const;
    void setAttachProblem(std::string problem);

    const FactionTemplate* getFactionTemplate(std::uint32_t templateId) const;
    Reaction calculateReaction(std::uint32_t playerTemplateId, std::uint32_t targetTemplateId) const;

    WowProviderConfig config_;
    Logger& log_;
    const IClock& clock_;
    std::unique_ptr<IProcessMemory> memory_;
    const ClientLayout* layout_;
    std::uint64_t imageBase_ = consts::ImageBase;
    // Reads the snapshots while a 64-bit client is attached.
    std::unique_ptr<ModernClientReader> modern_;
    // Created on the first Lua call, so attaching never touches the client.
    std::unique_ptr<LuaExecutor> lua_;
    SpellNames spellNames_;
    TimePoint lastAttachAttempt_{};
    bool loggedNotInGame_ = false;
    std::optional<Vec3> lastPlayerPosition_;
    std::optional<TimePoint> lastPlayerPositionAt_;
    mutable std::unordered_map<Guid, std::string> nameCache_;
    mutable std::unordered_map<std::uint32_t, FactionTemplate> factionTemplateCache_;
    std::vector<std::string> chatEntries_; // last seen text per chat slot
    std::optional<std::int32_t> chatPosition_;
    bool chatHookInstalled_ = false;
    mutable std::uint32_t chatCVarAddress_ = 0;
    int lastChatSeq_ = 0;
    std::string lastRawChatLog_;
    std::vector<ChatMessage> chatIn_; // the lines received, for takeChat
    // The 64-bit clients: the chat window's lines, read once a second, and
    // those seen before (a line not among them is new).
    void readChatFrame(TimePoint now);
    std::optional<TimePoint> chatFrameReadAt_;
    std::unordered_set<std::string> chatSeen_; // the chat lines seen, the last few thousand
    std::deque<std::string> chatSeenOrder_;    // the same, oldest first
    std::map<std::string, int> objectiveWanted_; // the game's objective counts, by lower-case name
    std::map<std::string, std::vector<PlayerState::TrackedObjective>> trackedQuests_; // the quest tracker's lines, by title
    Guid objectiveWantedOf_ = 0;                 // the character they are of
    std::string zoneText_;                       // the minimap's zone text at the last read
    bool breathShown_ = false;                   // the breath bar at the last read
    bool spellErrorHookInstalled_ = false;
    std::uint32_t spellErrorCVarAddress_ = 0;
    std::optional<long> lastSpellErrorCount_;
    std::optional<long> lastOutOfReachCount_;
    std::optional<long> lastMovingCount_;
    // Queues the action bar and spellbook scan.
    bool requestActionBarScan();
    std::uint32_t actionBarSeqAddress_ = 0;
    std::array<std::uint32_t, 8> actionBarPageAddresses_{};
    std::array<std::uint32_t, 8> spellbookPageAddresses_{};
    std::string lastActionBarSeq_;
    bool luaCanUseActions_ = false;
    std::set<int> warnedKeylessSlots_;
    std::set<std::uint32_t> warnedNamelessSpells_;
    std::filesystem::path learnedSpellsFile_;
    std::map<std::uint32_t, std::string> learnedSpells_;
    std::set<std::uint32_t> tooltipsTried_; // nothing readable on it: not hovered again this session
    std::map<std::uint32_t, SpellTooltipData> tooltipSpellData_; // by spell id, from the last reading
    // The tooltip a frame shows once the mouse is on it; nullopt when none
    // came up. `moved` is false when the mouse could not be put there.
    std::optional<UiItemTooltip> tooltipOver(const UiFrame& frame, const std::function<bool(const UiFrame&)>& hover, bool& moved);
    bool warnedNoBindingFiles_ = false;
    std::atomic<bool> bindingsAddonUsable_{false};
    std::optional<KeyChord> modernInteractKey_; // Interact With Target, from WTF or the addon
    // The character's key bindings, as last read with the action bars: where
    // moving, jumping and Tab targeting sit.
    std::optional<WowBindings> clientBindings_;
    bool warnedNoInteractKey_ = false;
    bool addonReported_ = false;

    mutable std::mutex infoMutex_;
    std::string clientDescription_;
    std::string attachProblem_;
    std::atomic<bool> licenseRequired_{false};
};

} // namespace og::wowclient
