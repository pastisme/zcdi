#include "emu_runner.h"

#include "core/cpu/musashi_core.h"
#include "core/machine.h"
#include "core/machine_inspector.h"
#include "names.h"

#include <utility>

namespace {

// Musashi's hook takes no user pointer; there is one CPU and one runner.
EmuRunner* g_runner = nullptr;

bool is_return(uint16_t opcode) {
    return opcode == 0x4e75 || opcode == 0x4e73 || opcode == 0x4e77; // RTS, RTE, RTR
}

} // namespace

EmuRunner::EmuRunner(std::function<void()> run_one_frame) : run_one_frame_(std::move(run_one_frame)) {
    g_runner = this;
    thread_ = std::thread([this]() { thread_main(); });
}

EmuRunner::~EmuRunner() {
    finish_frame();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        emu_turn_ = true;
    }
    cv_.notify_all();
    thread_.join();
    zcdi::MusashiCore::set_instruction_hook(nullptr);
    g_runner = nullptr;
}

void EmuRunner::attach(zcdi::Machine* machine) {
    machine_ = machine;
    mode_ = Mode::kNone;
    skip_first_ = false;
    trace.clear();
    exceptions.clear();
    vectors_frame_ = -1;
    if (machine_) zcdi::MusashiCore::set_instruction_hook(&EmuRunner::hook);
}

void EmuRunner::begin(Mode mode, uint32_t target) {
    mode_ = mode;
    target_ = target;
    prev_was_return_ = false;
    if (!machine_) return;
    const zcdi::MachineInspector view(*machine_);
    start_sp_ = view.sp();
    start_supervisor_ = view.cpu().supervisor();
    start_line_ = view.line();
    start_frame_ = view.frame();
}

EmuRunner::Stop EmuRunner::run(int frames) {
    frames_left_ = frames;
    stopped_ = false;
    stop_ = Stop::kFrames;
    std::unique_lock<std::mutex> lock(mutex_);
    emu_turn_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this]() { return !emu_turn_; });
    return stop_;
}

void EmuRunner::finish_frame() {
    if (!mid_frame_) return;
    ignore_stops_ = true;
    mode_ = Mode::kNone;
    run(1);
    ignore_stops_ = false;
}

void EmuRunner::hook(unsigned int pc) {
    if (g_runner) g_runner->on_instruction(pc);
}

// On the emulation thread, before each instruction.
void EmuRunner::on_instruction(uint32_t pc) {
    hook_seen_ = true;
    const int vector = (tracing || !vector_breaks.empty()) && machine_ ? record(pc) : -1;
    if (ignore_stops_ || (mode_ == Mode::kNone && breakpoints.empty() && os9_breaks.empty() && vector_breaks.empty())) {
        return;
    }
    const bool skip = skip_first_;
    skip_first_ = false;
    Stop why = Stop::kStep;
    bool stop = false;
    if (!skip) {
        stop = should_stop(pc, why);
        if (!stop && vector >= 0 && vector_breaks.count(static_cast<uint8_t>(vector))) {
            stop_note = vector_name(static_cast<uint8_t>(vector));
            why = Stop::kBreakpoint;
            stop = true;
        }
        if (!stop && !os9_breaks.empty()) {
            const zcdi::MachineInspector view(*machine_);
            const uint16_t op = view.peek16(pc);
            const uint16_t code = view.peek16(pc + 2);
            if (op == 0x4e40 && os9_breaks.count(code)) {
                const char* name = os9_call_name(code);
                stop_note = std::string("OS-9 ") + (name ? name : "call");
                why = Stop::kBreakpoint;
                stop = true;
            }
        }
    }
    if (mode_ == Mode::kStepOut && machine_) {
        prev_was_return_ = is_return(zcdi::MachineInspector(*machine_).peek16(pc));
    }
    if (!stop) return;

    mode_ = Mode::kNone;
    stopped_ = true;
    stop_ = why;
    skip_first_ = true; // resuming runs this instruction first
    std::unique_lock<std::mutex> lock(mutex_);
    hand_back(lock);
}

int EmuRunner::record(uint32_t pc) {
    const zcdi::MachineInspector view(*machine_);
    const int frame = view.frame();
    if (frame != vectors_frame_) {
        // Vectors 2-255; the lowest wins where handlers are shared.
        vectors_frame_ = frame;
        vector_targets_.clear();
        for (int v = 255; v >= 2; --v)
            vector_targets_[view.peek32(uint32_t(v) * 4u) & 0x00ffffffu] = uint8_t(v);
    }
    const int16_t line = static_cast<int16_t>(view.line());
    const auto vector = vector_targets_.find(pc);
    const int found = vector != vector_targets_.end() ? vector->second : -1;
    if (tracing) {
        if (found >= 0) exceptions.push({frame, line, static_cast<uint8_t>(found), pc, last_pc_});
        trace.push({pc, line});
    }
    last_pc_ = pc;
    return found;
}

bool EmuRunner::should_stop(uint32_t pc, Stop& why) {
    const auto bp = breakpoints.find(pc);
    if (bp != breakpoints.end() && bp->second) {
        why = Stop::kBreakpoint;
        stop_note.clear();
        return true;
    }
    if (mode_ == Mode::kNone || !machine_) return false;
    why = Stop::kStep;
    const zcdi::MusashiCore& cpu = zcdi::MachineInspector(*machine_).cpu();
    const uint32_t sp = cpu.sp();
    const bool same_bank = cpu.supervisor() == start_supervisor_;
    switch (mode_) {
        case Mode::kStepInto: return true;
        case Mode::kStepOver: return pc == target_ && same_bank && sp >= start_sp_;
        case Mode::kStepLine: {
            const zcdi::MachineInspector view(*machine_);
            return view.line() != start_line_ || view.frame() != start_frame_;
        }
        case Mode::kStepOut: return prev_was_return_ && same_bank && sp > start_sp_;
        case Mode::kRunTo: return pc == target_;
        case Mode::kNone: break;
    }
    return false;
}

// Also the frame a stop cut: resuming returns into it from the hook.
void EmuRunner::run_frames() {
    while (frames_left_ > 0 && !stopped_) {
        mid_frame_ = true;
        run_one_frame_();
        mid_frame_ = false;
        --frames_left_;
    }
}

void EmuRunner::hand_back(std::unique_lock<std::mutex>& lock) {
    emu_turn_ = false;
    cv_.notify_all();
    cv_.wait(lock, [this]() { return emu_turn_; });
}

void EmuRunner::thread_main() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [this]() { return emu_turn_; });
        if (quit_) return;
        lock.unlock();
        run_frames();
        lock.lock();
        emu_turn_ = false;
        cv_.notify_all();
    }
}
