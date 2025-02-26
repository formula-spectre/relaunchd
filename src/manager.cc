/*
 * Copyright (c) 2015 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <fstream>
#include <iostream>
#include <unordered_map>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <queue>
#include <signal.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "channel.h"
#include "clock.h"
#include "event.h"
#include "job.h"
#include "log.h"
#include "manager.h"
#include "options.h"
#include "rpc_server.h"
#include "signal_names.h"
#include "state_file.hpp"

using json = nlohmann::json;

StateFile Manager::createOrOpenStatefile(const Domain &domain) {
    const auto &statedir = domain.statedir;

    if (getuid() != 0 && !std::filesystem::exists(statedir)) {
        log_debug("creating %s", statedir.c_str());
        std::filesystem::create_directories(statedir);
    }

    json defaultStateDoc = {{"SchemaVersion", 1},
                            {"Overrides", json::object()}};
    auto statefilepath = domain.statedir / "state.json";
    return StateFile(statefilepath, defaultStateDoc);
}

void Manager::setupSignalHandlers() {
    eventmgr.addSignal(SIGPIPE,
                       [](int) { log_debug("caught SIGPIPE and ignored it"); });

    eventmgr.addSignal(SIGINT, [this](int) { handleShutdownSignal("SIGINT"); });

    eventmgr.addSignal(SIGTERM,
                       [this](int) { handleShutdownSignal("SIGTERM"); });
}

bool Manager::loadManifest(const std::filesystem::path &path,
                           bool overrideDisabled, bool forceLoad) {
    json obj = manifest::parse(path);
    return loadManifest(obj, path, overrideDisabled, forceLoad);
}

bool Manager::loadManifest(const json &jsondata, const std::string &path,
                           bool overrideDisabled, bool forceLoad) {
    if (fsm.state() == States::GracefulShutdown) {
        log_error(
            "refusing to load a new job while the manager is shutting down");
        return false;
    }
    Manifest manifest;
    try {
        manifest = jsondata.get<manifest::Manifest>();
    } catch (const std::exception &exc) {
        log_error("failed to parse manifest at %s: %s", path.c_str(),
                  exc.what());
        return false;
    }
    const auto &label = static_cast<std::string>(manifest.label);

    /* Check for duplicate jobs */
    if (jobExists(manifest.label) ||
        pending_jobs.count(manifest.label.str()) > 0) {
        log_error("tried to load a duplicate job with label %s", label.c_str());
        return false;
    }

    if (overrideDisabled) {
        log_debug("%s: overriding the Disabled key", label.c_str());
        overrideJobEnabled(manifest.label, true);
    }

    // Check if the job is disabled via the manifest.
    if (manifest.disabled) {
        if (forceLoad) {
            log_notice("will forcibly load %s because forceLoad=true",
                       label.c_str());
        } else {
            log_notice("will not load %s: the manifest Disabled key is true",
                       label.c_str());
            return false;
        }
    }

    // Check if the job is disabled in the state file
    auto state = state_file.getValue();
    if (state.at("Overrides").contains(label)) {
        const auto job_state = state["Overrides"][label];
        if (!job_state.at("Enabled")) {
            if (forceLoad) {
                log_notice("will forcibly load %s even though it is disabled "
                           "in the state file",
                           label.c_str());
            } else {
                log_notice("will not load %s: it is explicitly disabled in the "
                           "state file",
                           label.c_str());
                return false;
            }
        }
    }

    log_notice("loaded job %s from %s", label.c_str(), path.c_str());
    auto jobp = std::make_unique<Job>(path, manifest, eventmgr, state_file);
    pending_jobs.emplace(jobp->manifest.label.str(), std::move(jobp));

    return true;
}

bool Manager::unloadJob(Job &job, bool overrideDisabled, bool forceUnload) {
    auto it = jobs.find(static_cast<std::string>(job.manifest.label));
    if (it != jobs.end()) {
        if (overrideDisabled) {
            log_debug("%s: overriding the Disabled key", job.getLabel());
            overrideJobEnabled(job.manifest.label, false);
        }
        return job.unloadJob(forceUnload);
    } else {
        log_info("tried to unload a job that is not loaded: %s",
                 job.manifest.label.c_str());
        return false;
    }
}

bool Manager::unloadJob(const Label &label, bool overrideDisabled,
                        bool forceUnload) {
    if (!jobExists(label)) {
        log_info("tried to unload a job that is not loaded: %s", label.c_str());
        return false;
    }
    auto &job = getJob(label);
    return unloadJob(job, overrideDisabled, forceUnload);
}

bool Manager::unloadJob(const std::filesystem::path &path,
                        bool overrideDisabled, bool forceUnload) {
    json obj;
    try {
        obj = manifest::parse(path);
    } catch (...) {
        log_error("error parsing %s", path.c_str());
        return false;
    }
    std::string buf;
    if (obj.contains("Label")) {
        obj.at("Label").get_to(buf);
        return unloadJob(Label{buf}, overrideDisabled, forceUnload);
    } else {
        log_error("manifest has no Label key");
        return false;
    }
}

json Manager::listJobs() {
    auto result = json::array();
    for (const auto &[label, jobp] : jobs) {
        auto &job = *jobp;
        std::string pid = (job.pid == 0) ? "-" : std::to_string(job.pid);
        if (job.pid == 0) {
            pid = "-";
        } else {
            pid = std::to_string(job.pid);
        }
        result.emplace_back(json::object({
            {"Label", std::string{job.manifest.label}},
            {"PID", std::move(pid)},
            {"LastExitStatus", job.last_exit_status},
        }));
    }
    return result;
}

void Manager::overrideJobEnabled(const Label &label_, bool enabled) {
    // FIXME: do we care if it exists?
    //  auto & job = manager_get_job_by_label(label);
    const auto &label = static_cast<std::string>(label_);
    auto doc = state_file.getValue();
    if (doc.at("Overrides").contains(label)) {
        doc["Overrides"][label]["Enabled"] = enabled;
    } else {
        doc["Overrides"].emplace(label, json::object({{"Enabled", enabled}}));
    }
    state_file.setValue(doc);
    log_notice("job %s: setting enabled=%d", label.c_str(), enabled);
}

Manager::Manager(Domain domain_)
    : domain(std::move(domain_)), state_file(createOrOpenStatefile(domain)) {
    initFSM();
    eventmgr.addIpcMethod("delete_job", [this](const std::string &arg) {
        auto it = jobs.find(arg);
        if (it != jobs.end()) {
            jobs.erase(it);
        }
    });
}

Manager::~Manager() {
    chan.unbindAndStopListening();
    forceUnloadAllJobs();
}

bool Manager::handleEvent(std::optional<std::chrono::milliseconds> timeout) {
    switch (fsm.state()) {
    case States::Unconfigured:
        throw std::logic_error("unsupported state");
    case States::Running:
        log_debug("waiting for an event: timeout=%lld",
                  timeout ? timeout.value().count() : 0LL);
        eventmgr.waitForEvent(timeout);
        break;
    case States::GracefulShutdown:
        if (jobs.empty()) {
            fsm.execute(Triggers::AllJobsExited);
        } else {
            log_debug("shutting down: %zu jobs remaining: waiting for an "
                      "event: timeout=0.5s",
                      jobs.size());
            eventmgr.waitForEvent(std::chrono::milliseconds{500});
        }
        break;
    case States::Finished:
        break;
    }
    return fsm.state() != States::Finished;
}

bool Manager::jobExists(const Label &label) const {
    return jobs.count(static_cast<std::string>(label));
}

Job &Manager::getJob(const Label &label) const {
    return *jobs.at(static_cast<std::string>(label));
}

bool Manager::unloadAllJobs() noexcept {
    bool success = true;
    log_debug("unloading all jobs");
    for (auto &[label, jobp] : jobs) {
        auto &job = *jobp;
        if (job.fsm.state() != Job::States::Unloaded && !job.unload_requested) {
            bool result;
            try {
                result = job.unloadJob(true);
            } catch (...) {
                log_error("unhandled exception unloading %s", job.getLabel());
                result = false;
            }
            if (!result) {
                log_error("failed to unload %s: ignoring because all jobs are "
                          "being unloaded",
                          job.getLabel());
                success = false;
            }
        }
    }
    return success;
}

//
// void Manager::rescheduleCalendarJob(Job &job) {
//    auto interval = job.manifest.calendar_interval.value();
//    auto maybe_schedule = calendar::schedule_calendar_job(interval);
//    if (!maybe_schedule) {
//        return;
//    }
//    auto [absolute_time, relative_time] = maybe_schedule.value();
//    log_debug("job %s scheduled to run in %lld minutes at t=%ld",
//              job.manifest.label.c_str(), (long long)relative_time.count(),
//              absolute_time);
//    job.state = job_state::waiting;
//    auto &label = job.manifest.label;
//    eventmgr.addTimer(relative_time, [label, this]() {
//        if (jobExists(label)) {
//            auto &job = getJob(label);
//            // The job may have been unloaded in the interval, or manually
//            // started by an administrator.
//            if (job.state == job_state::waiting) {
//                startJob(job);
//                rescheduleCalendarJob(job);
//            }
//        }
//    });
//    // XXX-FIXME: update StateFile to set the absolute start time.
//}

void Manager::startJob(Job &job) {
    log_debug("trying to start %s", job.manifest.label.c_str());
    job.fsm.execute(Job::Triggers::StartRequested);
}

void Manager::startAllJobs() {
    for (auto &[label, jobp] : pending_jobs) {
        if (jobs.count(label) == 0) {
            jobs.emplace(label, std::move(jobp));
            jobs.at(label)->fsm.execute(Job::Triggers::Bootstrap);
        } else {
            // This should not happen because loadManifest() and startAllJobs()
            // should be called together.
            assert(false);
            log_error("job %s is already loaded; will not load a new version",
                      label.c_str());
        }
    }
    pending_jobs.clear();
}

void Manager::loadDefaultManifests() {
    assert(fsm.state() == States::Unconfigured);
    log_info("loading default manifests for domain %s",
             domain.to_string().c_str());
    auto paths = domain.getLoadPaths();
    for (const auto &path : paths) {
        (void)loadAllManifests(path);
    }
}

bool Manager::loadAllManifests(const std::string &path, bool overrideDisabled,
                               bool forceLoad) {
    if (fsm.state() == States::GracefulShutdown) {
        log_error(
            "refusing to load new jobs while the manager is shutting down");
        return false;
    }
    log_debug("loading all in %s", path.c_str());

    // FIXME: do this elsewhere
    // Replace ~ with the actual HOME
    //    if (str.rfind("~", 0)) {
    //        char *home = getenv("HOME");
    //        if (!home) {
    //            continue;
    //        }
    //            str.replace(0, 5, getenv("HOME"));
    //        }
    //    }

    bool error = false;
    if (!std::filesystem::exists(path)) {
        log_warning("load failed: path does not exist: %s", path.c_str());
        return false;
    }
    if (std::filesystem::is_directory(path)) {
        log_notice("loading all manifests in %s/", path.c_str());
        using std::filesystem::directory_iterator;
        for (const auto &file : directory_iterator(path)) {
            try {
                if (!loadManifest(file.path(), overrideDisabled, forceLoad)) {
                    error = true;
                }
            } catch (...) {
                log_error("unhandled exception while parsing %s",
                          file.path().c_str());
                error = true;
            }
        }
    } else {
        try {
            std::filesystem::path p{path};
            if (!loadManifest(p, overrideDisabled, forceLoad)) {
                error = true;
            }
        } catch (...) {
            log_error("unhandled exception while parsing %s", path.c_str());
            error = true;
        }
    }

    return error;
}

bool Manager::killJob(const Label &label,
                      const std::string &signame_or_number) {
    auto maybe_signum = getSignalByName(signame_or_number);
    if (!maybe_signum) {
        return false;
    }
    if (!jobExists(label)) {
        log_debug("tried to kill a nonexistent job: %s", label.c_str());
        return false;
    }
    Job &job = getJob(label);
    bool success = job.killJob(maybe_signum.value());
    log_debug("sent signal %s to job %s with success=%d",
              signame_or_number.c_str(), label.c_str(), success);
    return success;
}

void Manager::dumpJob(const Label &label) const { getJob(label).dump(); }

void Manager::clearStateFile() {
#ifndef RELAUNCHD_UNIT_TESTS
    throw std::logic_error("This should not be used outside of testing");
#else
    state_file.clear();
#endif
}

const Domain &Manager::getDomain() const { return domain; }

void Manager::forceUnloadAllJobs() noexcept {
    for (auto &[_, jobp] : jobs) {
        jobp->forceUnloadJob();
    }
    jobs.clear();
}

void Manager::initFSM() {
    fsm.add_transitions({
        {
            States::Unconfigured,
            States::Finished,
            Triggers::StopRequested,
            [] { return true; },
            [] { return true; },
        },
        {
            States::Unconfigured,
            States::Running,
            Triggers::StartRequested,
            [] { return true; },
            [this] {
                setupSignalHandlers();
                startRpcServer();
                loadDefaultManifests();
                startAllJobs();
            },
        },
        {
            States::Running,
            States::Running,
            Triggers::StartRequested,
            [this] { return !pending_jobs.empty(); },
            [this] { startAllJobs(); },
        },
        {
            States::Running,
            States::GracefulShutdown,
            Triggers::StopRequested,
            [] { return true; },
            [this] {
                // Prevent users from submitting new jobs
                chan.unbindAndStopListening();
                unloadAllJobs();
            },
        },
        {
            States::GracefulShutdown,
            States::Finished,
            Triggers::StopRequested,
            [] { return true; },
            [] {},
        },
        {
            States::GracefulShutdown,
            States::Finished,
            Triggers::AllJobsExited,
            [] { return true; },
            [] { log_notice("all jobs have exited"); },
        },
    });

    fsm.add_debug_fn([](States from_state, States to_state, Triggers trigger) {
        log_debug("trigger %s caused the state to change from %s to %s",
                  triggerToString(trigger), stateToString(from_state),
                  stateToString(to_state));
    });
}

void Manager::startRpcServer() {
    auto sockfilename = domain.statedir / "rpc.sock";
    chan.bindAndListen(sockfilename, 1024);
    eventmgr.addSocketRead(chan.getSockFD(),
                           [this](int) { rpc_dispatch(chan, *this); });
}

void Manager::handleShutdownSignal(const std::string &signame) {
    std::string msg{"caught signal " + signame};
    switch (fsm.state()) {
    case States::Unconfigured:
        log_notice("%s before the manager started", msg.c_str());
        fsm.execute(Triggers::StopRequested);
        break;
    case States::GracefulShutdown:
        log_notice("%s; immediately unloading all running jobs", msg.c_str());
        forceUnloadAllJobs();
        fsm.execute(Triggers::AllJobsExited);
        break;
    case States::Running:
        log_notice("%s; shutting down gracefully", msg.c_str());
        fsm.execute(Triggers::StopRequested);
        break;
    case States::Finished:
        // LCOV_EXCL_START
        log_notice("%s after the manager was shutdown", msg.c_str());
        break;
        // LCOV_EXCL_STOP
    }
}

void Manager::runMainLoop() {
    if (fsm.state() != States::Running) {
        throw std::logic_error("must call startRunning() first");
    }
    while (handleEvent()) {
    }
}

bool Manager::runOnce(std::optional<std::chrono::milliseconds> timeout) {
    if (fsm.state() != States::Running) {
        throw std::logic_error("must call startRunning() first");
    }
    return handleEvent(timeout);
}

const char *Manager::stateToString(const States &state) {
    switch (state) {
    case States::Unconfigured:
        return "unconfigured";
    case States::Running:
        return "running";
    case States::GracefulShutdown:
        return "shutting-down";
    case States::Finished:
        return "finished";
    default:
        __builtin_unreachable();
    }
}

const char *Manager::triggerToString(const Triggers &trigger) {
    switch (trigger) {
    case Triggers::StartRequested:
        return "StartRequested";
    case Triggers::StopRequested:
        return "StopRequested";
    case Triggers::AllJobsExited:
        return "AllJobsExited";
    default:
        __builtin_unreachable();
    }
}

void Manager::startRunning() { fsm.execute(Triggers::StartRequested); }

void Manager::stopRunning() { fsm.execute(Triggers::StopRequested); }
