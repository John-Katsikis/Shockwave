#include <Engine/Schedule.h>

#include <Core/Profile.h>

#include <algorithm>

namespace shockwave {

namespace {

bool intersects(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    for (uint32_t x : a) {
        if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    }
    return false;
}

bool conflicts(const Schedule::Access& a, const Schedule::Access& b) {
    if (a.exclusive || b.exclusive) return true;
    return intersects(a.writes, b.writes) || intersects(a.writes, b.reads) || intersects(b.writes, a.reads);
}

}   // namespace

void Schedule::add(std::string name, Access access, SystemFn fn) {
    m_systems.push_back({std::move(name), std::move(access), std::move(fn)});
    m_built = false;
}

// Each system goes one stage after the last earlier system it conflicts with. Stages run
// one after another; the systems inside a stage run at the same time.
void Schedule::build() const {
    std::vector<size_t> stageOf(m_systems.size(), 0);
    m_stages.clear();
    for (size_t i = 0; i < m_systems.size(); ++i) {
        size_t stage = 0;
        for (size_t j = 0; j < i; ++j) {
            if (conflicts(m_systems[i].access, m_systems[j].access)) stage = std::max(stage, stageOf[j] + 1);
        }
        stageOf[i] = stage;
        if (stage >= m_stages.size()) m_stages.resize(stage + 1);
        m_stages[stage].push_back(i);
    }
    m_built = true;
}

// One profiler zone per system, named after it.
static void runSystem(const auto& system, const SystemContext& ctx) {
    SW_PROFILE_SCOPE_DYNAMIC(system.name.data(), system.name.size());
    system.fn(ctx);
}

void Schedule::run(World& world, JobSystem& jobs, double dt) {
    if (!m_built) build();
    if (m_commands.size() != jobs.threadCount()) m_commands.resize(jobs.threadCount());

    SystemContext ctx{world, jobs, static_cast<float>(dt), &m_commands};
    for (const std::vector<size_t>& stage : m_stages) {
        if (stage.size() == 1) {
            runSystem(m_systems[stage[0]], ctx);   // nothing to run beside it
            continue;
        }
        JobCounter counter;
        for (size_t i : stage) {
            jobs.run([this, i, &ctx] { runSystem(m_systems[i], ctx); }, &counter);
        }
        jobs.wait(counter);
    }
    for (Commands& commands : m_commands) commands.apply(world);
}

std::vector<std::vector<std::string>> Schedule::stages() const {
    if (!m_built) build();
    std::vector<std::vector<std::string>> names;
    for (const std::vector<size_t>& stage : m_stages) {
        auto& row = names.emplace_back();
        for (size_t i : stage) row.push_back(m_systems[i].name);
    }
    return names;
}

}   // namespace shockwave
