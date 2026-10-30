#include "../src/platform/windows/application_actors.hpp"
#include "../src/platform/windows/native_wake.hpp"
#include "view_creation_legacy.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace stormtty;
using namespace stormtty::windows;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Handle {
    HANDLE value{};

    ~Handle() {
        if (value) {
            CloseHandle(value);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit Handle(HANDLE owned) : value(owned) {
        require(value != nullptr, "Could not create authored event");
    }
};

void drain_wakes() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, wake_message, wake_message, PM_REMOVE)) {
        if (message.hwnd) {
            DispatchMessageW(&message);
        }
    }
}

struct MessageWindow {
    HWND value{};
    unsigned notifications{};

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                             reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        auto* self = reinterpret_cast<MessageWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == wake_message && self) {
            ++self->notifications;
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    MessageWindow() {
        WNDCLASSW definition{};
        definition.lpfnWndProc = procedure;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = L"StormTTY.HostLifetimeTest";
        require(RegisterClassW(&definition) != 0, "Could not register authored message window");
        value = CreateWindowExW(0, definition.lpszClassName, L"", 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                definition.hInstance, this);
        if (!value) {
            UnregisterClassW(definition.lpszClassName, definition.hInstance);
            throw std::runtime_error("Could not create authored message window");
        }
    }

    ~MessageWindow() {
        if (value) {
            DestroyWindow(value);
        }
        UnregisterClassW(L"StormTTY.HostLifetimeTest", GetModuleHandleW(nullptr));
    }

    void wait_for_notification(unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (notifications < expected) {
            drain_wakes();
            if (notifications >= expected) {
                return;
            }
            require(std::chrono::steady_clock::now() < deadline,
                    "Authored readiness notification did not arrive");
            const auto result = MsgWaitForMultipleObjectsEx(
                0, nullptr, 50, QS_POSTMESSAGE, MWMO_INPUTAVAILABLE);
            require(result != WAIT_FAILED, "Message wait failed");
        }
    }
};

void wake_routing_test() {
    MessageWindow window;
    auto completion = std::make_shared<WakeState>();
    completion->thread = GetCurrentThreadId();
    auto endpoint = std::make_shared<WakeState>();
    endpoint->window.store(window.value, std::memory_order_release);
    endpoint->thread = GetCurrentThreadId();
    endpoint->completion_fallback = completion;

    endpoint->wake();
    endpoint->wake();
    drain_wakes();
    require(window.notifications == 1 && endpoint->posted && !completion->posted,
            "HWND wake did not win precedence or coalesce");
    endpoint->posted.store(false, std::memory_order_release);
    endpoint->wake();
    drain_wakes();
    require(window.notifications == 2, "Acknowledged HWND wake did not rearm");

    endpoint->window.store(nullptr, std::memory_order_release);
    endpoint->posted = false;
    endpoint->wake();
    MSG message{};
    require(PeekMessageW(&message, nullptr, wake_message, wake_message, PM_REMOVE) &&
                message.hwnd == nullptr && endpoint->posted && !completion->posted,
            "Thread-only completion sink changed precedence");

    endpoint->thread = 0;
    endpoint->posted = false;
    endpoint->wake();
    require(completion->posted && !endpoint->posted,
            "Unbound endpoint did not use weak completion fallback");
    drain_wakes();
    completion.reset();
    endpoint->wake();
    require(!endpoint->posted, "Expired fallback retained completion ownership");

    // Invalid native target exercises real PostMessage failure without hooks.
    const auto destroyed = window.value;
    DestroyWindow(window.value);
    window.value = nullptr;
    endpoint->window.store(destroyed, std::memory_order_release);
    endpoint->wake();
    require(!endpoint->posted, "Failed native post left coalescing permanently set");
    endpoint->window.store(nullptr, std::memory_order_release);

    // Existing mailbox remains a separate synchronized payload, not a callback
    // into a window. Moving the endpoint must not discard either pending result.
    {
        std::lock_guard lock(endpoint->mutex);
        endpoint->clipboard = "authored selection";
        endpoint->error = "authored error";
    }
    std::optional<std::string> clipboard;
    std::optional<std::string> error;
    {
        std::lock_guard lock(endpoint->mutex);
        clipboard.swap(endpoint->clipboard);
        error.swap(endpoint->error);
    }
    require(clipboard == "authored selection" && error == "authored error" &&
                !endpoint->clipboard && !endpoint->error,
            "Native wake mailbox payload ownership changed");
}

void readiness_wait_test() {
    MessageWindow window;
    auto endpoint = std::make_shared<WakeState>();
    endpoint->window.store(window.value, std::memory_order_release);
    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));

    {
        RendererReadyWake ready(endpoint);
        ready.arm(event.value);
        ready.arm(event.value); // Same untriggered borrowed handle is a no-op.
        require(SetEvent(event.value), "Could not signal authored readiness");
        window.wait_for_notification(1);
        endpoint->posted = false;
        ready.arm(event.value); // Fired one-shot must rearm the same handle.
        require(SetEvent(event.value), "Could not signal second readiness");
        window.wait_for_notification(2);
        ready.cancel();
        ready.cancel();
        drain_wakes();

        endpoint->posted = false;
        ready.arm(event.value);
        ready.cancel();
        require(SetEvent(event.value), "Cancel closed its borrowed event");
        require(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0,
                "Canceled wait consumed a later event");
        drain_wakes();
        require(window.notifications == 2 && !endpoint->posted,
                "Canceled readiness still posted a notification");

        ready.arm(event.value);
        // Destruction must cancel this untriggered registration before the
        // caller closes its event, even though the endpoint still exists.
    }
    require(SetEvent(event.value), "Wait destructor closed the caller's event");
    require(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0,
            "Destroyed wait retained a registration");
    drain_wakes();
    require(window.notifications == 2, "Destroyed readiness wait posted again");
    endpoint->window.store(nullptr, std::memory_order_release);
}

void partial_registration_routes_test() {
    auto original = std::make_shared<WakeState>();
    auto completion = std::make_shared<WakeState>();
    original->thread = GetCurrentThreadId();
    completion->thread = GetCurrentThreadId();
    Session::Wake retained_callback;
    {
        ApplicationActors actors;
        const auto abandoned = actors.registry.allocate();
        const auto survivor = actors.registry.allocate();
        require(abandoned != survivor, "Application identities recycled");
        retained_callback = actors.registry.callback(abandoned, original);
        for (const auto id : {abandoned, survivor}) {
            actors.observations.observe_state(id, SessionState::starting);
            actors.observations.observe_background_label(id, "authored label");
            SessionInfo running;
            running.state = SessionState::running;
            running.command.phase = CommandPhase::running;
            running.command.command_generation = 1;
            actors.attention.baseline_after_park(id, running);
            require(actors.attention.arm_watch(id, {1, id.value, 1}, running, true),
                    "Could not arm authored attention");
        }
        actors.dispose(abandoned, completion);
        actors.dispose(abandoned);
        require(actors.registry.empty() && !actors.registry.identity(abandoned) &&
                    !actors.observations.state(abandoned) &&
                    !actors.observations.background_label(abandoned) &&
                    !actors.attention.parked(abandoned) &&
                    !actors.attention.stored_watch(abandoned),
                "Callback-only disposal leaked registration metadata");
        require(actors.observations.state(survivor) &&
                    actors.observations.background_label(survivor) &&
                    actors.attention.parked(survivor) &&
                    actors.attention.stored_watch(survivor),
                "Disposal removed unrelated actor metadata");
        retained_callback();
        require(completion->posted && !original->posted,
                "Retained callback missed completion rebind");
        drain_wakes();
    }
    completion->posted = false;
    original.reset();
    retained_callback();
    require(completion->posted, "Route died with registry or original endpoint");
    drain_wakes();
    std::weak_ptr<WakeState> weak = completion;
    completion.reset();
    require(weak.expired(), "Actor route retained its endpoint strongly");
    retained_callback(); // Expired route endpoint is harmless.
}

bool authored_actor_ready(const Session& actor) {
    const auto frame = actor.frame();
    if (!frame) {
        return false;
    }
    for (const auto& row : frame->lines) {
        if (!row) {
            continue;
        }
        std::string text;
        for (const auto& cell : row->cells) {
            text += cell.grapheme;
        }
        if (text.find("HOST_LIFETIME_READY") != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct ActorCleanup {
    std::shared_ptr<Session> actor;
    HANDLE release{};

    ~ActorCleanup() {
        SetEvent(release);
        if (actor) {
            actor->request_close();
            (void)actor->wait_closed(5s);
        }
    }
};

void registered_actor_test(const std::string& executable) {
    const auto event_name = L"Local\\StormTTY.HostLifetime." +
        std::to_wstring(GetCurrentProcessId());
    Handle release(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    ProcessOptions process;
    process.executable = executable;
    process.arguments = {"--authored-child", std::to_string(GetCurrentProcessId())};
    auto endpoint = std::make_shared<WakeState>();
    endpoint->thread = GetCurrentThreadId();
    std::shared_ptr<Session> retained;
    WatchIdentity first_identity;
    {
        ApplicationActors actors;
        const auto id = actors.registry.allocate();
        retained = Session::start(std::move(process), {}, actors.registry.callback(id, endpoint),
                                  SessionRetentionPolicy::retain_document);
        ActorCleanup cleanup{retained, release.value};
        actors.registry.register_actor(id, retained);
        first_identity = actors.registry.identity(id);
        require(bool(first_identity) && actors.registry.find(id) == retained.get() &&
                    actors.registry.retain(id) == retained && actors.registry.size() == 1,
                "Registered actor lookup/identity changed");
        const auto snapshot = actors.registry.snapshot();
        require(snapshot.size() == 1 && snapshot.front().identity == first_identity &&
                    snapshot.front().actor == retained,
                "Owned registry snapshot changed");
        const auto next = actors.registry.allocate();
        require(next != id, "Registered identity recycled");

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!authored_actor_ready(*retained) && !retained->process_finished()) {
            require(std::chrono::steady_clock::now() < deadline, "Authored actor did not start");
            std::this_thread::sleep_for(5ms);
        }
        require(authored_actor_ready(*retained) && !retained->process_finished(),
                "Authored actor did not reach its event wait");
        actors.dispose(id, endpoint);
        require(!actors.registry.contains(id) && !actors.registry.identity(id) &&
                    retained != nullptr && snapshot.front().actor == retained,
                "Disposal invalidated explicitly retained actor ownership");
        require(retained->wait_closed(5s), "Disposed authored actor did not close");
    }
    require(ResetEvent(release.value), "Could not reset authored child event");
    ActorCleanup destructor_cleanup{nullptr, release.value};
    {
        ActorLifetimeRegistry closing;
        const auto id = closing.allocate();
        ProcessOptions next_process;
        next_process.executable = executable;
        next_process.arguments = {"--authored-child", std::to_string(GetCurrentProcessId())};
        destructor_cleanup.actor = Session::start(
            std::move(next_process), {}, closing.callback(id, endpoint),
            SessionRetentionPolicy::retain_document);
        closing.register_actor(id, destructor_cleanup.actor);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!authored_actor_ready(*destructor_cleanup.actor) &&
               !destructor_cleanup.actor->process_finished()) {
            require(std::chrono::steady_clock::now() < deadline,
                    "Destructor fixture actor did not start");
            std::this_thread::sleep_for(5ms);
        }
        require(authored_actor_ready(*destructor_cleanup.actor) &&
                    !destructor_cleanup.actor->process_finished(),
                "Destructor fixture did not reach its event wait");
    }
    require(destructor_cleanup.actor->wait_closed(5s),
            "Registry destruction did not request retained actor closure");

    // A distinct application registry must never inherit another host identity.
    ActorLifetimeRegistry another;
    const auto next = another.allocate();
    another.register_actor(next, retained);
    const auto another_identity = another.identity(next);
    require(bool(another_identity) && another_identity.host != first_identity.host,
            "Registered actor reused another application's host identity");
    drain_wakes();
}

template<class Creation>
void view_creation_test(const std::string& executable) {
    ApplicationWorkspaces projects;
    ApplicationActors actors;
    Creation creation(projects, actors);
    const auto original = projects.create_window();
    require(bool(original), "Could not create survivor catalog window");
    const auto survivor_project = projects.create_workspace(*original.value, {"Survivor", "", ""});
    require(bool(survivor_project), "Could not create survivor workspace");
    const auto event_name = L"Local\\StormTTY.HostLifetime." +
        std::to_wstring(GetCurrentProcessId());
    Handle release(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
    ProcessOptions process;
    process.executable = executable;
    process.arguments = {"--authored-child", std::to_string(GetCurrentProcessId())};
    auto survivor_endpoint = std::make_shared<WakeState>();
    survivor_endpoint->thread = GetCurrentThreadId();
    const auto survivor_id = actors.registry.allocate();
    auto survivor = Session::start(process, {}, actors.registry.callback(survivor_id, survivor_endpoint),
                                   SessionRetentionPolicy::retain_document);
    ActorCleanup survivor_cleanup{survivor, release.value};
    actors.registry.register_actor(survivor_id, survivor);
    require(bool(projects.workspace(*survivor_project.value)->create_tab(survivor_id, "default")),
            "Could not bind survivor actor");
    const auto identity = actors.registry.identity(survivor_id);
    const auto candidate = creation.begin();
    require(creation.active(), "Creation guard not active");
    bool nested_rejected = false;
    try { (void)creation.begin(); } catch (const std::runtime_error&) { nested_rejected = true; }
    require(nested_rejected, "Reentrant creation accepted");
    const auto candidate_project = projects.create_workspace(candidate, {"Candidate", "", ""});
    require(bool(candidate_project), "Could not stage candidate workspace");
    auto candidate_window = std::make_unique<MessageWindow>();
    auto candidate_endpoint = std::make_shared<WakeState>();
    candidate_endpoint->window = candidate_window->value;
    const auto candidate_id = actors.registry.allocate();
    creation.record_actor(candidate, candidate_id); // Before launch, as in host.
    auto candidate_actor = Session::start(process, {}, actors.registry.callback(candidate_id, candidate_endpoint),
                                         SessionRetentionPolicy::retain_document);
    ActorCleanup candidate_cleanup{candidate_actor, release.value};
    actors.registry.register_actor(candidate_id, candidate_actor);
    require(bool(projects.workspace(*candidate_project.value)->create_tab(candidate_id, "default")),
            "Could not bind candidate actor");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!authored_actor_ready(*survivor) || !authored_actor_ready(*candidate_actor)) &&
           !survivor->process_finished() && !candidate_actor->process_finished()) {
        require(std::chrono::steady_clock::now() < deadline, "Creation fixture actors did not start");
        std::this_thread::sleep_for(5ms);
    }
    require(authored_actor_ready(*survivor) && authored_actor_ready(*candidate_actor) &&
                !survivor->process_finished() && !candidate_actor->process_finished(),
            "Creation fixture actors did not reach authored event wait");
    const auto unregistered = actors.registry.allocate();
    creation.record_actor(candidate, unregistered);
    auto retained_callback = actors.registry.callback(unregistered, candidate_endpoint);
    actors.observations.observe_state(unregistered, SessionState::starting);
    actors.observations.observe_state(survivor_id, SessionState::running);
    require(bool(projects.rename(*survivor_project.value, "Survivor edit during construction")),
            "Survivor edit was rejected");
    require(creation.defer_model_change(), "Reentrant edit was not deferred");
    // Exact host ordering: wake target cleared and native candidate destroyed
    // before catalog abort and actor disposal. No production injection hook.
    candidate_endpoint->window = nullptr;
    candidate_window.reset();
    require(creation.rollback_after_candidate_destruction(), "Rollback lost survivor dirty notification");
    require(!creation.active() && !creation.defer_model_change(), "Rollback left creation guard active");
    require(!projects.window(candidate) && projects.window(*original.value) && projects.validate(),
            "Rollback damaged catalog ownership");
    require(projects.catalog().find(*survivor_project.value)->metadata.name ==
                "Survivor edit during construction", "Rollback overwrote survivor edit");
    require(actors.registry.size() == 1 && actors.registry.retain(survivor_id) == survivor &&
                actors.registry.identity(survivor_id) == identity && !survivor->closed(),
            "Rollback disposed or replaced survivor actor");
    require(candidate_actor->wait_closed(5s), "Rollback did not dispose candidate actor");
    require(!actors.observations.state(unregistered) && actors.observations.state(survivor_id),
            "Exact ledger cleanup removed survivor or leaked unregistered metadata");
    auto replacement_endpoint = std::make_shared<WakeState>();
    replacement_endpoint->thread = GetCurrentThreadId();
    (void)actors.registry.callback(unregistered, replacement_endpoint);
    retained_callback();
    require(!replacement_endpoint->posted, "Aborted route remained discoverable");
    actors.dispose(unregistered);

    const auto published = creation.begin();
    require(published.value > candidate.value, "Candidate window identity was reused");
    require(bool(projects.create_workspace(published, {"Published", "", ""})), "Publish staging failed");
    require(creation.defer_model_change(), "Staged dirty change lost");
    creation.commit_catalog();
    require(creation.active() && creation.defer_model_change(),
            "Catalog commit ended publication guard early");
    require(creation.finish_publication() && !creation.active(), "Publication lost deferred notification");
    // A post-publication notification exception is not a creation failure.
    try { throw std::runtime_error("Authored postcommit notice failure"); }
    catch (const std::runtime_error&) {}
    require(projects.window(published) && actors.registry.retain(survivor_id) == survivor,
            "Postcommit notification entered rollback");
    require(!creation.defer_model_change(), "Finished publication retained dirty suppression");
    drain_wakes();
}

int authored_child(const char* identifier) {
    const auto name = L"Local\\StormTTY.HostLifetime." +
        std::to_wstring(std::stoul(identifier));
    Handle release(OpenEventW(SYNCHRONIZE, FALSE, name.c_str()));
    std::puts("HOST_LIFETIME_READY");
    std::fflush(stdout);
    return WaitForSingleObject(release.value, 15000) == WAIT_OBJECT_0 ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--authored-child") {
            return authored_child(argv[2]);
        }
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        wake_routing_test();
        readiness_wait_test();
        partial_registration_routes_test();
        registered_actor_test(std::filesystem::absolute(argv[0]).string());
        view_creation_test<stormtty::test::LegacyViewCreation>(std::filesystem::absolute(argv[0]).string());
        std::cout << "Windows host lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
