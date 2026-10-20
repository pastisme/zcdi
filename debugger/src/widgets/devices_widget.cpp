#include "widgets/devices_widget.h"

#include "cdic/cdic.h"
#include "core/machine_inspector.h"
#include "disc/sector.h"
#include "dvc/dvc_video.h"
#include "imgui/imgui.h"
#include "scc68070/scc68070.h"
#include "slave/slave.h"
#include "vmpeg/vmpeg.h"

#include <cstdarg>
#include <cstdio>

namespace {

// One "name  value" row of the current two-column table.
void field(const char* name, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("%s", name);
    ImGui::TableSetColumnIndex(1);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

bool begin_fields(const char* id) {
    return ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit);
}

const char* yes(bool b) { return b ? "yes" : "no"; }

const char* cdic_command_name(uint16_t command) {
    switch (command & 0xff) {
        case 0x00: return "idle";
        case 0x23: return "Reset Mode 1";
        case 0x24: return "Reset Mode 2";
        case 0x27: return "Fetch TOC";
        case 0x28: return "Play CDDA";
        case 0x29: return "Read Mode 1";
        case 0x2a: return "Read Mode 2";
        case 0x2b: return "Stop CDDA";
        case 0x2c: return "Seek";
        case 0x2e: return "Update";
        default: return "?";
    }
}

void draw_cdic(const zcdi::mamecdi::Cdic& cdic) {
    const zcdi::mamecdi::CdicSaveState& s = cdic.state();
    int m = 0, sec = 0, f = 0;
    zcdi::lba_to_msf(s.curr_lba_, m, sec, f);
    if (ImGui::CollapsingHeader("Registers", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("cdic_regs")) {
        field("Command", "%04X  %s", s.command_, cdic_command_name(s.command_));
        field("Time (MSF)", "%02X:%02X:%02X", (s.time_ >> 24) & 0xff, (s.time_ >> 16) & 0xff, (s.time_ >> 8) & 0xff);
        field("File", "%04X", s.file_);
        field("Channels", "%08X", s.channel_);
        field("Audio channels", "%04X", s.audio_channel_);
        field("DBUF", "%04X  bank %d%s%s", s.data_buffer_, s.data_buffer_ & 1, (s.data_buffer_ & 4) ? " audio" : "",
              (s.data_buffer_ & 0x4000) ? " active" : "");
        field("XBUF", "%04X%s", s.x_buffer_, (s.x_buffer_ & 0x8000) ? "  sector ready" : "");
        field("ABUF", "%04X%s", s.audio_buffer_, (s.audio_buffer_ & 0x8000) ? "  audio ready" : "");
        field("AUDCTL", "%04X", s.audio_control_);
        field("DMA control", "%04X", s.dma_control_);
        field("IRQ vector", "%02X", s.interrupt_vector_);
        field("IRQ4", "%s", yes(s.irq4_));
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Disc", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("cdic_disc")) {
        field("Running", "%02X  %s", s.disc_command_, cdic_command_name(s.disc_command_));
        field("LBA", "%d  (%02d:%02d:%02d)", s.curr_lba_, m, sec, f);
        field("Disc present", "%s", yes(cdic.disc_active()));
        field("Spin-up", "%d", s.spinup_counter_);
        field("Spin-down", "%d", s.spin_down_count_);
        field("Read failures", "%d", s.read_fail_streak_);
        field("Next data/audio bank", "%d / %d", s.data_target_buffer_ ? 1 : 0, s.audio_target_buffer_ ? 1 : 0);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Audio player", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("cdic_audio")) {
        field("Playing", "%s%s", yes(s.decoding_audio_map_), s.cdda_mode_ ? "  (CD-DA)" : "");
        field("Next buffer", "%04X", s.decode_addr_);
        field("Ticks left", "%d of %d", s.audio_sector_counter_, s.audio_format_sectors_);
        ImGui::EndTable();
    }
}

void draw_vmpeg(const zcdi::mamecdi::VmpegDevice& vmpeg, const zcdi::dvc::DvcVideo& video, bool dvc_loaded) {
    if (!dvc_loaded) ImGui::TextDisabled("No DVC cartridge ROM loaded");
    const zcdi::mamecdi::VmpegSaveState& s = vmpeg.state();
    if (ImGui::CollapsingHeader("FMV (video)", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("fmv")) {
        field("System command", "%04X", s.fmv_system_command_register_);
        field("Video command", "%04X", s.fmv_video_command_register_);
        field("System control", "%04X", s.fmv_system_control_register_);
        field("Decoder command", "%04X", s.fmv_decoder_command_);
        field("Playback", "%s%s", yes(s.fmv_playback_active_), s.fmv_show_on_next_video_frame_ ? ", show next" : "");
        field("ISR / IER / IVR", "%04X / %04X / %04X", s.fmv_interrupt_status_register_,
              s.fmv_interrupt_enable_register_, s.fmv_interrupt_vector_register_);
        field("Image", "%dx%d", s.image_width_, s.image_height_);
        field("Shown", "%dx%d", s.display_width_, s.display_height_);
        field("Stream", "%d", s.fmv_stream_number_);
        field("Demuxer DTS", "%lld", static_cast<long long>(s.fmv_demuxer_dts_));
        field("DTS FIFO", "%zu", s.fmv_dts_fifo_.size());
        field("ES bytes waiting", "%zu", s.fmv_es_bytes_.size());
        field("Decoded time code", "%08X", s.fmv_decoder_timecode_);
        field("Shown time code", "%08X", s.fmv_display_timecode_);
        field("Slow motion", "%d", s.fmv_slow_motion_);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Decoder queue", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("dvc_queue")) {
        field("Queued pictures", "%zu of %zu", video.queue_depth(), zcdi::dvc::DvcVideo::kMaxDecodedPictures);
        field("Held in decoder", "%d", video.decoder_held_pictures());
        field("On screen", "%s  %dx%d", yes(video.has_frame()), video.width(), video.height());
        field("Shown time code", "%08X", video.presented_timecode());
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Window", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("fmv_window")) {
        field("Display offset", "%d, %d  (latched %d, %d)", s.video_ctrl_x_offset_, s.video_ctrl_y_offset_,
              s.latched_display_offset_x_, s.latched_display_offset_y_);
        field("Window", "%dx%d  (latched %dx%d)", s.video_ctrl_window_width_, s.video_ctrl_window_height_,
              s.latched_window_width_, s.latched_window_height_);
        field("Decoder offset", "%d, %d", s.video_ctrl_decoder_offset_x_, s.video_ctrl_decoder_offset_y_);
        field("RegsUpd pending", "%s", yes(s.register_update_latch_));
        field("VCD pixel clock", "%s", yes(s.vcd_pixel_clock_));
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("FMA (audio)", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("fma")) {
        field("Command / status", "%04X / %02X", s.fma_command_register_, s.fma_status_register_);
        field("ISR / IER / IVR", "%04X / %04X / %04X", s.fma_interrupt_status_register_,
              s.fma_interrupt_enable_register_, s.fma_interrupt_vector_register_);
        field("Stream", "%d", s.fma_stream_number_);
        field("Header", "%08X", s.fma_mpeg_audio_header_);
        field("DSP enabled", "%s", yes(vmpeg.audio_dsp_enabled()));
        field("Unplayed samples", "%d", s.fma_unplayed_samples_);
        field("ES bytes waiting", "%zu", s.fma_es_bytes_.size());
        field("Volume LL LR RL RR", "%d %d %d %d", s.mpeg_volume_l2l_, s.mpeg_volume_l2r_, s.mpeg_volume_r2l_,
              s.mpeg_volume_r2r_);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Host interface") && begin_fields("vmpeg_host")) {
        field("MPEG RAM", "%s", yes(vmpeg.mpeg_ram_enabled()));
        field("DMA to", "%s%s", s.dma_for_fma_ ? "FMA" : "FMV", s.dma_active_ ? ", active" : "");
        field("Pending", "%s%s%s", s.clear_fifo_pending_ ? "clear FIFO " : "",
              s.decoder_off_pending_ ? "decoder off " : "", s.audio_reset_pending_ ? "audio reset" : "");
        ImGui::EndTable();
    }
}

void draw_dma_channel(const char* id, const zcdi::mamecdi::DmaChannel& ch) {
    if (!begin_fields(id)) return;
    field("CSR / CER", "%02X / %02X", ch.csr, ch.cer);
    field("DCR / OCR / SCR / CCR", "%02X / %02X / %02X / %02X", ch.dcr, ch.ocr, ch.scr, ch.ccr);
    field("Count", "%04X", ch.mtc);
    field("Address", "%06X", ch.mac);
    ImGui::EndTable();
}

void draw_scc68070(const zcdi::mamecdi::Scc68070Periph& scc) {
    const zcdi::mamecdi::Scc68070SaveState& s = scc.state();
    if (ImGui::CollapsingHeader("Interrupts", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("scc_irq")) {
        field("LIR", "%02X  (INT1 level %d)", s.lir_, (s.lir_ >> 4) & 7);
        field("PICR1", "%02X  (I2C %d, timer %d)", s.picr1_, (s.picr1_ >> 4) & 7, s.picr1_ & 7);
        field("PICR2", "%02X  (UART rx %d, tx %d)", s.picr2_, (s.picr2_ >> 4) & 7, s.picr2_ & 7);
        field("Inputs", "INT1 %s  IN2 %s  IN4 %s", yes(s.int1_asserted_), yes(s.in2_), yes(s.in4_));
        field("UART tx", "%s", yes(s.uart_tx_int_));
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Timer", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("scc_timer")) {
        field("TSR / TCR", "%02X / %02X", s.timer_tsr_, s.timer_tcr_);
        field("Reload", "%04X", s.timer_reload_);
        field("T0 / T1 / T2", "%04X / %04X / %04X", s.timer0_, s.timer1_, s.timer2_);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("DMA channel 0", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_dma_channel("dma0", s.dma_[0]);
    }
    if (ImGui::CollapsingHeader("DMA channel 1", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_dma_channel("dma1", s.dma_[1]);
    }
    if (ImGui::CollapsingHeader("UART") && begin_fields("scc_uart")) {
        field("UMR / UCSR / UCR", "%02X / %02X / %02X", s.uart_umr_, s.uart_ucsr_, s.uart_ucr_);
        field("Received", "%02X%s", s.uart_rhr_, s.uart_rx_valid_ ? " (unread)" : "");
        ImGui::EndTable();
    }
}

void draw_slave(const zcdi::mamecdi::SlaveHle& slave) {
    const zcdi::mamecdi::SlaveSaveState& s = slave.state();
    if (ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("slave_state")) {
        field("IRQ2", "%s", yes(slave.irq2_asserted()));
        field("Tray open", "%s", yes(s.tray_open_));
        field("Status forwarding", "%s", yes(s.status_forwarding_));
        field("Pointer", "%s  at %d, %d  buttons %02X", s.pointer_enabled_ ? "on" : "off", s.pointer_x_, s.pointer_y_,
              s.buttons_);
        static const char* const kTypes[] = {"none", "relative (M)", "maneuvering (J)", "absolute (T)", "absolute (S)"};
        const unsigned type = unsigned(s.front_type_);
        field("Front device", "%s%s  %s baud", type < 5 ? kTypes[type] : "power-on",
              s.detect_ticks_ ? ", detecting" : "", s.sci_9600_ ? "9600" : "1200");
        static const char* const kPc6[] = {"released", "high", "low"};
        field("Back RTS (PC6)", "%s", kPc6[unsigned(s.pc6_)]);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen) && begin_fields("slave_channels")) {
        for (int c = 0; c < zcdi::mamecdi::SlaveSaveState::kNumChannels; ++c) {
            const auto& ch = s.channels_[c];
            char in[64]{};
            int at = 0;
            for (int i = 0; i < ch.in_len && at < 60; ++i)
                at += std::snprintf(in + at, sizeof(in) - at, "%02X ", ch.in[i]);
            char name[16]{};
            std::snprintf(name, sizeof(name), "Channel %d", c);
            field(name, "in [%s]  out %d/%d", in, ch.out_pos, ch.out_len);
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("Audio attenuation") && begin_fields("slave_audio")) {
        field("Command", "%02X", s.att_command_);
        field("dB LL LR RR RL", "%d %d %d %d", s.att_db_[0], s.att_db_[1], s.att_db_[2], s.att_db_[3]);
        field("DAC", "%d %d %d %d", s.dac_[0], s.dac_[1], s.dac_[2], s.dac_[3]);
        field("Master", "%02X%s", s.master_, s.master_enabled_ ? " (on)" : "");
        field("Mute / dim / fade", "%s / %s / %s", yes(s.muted_), yes(s.dim_), yes(s.fading_));
        ImGui::EndTable();
    }
}

} // namespace

void DevicesWidget::draw(AppState& state) {
    if (!begin_machine_window(state)) return;
    const zcdi::MachineInspector view(*state.machine);
    if (ImGui::BeginTabBar("devices_tabs")) {
        if (ImGui::BeginTabItem("CDIC")) {
            draw_cdic(view.cdic());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("VMPEG / DVC")) {
            draw_vmpeg(view.vmpeg(), view.dvc_video(), state.dvc_rom_loaded);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SCC68070")) {
            draw_scc68070(view.scc68070());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("SLAVE")) {
            draw_slave(view.slave());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
