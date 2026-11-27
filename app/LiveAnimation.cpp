#include "LiveAnimation.hpp"

#include <lunar/Classes.hpp>
#include <lunar/net/Replication.hpp>
#include <lunar/script/PlaySession.hpp>

#include <format>
#include <utility>

LiveAnimation::LiveAnimation(std::shared_ptr<lunar::net::Replica> replica)
    : replica_(std::move(replica)), assets_(std::make_shared<lunar::assets::AssetCache>()), system_(assets_)
{
}

void LiveAnimation::attach(lunar::ClientSession& client)
{
    client.setAnimationSystem(&system_);
    lunar::FrameHooks hooks = client.hooks();
    hooks.animation = [this](double dt) { step(dt); };
    client.setHooks(std::move(hooks));
}

// Animator.OnCombinedUpdate events the replica took in since last time.
// Their Animators are looked up now, so updates that came before their
// character (in the join) still land. On our own character the server
// plays tracks too (the game's moves: 1667 updates in a 40 s session), which
// play here like anyone's; an update for a track our scripts are playing is
// the server relaying ours, and is left to them.
void LiveAnimation::receive()
{
    const lunar::net::Schema* schema = replica_->schema();
    const auto& events = replica_->events;
    if (!schema) {
        received_ = events.size();
        return;
    }
    const auto* players = dynamic_cast<const lunar::Players*>(replica_->game()->findFirstChildOfClass("Players"));
    const lunar::Player* local = players ? players->localPlayer() : nullptr;
    const lunar::Model* ours = local ? local->character() : nullptr;
    // Ones that went before they were read (more than the log keeps came at once) are missed.
    received_ = std::max(received_, events.first());
    for (; received_ < events.size(); ++received_) {
        const lunar::net::EventInvocationItem& event = events[received_];
        if (event.eventId >= schema->events.size() || schema->events[event.eventId].name != "OnCombinedUpdate")
            continue;
        lunar::InstancePtr animator = replica_->find(event.id);
        if (!animator || !animator->isA("Animator"))
            continue;
        auto update = lunar::avatar::TrackUpdate::fromArguments(event.arguments);
        if (!update)
            continue;
        if (ours && animator->isDescendantOf(ours) && replicated_.playsLocally(*animator, update->animationId))
            continue;
        replicated_.apply(*animator, *update);
    }
}

void LiveAnimation::step(double dt)
{
    replica_->stepMotion(clock_ ? clock_() : lunar::net::Replica::clockSeconds());
    receive();
    if (waitForClips_)
        system_.clips().wait();
    system_.step(static_cast<float>(dt));
}

std::vector<std::string> LiveAnimation::describe()
{
    std::vector<std::string> lines;
    const lunar::Instance* workspace = replica_->game()->findFirstChildOfClass("Workspace");
    if (!workspace)
        return lines;
    for (const lunar::InstancePtr& child : workspace->descendants()) {
        auto* model = dynamic_cast<lunar::Model*>(child.get());
        lunar::avatar::CharacterAnimation* entry = model ? system_.find(*model) : nullptr;
        if (!entry)
            continue;
        std::string line = model->name() + ":";
        for (const lunar::avatar::TrackPtr& track : entry->animator->playingTracks())
            if (track->weightCurrent() >= 0.01f)
                line += std::format(" {} {:.2f}{}", track->name(), track->weightCurrent(),
                                    track->clip() ? "" : " (no clip)");
        lines.push_back(std::move(line));
    }
    return lines;
}
