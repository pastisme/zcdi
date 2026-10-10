#pragma once

#include "widgets/widget.h"

#include "core/cpu/musashi_core.h"
#include "core/machine_inspector.h"

#include <optional>

class CpuWidget : public DebuggerWidget {
public:
    CpuWidget() : DebuggerWidget("CPU") {}
    void draw(AppState& state) override;

private:
    // `shown` replaces the "NAME value" text.
    void reg(const char* name, zcdi::CpuReg id, uint32_t value, bool changed, bool editable,
             const char* shown = nullptr);

    // Registers at the last two stops, to mark what the last step changed.
    zcdi::MachineInspector::CpuRegisters before_{};
    zcdi::MachineInspector::CpuRegisters at_stop_{};
    int seen_stop_ = -1;
    // The register being edited.
    std::optional<zcdi::CpuReg> editing_;
    uint32_t edit_value_ = 0;
};
