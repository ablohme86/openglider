/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-10-16
 *  File: TrainerBehavior.cpp
 *
 *  Desc:
 *  Train Abilities: to the class trainer after an even level, and the
 *  spells it lists learned.
 *
 */
#include "og/core/BotContext.h"
#include "og/core/behaviors/Behaviors.h"
#include <algorithm>
#include <format>
#include <utility>

namespace og
{

namespace
{
// Walking there, talking and training: as long as a quest hand-in has.
const Duration kTimeout{300000};
// After a trip that came to nothing, the next try.
const Duration kRetryAfter{600000};
constexpr int kTries = 3;
constexpr float kCloseIn = 1.5f;
// The trainer window fills its list a moment after it shows.
const Duration kSettle{1000};
// Between choosing a row and clicking Train, and the money going down after.
const Duration kRowPick{400};
const Duration kTrainAnswer{1500};
// A gossip line clicked and no trainer window.
const Duration kGossipAnswer{3000};
constexpr int kMaxTrainClicks = 40;

// Copper as the game shows it: "1g 2s 3c", "6s".
std::string money(std::int64_t copper)
{
    std::string out;
    const auto part = [&](std::int64_t amount, char unit)
    {
        if (amount > 0)
        {
            out += std::format("{}{}{}", out.empty() ? "" : " ", amount, unit);
        }
    };
    part(copper / 10000, 'g');
    part(copper / 100 % 100, 's');
    part(copper % 100, 'c');
    return out.empty() ? "0c" : out;
}

// A vendor trip asked for before training that has not begun by then: no
// vendor to go to (none on the route, none in the data) - not waited on.
const Duration kVendorStart{60000};

// What a vendor might buy: anything in the bags but the Hearthstone, a quest's
// items (by the name its progress shows, "Stolen Book 3/8") and what is never
// to be sold. The quality is the client's to know: a grey left to sell is
// taken for granted.
bool somethingToSell(const GameSnapshot& s, const BotContext& ctx)
{
    constexpr std::uint32_t kHearthstone = 6948;
    const auto& never = ctx.config.vendor.neverSell;

    for (const ItemStack& item : s.player.bagItems)
    {
        if (item.itemId == 0 || item.itemId == kHearthstone || item.count <= 0)
        {
            continue;
        }

        if (!item.name.empty())
        {
            if (std::find(never.begin(), never.end(), item.name) != never.end())
            {
                continue;
            }

            const bool forQuest = std::any_of(ctx.questTargets.begin(), ctx.questTargets.end(), [&](const QuestTarget& q) { return std::any_of(q.progress.begin(), q.progress.end(), [&](const std::string& line) { return line.starts_with(item.name + " "); }); });

            if (forQuest)
            {
                continue;
            }
        }

        return true;
    }

    return false;
}

// The lines of the trainer window's list, top first: the frames inside its
// inset as wide as a row and one row high. On 1.60.1 the window shows six,
// only what can be learned now (Maximillian Crowe, Goldshire).
std::vector<UiFrame> trainerRows(BotContext& ctx)
{
    std::vector<UiFrame> rows;
    const auto inset = ctx.provider->uiFrame("ClassTrainerFrameInset");

    if (!inset || !inset->visible)
    {
        return rows;
    }

    const float width = inset->right - inset->left;

    for (const UiFrame& f : ctx.provider->uiFramesWithText("ClassTrainerFrame", ""))
    {
        const float height = f.bottom - f.top;

        if (f.visible && f.left >= inset->left - 0.005f && f.right <= inset->right + 0.005f && f.top >= inset->top - 0.005f && f.bottom <= inset->bottom + 0.005f && f.right - f.left > width * 0.7f && height > 0.03f && height < 0.09f)
        {
            rows.push_back(f);
        }
    }

    std::sort(rows.begin(), rows.end(), [](const UiFrame& a, const UiFrame& b) { return a.top < b.top; });
    return rows;
}

} // namespace

bool TrainerBehavior::canRun(const GameSnapshot& s, BotContext& ctx)
{
    if (!ctx.config.trainAbilities)
    {
        // Switched on later: from the level then.
        levelSeen_ = 0;
        due_ = false;

        if (std::exchange(sellAsked_, false))
        {
            ctx.vendorBeforeTraining = false;
        }

        // Switched off on the way: the trip is over, not resumed later.
        if (trainer_)
        {
            trainer_.reset();
            ctx.objectives.pop("Train");
        }
        return false;
    }

    // An even level reached (2, 4, 6...): what the trainer has new comes then.
    // Not the one the session began at - trained or not, that is the player's.
    if (s.player.level > 0)
    {
        if (levelSeen_ > 0 && s.player.level > levelSeen_)
        {
            for (int level = levelSeen_ + 1; level <= s.player.level; ++level)
            {
                if (level % 2 == 0)
                {
                    due_ = true;
                    sellAsked_ = false;
                    failures_ = 0;
                    notBefore_ = TimePoint{};
                    ctx.log.info(logcat::Profile, "level {}: new spells at the {} trainer - going there once nothing else comes first", s.player.level, toString(s.player.playerClass));
                    break;
                }
            }
        }

        levelSeen_ = std::max(levelSeen_, s.player.level);
    }

    if (trainer_)
    {
        return !s.isDead();
    }

    if (!due_ || ctx.partyFollower() || s.isDead() || s.inCombat() || !s.attackers().empty() || ctx.now() < notBefore_)
    {
        return false;
    }

    if (!ctx.questSource || !ctx.provider)
    {
        return false;
    }

    // A vendor first, where the bags have anything to sell: the more money,
    // the more the trainer teaches. Then the trainer.
    if (!sellAsked_ && ctx.config.vendor.enabled && somethingToSell(s, ctx))
    {
        sellAsked_ = true;
        sellAskedAt_ = ctx.now();
        ctx.vendorBeforeTraining = true;
        ctx.log.info(logcat::Profile, "train abilities: selling at a vendor first, then to the trainer (level {})", s.player.level);
        return false;
    }

    // The trip asked for under way, or about to start.
    if (sellAsked_ && ctx.vendorBeforeTraining && (ctx.vendorTrip || ctx.now() - sellAskedAt_ < kVendorStart))
    {
        return false;
    }

    ctx.vendorBeforeTraining = false;

    // Not a copper to pay with: nothing to learn there - the next even level
    // then. Money not known (-1): gone to all the same.
    if (s.player.money == 0)
    {
        ctx.log.info(logcat::Profile, "train abilities: no money to train with at level {}{} - waiting for the next even level", s.player.level, sellAsked_ ? " after the vendor" : "");
        due_ = false;
        sellAsked_ = false;
        return false;
    }

    const auto trainers = ctx.questSource->classTrainers(s);

    if (trainers.empty())
    {
        ctx.log.warn(logcat::Profile, "train abilities: the data has no {} trainer on this continent - not training at level {}", toString(s.player.playerClass), s.player.level);
        due_ = false;
        return false;
    }

    return true;
}

void TrainerBehavior::onEnter(const GameSnapshot& s, BotContext& ctx)
{
    if (!trainer_)
    {
        const auto trainers = ctx.questSource ? ctx.questSource->classTrainers(s) : std::vector<TrainerSpot>{};

        if (trainers.empty())
        {
            return;
        }

        trainer_ = trainers.front();
        stage_ = Stage::Travelling;
        npc_ = 0;
        started_ = stageStarted_ = ctx.now();
        progressFrom_.reset();
        opens_ = rightClicks_ = 0;
        mouseTried_ = closeIn_ = false;
        row_ = 0;
        rowClicked_ = trainClicked_ = false;
        trainClicks_ = learned_ = 0;
        if (!tripFrom_)
        {
            tripFrom_ = s.player.position;
        }
        taxiTrip_.reset();
        taxiChecked_ = false;
        taxiKnown_ = 0;
        ctx.log.info(logcat::Profile, "train abilities: going to {} ({:.0f} yd)", trainer_->name, distance2D(s.player.position, trainer_->position));
        ctx.objectives.push(Objective{"Train", 0, trainer_->position});
    }
    else if (stage_ == Stage::Returning)
    {
        stageStarted_ = ctx.now();
    }
    else if (stage_ != Stage::Travelling)
    {
        // Back from a fight: the window closed in it; talked to again.
        stage_ = Stage::Travelling;
        stageStarted_ = ctx.now();
    }

    ctx.combat.defendOnly = true;
    ctx.combat.engaged = 0;
}

void TrainerBehavior::onExit(BotContext& ctx)
{
    ctx.navigator.stop();
    ctx.cancelInteraction();
    ctx.stopMouseSeek();
    ctx.restoreCamera();
    ctx.combat.defendOnly = false;
}

BehaviorStatus TrainerBehavior::finish(BotContext& ctx, const std::string& reason, bool failed)
{
    ctx.navigator.stop();
    ctx.movement.stop();
    ctx.stopMouseSeek();
    ctx.restoreCamera();
    ctx.objectives.pop("Train");
    ctx.combat.defendOnly = false;
    taxiTrip_.reset();
    taxiChecked_ = false;
    taxiKnown_ = 0;
    tripFrom_.reset();
    stage_ = Stage::Travelling;

    for (const auto* close : {"ClassTrainerFrameCloseButton", "GossipFrameCloseButton"})
    {
        if (ctx.uiFrameVisible(close))
        {
            ctx.clickUiFrame(close);
        }
    }

    const std::string who = trainer_ ? trainer_->name : std::string("the trainer");
    trainer_.reset();
    sellAsked_ = false;

    if (failed)
    {
        if (++failures_ >= kTries)
        {
            due_ = false;
            ctx.log.warn(logcat::Profile, "train abilities: could not train at {}: {} - given up until the next even level", who, reason);
        }
        else
        {
            notBefore_ = ctx.now() + kRetryAfter;
            ctx.log.warn(logcat::Profile, "train abilities: could not train at {}: {} - trying again in {:.0f} minutes", who, reason, toSeconds(kRetryAfter) / 60.0);
        }

        return BehaviorStatus::Failure;
    }

    due_ = false;
    ctx.log.info(logcat::Profile, "train abilities: {} at {}", reason, who);
    return BehaviorStatus::Success;
}

bool TrainerBehavior::clickAt(BotContext& ctx, const UiFrame& frame)
{
    const auto size = ctx.actions.windowSize();

    if (!size || !ctx.actions.mouseFree())
    {
        return false;
    }

    ctx.actions.mouseMove(static_cast<int>(frame.centerX() * static_cast<float>(size->first) + 0.5f), static_cast<int>(frame.centerY() * static_cast<float>(size->second) + 0.5f));
    ctx.actions.click(MouseButton::Left);
    return true;
}

BehaviorStatus TrainerBehavior::tick(const GameSnapshot& s, BotContext& ctx)
{
    if (!trainer_)
    {
        onEnter(s, ctx);
    }

    if (!trainer_)
    {
        due_ = false;
        return BehaviorStatus::Success;
    }

    const TimePoint now = ctx.now();

    // A cast under way (a heal on the way): not walked or talked out of.
    if (s.player.isCasting || ctx.actions.isCasting())
    {
        ctx.navigator.stop();
        return BehaviorStatus::Running;
    }

    // Getting on: 200 yards from where the last stretch began, the time
    // starts again, as on the way to a quest's NPC.
    if (!progressFrom_ || distance2D(s.player.position, *progressFrom_) >= 200.0f)
    {
        progressFrom_ = s.player.position;
        started_ = now;
    }

    if (now - started_ > kTimeout)
    {
        return finish(ctx, "it took too long", true);
    }

    // The trainer, once in sight.
    const UnitState* npc = nullptr;

    for (const auto& u : s.units)
    {
        if (!u.isPlayer && u.isAlive() && u.entry == trainer_->entry && (!npc || distance(s.player.position, u.position) < distance(s.player.position, npc->position)))
        {
            npc = &u;
        }
    }

    // The window up, however it came to be: trained from at once.
    if (stage_ != Stage::Training && ctx.uiFrameVisible("ClassTrainerFrame"))
    {
        stage_ = Stage::Training;
        stageStarted_ = stepAt_ = now;
        row_ = 0;
        rowClicked_ = trainClicked_ = false;
        return BehaviorStatus::Running;
    }

    switch (stage_)
    {
    case Stage::Travelling:
    {
        if (taxiTrip_)
        {
            const auto status = taxiTrip_->tick(s, ctx);
            if (status == TaxiTrip::Status::Landed)
            {
                ctx.log.info(logcat::Profile, "train abilities: landed from flight - walking to {}", trainer_->name);
                taxiTrip_.reset();
                ctx.navigator.stop();
                ctx.movement.stop();
                started_ = now;
                progressFrom_ = s.player.position;
            }
            else if (status == TaxiTrip::Status::Failed)
            {
                ctx.log.warn(logcat::Profile, "train abilities: flight failed ({}) - continuing on foot", taxiTrip_->why());
                taxiTrip_.reset();
                ctx.navigator.stop();
                ctx.movement.stop();
                started_ = now;
                progressFrom_ = s.player.position;
            }
            else
            {
                started_ = now;
                progressFrom_ = s.player.position;
                return BehaviorStatus::Running;
            }
        }

        if (npc)
        {
            const float reach = closeIn_ ? kCloseIn : ctx.humanReach(ctx.config.interactDistance, npc->guid);

            if (distance(s.player.position, npc->position) <= reach)
            {
                ctx.navigator.stop();
                ctx.movement.stop();
                npc_ = npc->guid;
                mouseTried_ = false;
                stage_ = Stage::Opening;
                stageStarted_ = now;
                return BehaviorStatus::Running;
            }

            const NavStatus status = ctx.navigator.moveTo(s, npc->position, reach - 0.5f, true);

            if (status == NavStatus::Unreachable)
            {
                return finish(ctx, "the trainer could not be reached", true);
            }

            if (closeIn_ && status == NavStatus::Arrived && distance(s.player.position, npc->position) <= ctx.config.interactDistance)
            {
                ctx.navigator.stop();
                ctx.movement.stop();
                npc_ = npc->guid;
                mouseTried_ = false;
                stage_ = Stage::Opening;
                stageStarted_ = now;
            }

            return BehaviorStatus::Running;
        }

        const auto known = s.player.knownTaxiNodes.value_or(std::vector<std::uint32_t>{});

        if (!taxiTrip_ && (!taxiChecked_ || known.size() != taxiKnown_))
        {
            taxiChecked_ = true;
            taxiKnown_ = known.size();

            if (ctx.config.useFlightPaths && ctx.network() && s.mapId && known.size() >= 2 && distance2D(s.player.position, trainer_->position) > 400.0f)
            {
                if (const auto plan = ctx.network()->plan(*s.mapId, s.player.position, trainer_->position, known, s.player.money))
                {
                    ctx.log.info(logcat::Profile, "train abilities: flying to {} (saves {:.0f}s)", trainer_->name, plan->walkSeconds - plan->seconds);
                    taxiTrip_.emplace(*plan, *s.mapId);
                    return tick(s, ctx);
                }
            }
        }

        const NavStatus status = ctx.navigator.moveTo(s, trainer_->position, 3.0f, true);

        if (status == NavStatus::Unreachable)
        {
            return finish(ctx, "where the trainer stands could not be reached", true);
        }

        if (status == NavStatus::Arrived && distance2D(s.player.position, trainer_->position) < 5.0f && now - stageStarted_ > Duration(15000))
        {
            return finish(ctx, trainer_->name + " is not where the data has them", true);
        }

        return BehaviorStatus::Running;
    }

    case Stage::Opening:
    {
        if (!npc || npc->guid != npc_)
        {
            stage_ = Stage::Travelling;
            stageStarted_ = now;
            return BehaviorStatus::Running;
        }

        if (distance(s.player.position, npc->position) > (closeIn_ ? kCloseIn : ctx.humanReach(ctx.config.interactDistance, npc->guid)) + 0.5f)
        {
            stage_ = Stage::Travelling;
            stageStarted_ = now;
            return BehaviorStatus::Running;
        }

        // Talked to once from where it stands and nothing opened: right up
        // to the trainer, then again.
        if (opens_ >= 1 && !closeIn_ && distance(s.player.position, npc->position) > kCloseIn + 0.5f)
        {
            closeIn_ = true;
            stage_ = Stage::Travelling;
            stageStarted_ = now;
            return BehaviorStatus::Running;
        }

        if (opens_ >= 4)
        {
            return finish(ctx, "talking to " + trainer_->name + " opened nothing", true);
        }

        if (!ctx.turningRound(npc_) && !ctx.movement.face(s.player, npc->position, 0.4f))
        {
            return BehaviorStatus::Running;
        }

        if (!ctx.provider->supportsLua())
        {
            // No Lua (1.60.1): right-click the trainer under the mouse, as a
            // quest's NPC is; where the mouse finds nothing, the interact key.
            if (!mouseTried_ && ctx.zoomInForClick(rightClicks_))
            {
                return BehaviorStatus::Running;
            }

            const auto seek = mouseTried_ ? BotContext::MouseSeek::Unsupported : ctx.seekNpcTurning(s.player, npc_, npc->position);

            if (seek == BotContext::MouseSeek::Searching)
            {
                return BehaviorStatus::Running;
            }

            if (seek == BotContext::MouseSeek::Found)
            {
                ctx.actions.click(MouseButton::Right);
                ctx.stopMouseSeek();
                ++rightClicks_;
            }
            else
            {
                if (seek == BotContext::MouseSeek::NotFound)
                {
                    ctx.stopMouseSeek();
                }

                mouseTried_ = true;

                if (ctx.actions.execute(actions::Interact) != ActionResult::Executed)
                {
                    return BehaviorStatus::Running;
                }
            }
        }
        else
        {
            ctx.interactNpc(npc_, npc->position);
        }

        ++opens_;
        ctx.log.info(logcat::Profile, "train abilities: talking to {}", trainer_->name);
        stage_ = Stage::Gossip;
        stageStarted_ = now;
        return BehaviorStatus::Running;
    }

    case Stage::Gossip:
    {
        if (ctx.uiFrameVisible("GossipFrame"))
        {
            if (now - stepAt_ < kGossipAnswer && stepAt_ > stageStarted_)
            {
                return BehaviorStatus::Running;
            }

            // The line to train by, by what it says: "I am interested in
            // warlock training." The greeting above it says "training" too
            // (Maximillian Crowe's "Keep up on your training"): a line of
            // text, not a paragraph.
            for (std::string_view phrase : {"training", "train", "teach", "ausbild", "lehr", "entraîn", "formation", "entren", "trein", "обуч", "трениров", "수련", "훈련", "訓練", "训练"})
            {
                for (const UiFrame& line : ctx.provider->uiFramesWithText("GossipFrame", phrase))
                {
                    if (line.visible && line.bottom - line.top < 0.03f && clickAt(ctx, line))
                    {
                        ctx.log.info(logcat::Profile, "train abilities: chose the trainer's line ('{}')", phrase);
                        stepAt_ = now;
                        return BehaviorStatus::Running;
                    }
                }
            }

            return finish(ctx, trainer_->name + " has no line to train by", true);
        }

        // Nothing came up: talked to again.
        if (now - stageStarted_ > kGossipAnswer)
        {
            stage_ = Stage::Opening;
            stageStarted_ = now;
        }

        return BehaviorStatus::Running;
    }

    case Stage::Training:
    {
        const auto planReturnFlight = [&]() -> bool
        {
            const auto known = s.player.knownTaxiNodes.value_or(std::vector<std::uint32_t>{});
            if (tripFrom_ && ctx.config.useFlightPaths && ctx.network() && s.mapId && known.size() >= 2 && distance2D(s.player.position, *tripFrom_) > 400.0f)
            {
                if (const auto plan = ctx.network()->plan(*s.mapId, s.player.position, *tripFrom_, known, s.player.money))
                {
                    ctx.log.info(logcat::Profile, "train abilities: flying back to where training began (saves {:.0f}s)", plan->walkSeconds - plan->seconds);
                    stage_ = Stage::Returning;
                    stageStarted_ = stepAt_ = now;
                    taxiTrip_.emplace(*plan, *s.mapId);
                    for (const auto* close : {"ClassTrainerFrameCloseButton", "GossipFrameCloseButton"})
                    {
                        if (ctx.uiFrameVisible(close))
                        {
                            ctx.clickUiFrame(close);
                        }
                    }
                    return true;
                }
            }
            return false;
        };

        if (!ctx.uiFrameVisible("ClassTrainerFrame"))
        {
            if (learned_ > 0)
            {
                if (planReturnFlight())
                {
                    return BehaviorStatus::Running;
                }
                return finish(ctx, std::format("learned {} spell{}", learned_, learned_ == 1 ? "" : "s"), false);
            }
            return finish(ctx, "the trainer window closed", true);
        }

        if (now - stepAt_ < kSettle && !rowClicked_ && !trainClicked_ && row_ == 0 && learned_ == 0)
        {
            return BehaviorStatus::Running;
        }

        // A Train click waiting on its answer: the money gone down is a
        // spell learned, and the list starts again from the top (the next
        // rank stands in it now). Not gone down: too dear, the next row.
        if (trainClicked_)
        {
            const bool paid = moneyBefore_ >= 0 && s.player.money >= 0 && s.player.money < moneyBefore_;

            if (!paid && now - stepAt_ < kTrainAnswer)
            {
                return BehaviorStatus::Running;
            }

            trainClicked_ = rowClicked_ = false;

            if (paid)
            {
                ++learned_;
                ctx.log.info(logcat::Profile, "train abilities: learned a spell for {}", money(moneyBefore_ - s.player.money));
                row_ = 0;
            }
            else
            {
                ++row_;
            }

            stepAt_ = now;
            return BehaviorStatus::Running;
        }

        const auto rows = trainerRows(ctx);

        if (row_ >= rows.size() || trainClicks_ >= kMaxTrainClicks)
        {
            if (learned_ == 0)
            {
                ctx.log.info(logcat::Profile, "train abilities: nothing {} - {}", rows.empty() ? "to learn now" : "the money allows", trainer_->name);
            }

            if (planReturnFlight())
            {
                return BehaviorStatus::Running;
            }

            return finish(ctx, std::format("learned {} spell{}", learned_, learned_ == 1 ? "" : "s"), false);
        }

        if (now - stepAt_ < kRowPick)
        {
            return BehaviorStatus::Running;
        }

        // The row first: Train does nothing with none chosen.
        if (!rowClicked_)
        {
            if (clickAt(ctx, rows[row_]))
            {
                rowClicked_ = true;
                stepAt_ = now;
            }

            return BehaviorStatus::Running;
        }

        if (ctx.uiFrameVisible("ClassTrainerTrainButton") && ctx.clickUiFrame("ClassTrainerTrainButton"))
        {
            moneyBefore_ = s.player.money;
            trainClicked_ = true;
            ++trainClicks_;
            stepAt_ = now;
        }

        return BehaviorStatus::Running;
    }

    case Stage::Returning:
    {
        if (taxiTrip_)
        {
            const auto status = taxiTrip_->tick(s, ctx);
            if (status == TaxiTrip::Status::Landed)
            {
                ctx.log.info(logcat::Profile, "train abilities: landed back from training flight");
                taxiTrip_.reset();
                ctx.navigator.stop();
                ctx.movement.stop();
                return finish(ctx, std::format("learned {} spell{}", learned_, learned_ == 1 ? "" : "s"), false);
            }
            if (status == TaxiTrip::Status::Failed)
            {
                ctx.log.warn(logcat::Profile, "train abilities: return flight failed ({}) - walking back", taxiTrip_->why());
                taxiTrip_.reset();
                ctx.navigator.stop();
                ctx.movement.stop();
                return finish(ctx, std::format("learned {} spell{}", learned_, learned_ == 1 ? "" : "s"), false);
            }

            started_ = now;
            progressFrom_ = s.player.position;
            return BehaviorStatus::Running;
        }

        return finish(ctx, std::format("learned {} spell{}", learned_, learned_ == 1 ? "" : "s"), false);
    }
    }

    return BehaviorStatus::Running;
}

} // namespace og
