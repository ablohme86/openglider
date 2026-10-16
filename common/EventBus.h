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
 *  File: EventBus.h
 *
 *  Desc:
 *  Declarations and interface for EventBus (common utility functions,
 *  math, logging, and data structures).
 *
 *
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace og
{

// Synchronous, type-indexed publish/subscribe bus.
//
//   auto sub = events.subscribe<PlayerDiedEvent>([](const PlayerDiedEvent& e) { ... });
//   events.publish(PlayerDiedEvent{...});
//
// Subscriptions are RAII handles: dropping the handle unsubscribes. Handlers may
// safely subscribe/unsubscribe while an event is being dispatched.
class EventBus
{
    struct Registry;

public:
    class Subscription
    {
    public:
        Subscription() = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& o) noexcept
        {
            *this = std::move(o);
        }

        Subscription& operator=(Subscription&& o) noexcept
        {
            if (this != &o)
            {
                reset();
                registry_ = std::move(o.registry_);
                type_ = o.type_;
                id_ = std::exchange(o.id_, 0);
            }

            return *this;
        }

        ~Subscription()
        {
            reset();
        }

        void reset()
        {
            if (auto reg = registry_.lock(); reg && id_ != 0)
            {
                reg->remove(type_, id_);
            }

            registry_.reset();
            id_ = 0;
        }

        bool active() const
        {
            return id_ != 0 && !registry_.expired();
        }

    private:
        friend class EventBus;
        Subscription(std::weak_ptr<Registry> r, std::type_index t, std::uint64_t id) : registry_(std::move(r)), type_(t), id_(id)
        {
        }

        std::weak_ptr<Registry> registry_;
        std::type_index type_ = typeid(void);
        std::uint64_t id_ = 0;
    };

    EventBus() : registry_(std::make_shared<Registry>())
    {
    }

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <class Event> [[nodiscard]] Subscription subscribe(std::function<void(const Event&)> handler)
    {
        auto erased = [h = std::move(handler)](const void* e) { h(*static_cast<const Event*>(e)); };
        const auto id = registry_->add(typeid(Event), std::move(erased));
        return Subscription(registry_, typeid(Event), id);
    }

    template <class Event> void publish(const Event& event) const
    {
        registry_->dispatch(typeid(Event), &event);
    }

    template <class Event> std::size_t subscriberCount() const
    {
        return registry_->count(typeid(Event));
    }

private:
    struct Registry
    {
        using Handler = std::function<void(const void*)>;
        struct Entry
        {
            std::uint64_t id;
            std::shared_ptr<Handler> fn;
        };

        std::uint64_t add(std::type_index type, Handler fn)
        {
            std::lock_guard lock(mutex);
            const auto id = ++nextId;
            handlers[type].push_back({id, std::make_shared<Handler>(std::move(fn))});
            return id;
        }

        void remove(std::type_index type, std::uint64_t id)
        {
            std::lock_guard lock(mutex);
            auto it = handlers.find(type);

            if (it == handlers.end())
            {
                return;
            }

            std::erase_if(it->second, [id](const Entry& e) { return e.id == id; });
        }

        void dispatch(std::type_index type, const void* event)
        {
            std::vector<std::shared_ptr<Handler>> targets;
            {
                std::lock_guard lock(mutex);
                auto it = handlers.find(type);

                if (it == handlers.end())
                {
                    return;
                }

                targets.reserve(it->second.size());

                for (const auto& e : it->second)
                {
                    targets.push_back(e.fn);
                }
            }

            for (const auto& fn : targets)
            {
                (*fn)(event);
            }
        }

        std::size_t count(std::type_index type)
        {
            std::lock_guard lock(mutex);
            auto it = handlers.find(type);
            return it == handlers.end() ? 0 : it->second.size();
        }

        std::mutex mutex;
        std::uint64_t nextId = 0;
        std::unordered_map<std::type_index, std::vector<Entry>> handlers;
    };

    std::shared_ptr<Registry> registry_;
};

} // namespace og
