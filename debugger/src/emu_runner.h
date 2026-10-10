#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zcdi {
class Machine;
}

// A fixed-size ring, oldest entry first by index.
template <class T>
class Ring {
public:
    explicit Ring(std::size_t capacity) : items_(capacity) {}
    void push(const T& item) {
        items_[head_] = item;
        head_ = (head_ + 1) % items_.size();
        if (count_ < items_.size()) ++count_;
    }
    std::size_t size() const { return count_; }
    const T& operator[](std::size_t i) const { return items_[(head_ + items_.size() - count_ + i) % items_.size()]; }
    void clear() { head_ = count_ = 0; }

private:
    std::vector<T> items_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

struct TraceEntry {
    uint32_t pc = 0;
    int16_t line = 0;
};

// An instruction that is a vector's handler: the hook sees no exceptions.
struct ExceptionEntry {
    int frame = 0;
    int16_t line = 0;
    uint8_t vector = 0;
    uint32_t handler = 0;
    uint32_t from = 0; // the instruction before
};

// Runs frames on its own thread, which parks in Musashi's instruction hook
// to stop mid-frame. The UI waits while it runs.
class EmuRunner {
public:
    enum class Mode : uint8_t {
        kNone,
        kStepInto, // the next instruction
        kStepOver, // `target` (after the call), same SR.S, SP no deeper
        kStepLine, // the first instruction on another field line
        kStepOut, // after a return that leaves the starting stack level
        kRunTo, // `target`, at any stack level
    };
    enum class Stop : uint8_t { kFrames, kBreakpoint, kStep };

    explicit EmuRunner(std::function<void()> run_one_frame);
    ~EmuRunner();
    EmuRunner(const EmuRunner&) = delete;
    EmuRunner& operator=(const EmuRunner&) = delete;

    // After each build, not mid-frame: Musashi's init drops the hook.
    void attach(zcdi::Machine* machine);
    // Sets what the next run() stops on; kNone keeps only breakpoints.
    // `target` is for kStepOver and kRunTo.
    void begin(Mode mode, uint32_t target = 0);
    // After a stop: the next run() executes PC's instruction unchecked.
    void note_paused() { skip_first_ = true; }
    // Up to `frames` frame ends (a cut frame counts); kFrames if all ran.
    Stop run(int frames);
    // Ends a cut frame with breakpoints and the mode off, e.g. before a reset.
    void finish_frame();
    bool mid_frame() const { return mid_frame_; }
    Mode mode() const { return mode_; }
    // The hook ran at least once: the core was built with it.
    bool hook_seen() const { return hook_seen_; }

    // address -> enabled
    std::map<uint32_t, bool> breakpoints;
    // Stop at a TRAP #0 calling one of these OS-9 functions (before it runs).
    std::set<uint16_t> os9_breaks;
    // Stop at the handler of one of these exception vectors.
    std::set<uint8_t> vector_breaks;
    // What the last breakpoint stop was for, when not a plain address.
    std::string stop_note;

    // Filled by the hook while on; read only while parked.
    bool tracing = true;
    Ring<TraceEntry> trace{1u << 16};
    Ring<ExceptionEntry> exceptions{4096};

private:
    static void hook(unsigned int pc);
    void on_instruction(uint32_t pc);
    bool should_stop(uint32_t pc, Stop& why);
    void run_frames(); // on the emulation thread
    void hand_back(std::unique_lock<std::mutex>& lock);
    void thread_main();

    std::function<void()> run_one_frame_;
    zcdi::Machine* machine_ = nullptr;

    Mode mode_ = Mode::kNone;
    uint32_t target_ = 0;
    uint32_t start_sp_ = 0;
    bool start_supervisor_ = false;
    int start_line_ = 0;
    int start_frame_ = 0;
    bool prev_was_return_ = false;
    bool skip_first_ = false;
    bool ignore_stops_ = false;
    bool hook_seen_ = false;

    // The vector `pc` is the handler of (-1: none); records the trace if on.
    int record(uint32_t pc);
    // Handler address -> vector, from the table at 0, rebuilt each frame.
    std::unordered_map<uint32_t, uint8_t> vector_targets_;
    int vectors_frame_ = -1;
    uint32_t last_pc_ = 0;

    int frames_left_ = 0;
    bool stopped_ = false;
    Stop stop_ = Stop::kFrames;
    bool mid_frame_ = false;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool emu_turn_ = false;
    bool quit_ = false;
    std::thread thread_;
};
