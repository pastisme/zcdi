#include "scc68070/scc68070.h"

#include "core/state_archive.h"
#include "core/util.h"

#include <algorithm>
#include <utility>

namespace zcdi {
namespace mamecdi {

namespace {
constexpr uint32_t kLir = 0x80001001;

constexpr uint32_t kI2cIdr = 0x80002001;
constexpr uint32_t kI2cIar = 0x80002003;
constexpr uint32_t kI2cIsr = 0x80002005; // read-only, fixed "idle" value
constexpr uint32_t kI2cIcr = 0x80002007;
constexpr uint32_t kI2cIccr = 0x80002009;
constexpr uint8_t kI2cIsrIdle = 0x10; // ISR_PIN: no I2C interrupt pending

constexpr uint32_t kUartUmr = 0x80002011;
constexpr uint32_t kUartUsr = 0x80002013;
constexpr uint32_t kUartUcsr = 0x80002015;
constexpr uint32_t kUartUcr = 0x80002017;
constexpr uint32_t kUartUth = 0x80002019;
constexpr uint32_t kUartUrh = 0x8000201b;
constexpr uint8_t kUsrRxRdy = 0x01;
constexpr uint8_t kUsrTxRdy = 0x04;
constexpr uint8_t kUsrTxEmt = 0x08;
constexpr std::size_t kUartOutputCap = 1 << 16;

constexpr uint32_t kTimerTsrTcr = 0x80002020; // word: hi=TSR, lo=TCR
constexpr uint32_t kTimerReload = 0x80002022;
constexpr uint32_t kTimer0 = 0x80002024;
constexpr uint32_t kTimer1 = 0x80002026;
constexpr uint32_t kTimer2 = 0x80002028;
constexpr uint8_t kTsrOv0 = 0x80;
constexpr uint8_t kTsrMa1 = 0x40;
constexpr uint8_t kTsrMa2 = 0x08;
constexpr uint8_t kTsrFlags = 0xfe; // bit 0 reserved
constexpr uint8_t kTcrMatch = 0x1; // T1 mode in bits 5:4, T2 in bits 1:0

constexpr uint32_t kPicr1 = 0x80002045;
constexpr uint32_t kPicr2 = 0x80002047;

// DMA channel 0 at 0x80004000, channel 1 (VMPEG's, cditop.sv req2/ack2,
// which MAME/same_cdi never decode) at +0x40; layout as MAME's dma_r/dma_w.
constexpr uint32_t kDmaBase = 0x80004000;
constexpr uint32_t kDmaEnd = 0x80004080;
constexpr uint32_t kDmaCsr = 0x00; // hi=channel_status, lo=channel_error
constexpr uint32_t kDmaOcr = 0x04; // hi=device_control, lo=operation_control
constexpr uint32_t kDmaCcr = 0x06; // hi=sequence_control, lo=channel_control
constexpr uint32_t kDmaMtc = 0x0a; // transfer_counter (word)
constexpr uint32_t kDmaMac = 0x0c; // memory_address_counter (32-bit, hi word then lo word)

constexpr int kCyclesPerTimerTick = 96;

// CPU cycles per character (10 bit times) for each UCSR clock select:
// divisor * 10 * kCpuClockHz / 4915200, floored (datasheet Fig. 37).
constexpr int kUartTxPeriodCycles[8] = {
    2000000, 1000000, 500000, 125000, 62500, 31250, 15625, 7812,
};

} // namespace

Scc68070Periph::Scc68070Periph() = default;

template <class T>
void Scc68070Periph::state_io(T& archive) {
    archive.begin_section(state_tag("SCC "));
    archive.io_struct(static_cast<Scc68070SaveState&>(*this));
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(Scc68070Periph);

uint8_t Scc68070Periph::read_byte(uint32_t addr) {
    if (addr >= kDmaBase && addr < kDmaEnd) return read_dma(addr);
    switch (addr) {
        case kLir: return lir_ & 0x77;

        case kI2cIdr: return i2c_idr_;
        case kI2cIar: return i2c_iar_;
        case kI2cIsr: return kI2cIsrIdle;
        case kI2cIcr: return i2c_icr_;
        case kI2cIccr: return i2c_iccr_;

        case kUartUmr: return uart_umr_;
        case kUartUsr:
            return kUsrTxRdy | kUsrTxEmt |
                   (uart_rx_valid_ ? kUsrRxRdy : 0); // tx always ready: bytes go straight to uart_output_
        case kUartUcsr: return uart_ucsr_;
        case kUartUcr: return uart_ucr_;
        case kUartUrh: uart_rx_valid_ = false; return uart_rhr_;

        case kTimerTsrTcr: return timer_tsr_;
        case kTimerTsrTcr + 1: return timer_tcr_;
        case kTimerReload: return uint8_t(timer_reload_ >> 8);
        case kTimerReload + 1: return uint8_t(timer_reload_);
        case kTimer0: return uint8_t(timer0_ >> 8);
        case kTimer0 + 1: return uint8_t(timer0_);
        case kTimer1: return uint8_t(timer1_ >> 8);
        case kTimer1 + 1: return uint8_t(timer1_);
        case kTimer2: return uint8_t(timer2_ >> 8);
        case kTimer2 + 1: return uint8_t(timer2_);

        case kPicr1: return picr1_ & 0x77;
        case kPicr2: return picr2_ & 0x77;

        default: return 0; // MMU and other unmodelled registers
    }
}

void Scc68070Periph::write_byte(uint32_t addr, uint8_t v) {
    if (addr >= kDmaBase && addr < kDmaEnd) {
        write_dma(addr, v);
        return;
    }
    switch (addr) {
        case kLir:
            // Level fields only: there is no pending bit to acknowledge.
            lir_ = v & 0x77;
            return;

        case kI2cIdr: i2c_idr_ = v; return;
        case kI2cIar: i2c_iar_ = v; return;
        case kI2cIcr: i2c_icr_ = v; return;
        case kI2cIccr: i2c_iccr_ = v; return;

        case kUartUmr: uart_umr_ = v; return;
        case kUartUcsr:
            uart_ucsr_ = v;
            // Restarts the transmit baud timer (MAME tx_timer->adjust).
            uart_tx_period_cycles_ = kUartTxPeriodCycles[v & 7];
            uart_tx_cycle_accum_ = 0;
            return;
        case kUartUcr: uart_ucr_ = v; return;
        case kUartUth:
            if (uart_output_.size() < kUartOutputCap) uart_output_.push_back(static_cast<char>(v));
            return;

        case kTimerTsrTcr: timer_tsr_ &= ~v; return; // write-1-to-clear
        case kTimerTsrTcr + 1: timer_tcr_ = v; return;
        case kTimerReload: timer_reload_ = set_hi(timer_reload_, v); return;
        case kTimerReload + 1: timer_reload_ = set_lo(timer_reload_, v); return;
        case kTimer0: timer0_ = set_hi(timer0_, v); return;
        case kTimer0 + 1: timer0_ = set_lo(timer0_, v); return;
        case kTimer1: timer1_ = set_hi(timer1_, v); return;
        case kTimer1 + 1: timer1_ = set_lo(timer1_, v); return;
        case kTimer2: timer2_ = set_hi(timer2_, v); return;
        case kTimer2 + 1: timer2_ = set_lo(timer2_, v); return;

        case kPicr1:
            picr1_ = (picr1_ & 0x88) | (v & 0x77);
            if (v & 0x80) picr1_ &= ~0x80;
            if (v & 0x08) picr1_ &= ~0x08;
            return;
        case kPicr2:
            picr2_ = (picr2_ & 0x88) | (v & 0x77);
            if (v & 0x80) picr2_ &= ~0x80;
            if (v & 0x08) {
                picr2_ &= ~0x08;
                uart_tx_int_ = false;
            } // ack uart-tx
            return;

        default: return; // MMU and other unmodelled registers
    }
}

uint8_t Scc68070Periph::read_dma(uint32_t addr) const {
    const DmaChannel& ch = dma_[(addr - kDmaBase) >> 6];
    switch (addr & 0x3f) {
        case kDmaCsr: return ch.csr;
        case kDmaCsr + 1: return ch.cer;
        case kDmaOcr: return ch.dcr;
        case kDmaOcr + 1: return ch.ocr;
        case kDmaCcr: return ch.scr;
        case kDmaCcr + 1: return ch.ccr;
        case kDmaMtc: return uint8_t(ch.mtc >> 8);
        case kDmaMtc + 1: return uint8_t(ch.mtc);
        case kDmaMac: return uint8_t(ch.mac >> 24);
        case kDmaMac + 1: return uint8_t(ch.mac >> 16);
        case kDmaMac + 2: return uint8_t(ch.mac >> 8);
        case kDmaMac + 3: return uint8_t(ch.mac);
        default: return 0;
    }
}

void Scc68070Periph::write_dma(uint32_t addr, uint8_t v) {
    DmaChannel& ch = dma_[(addr - kDmaBase) >> 6];
    switch (addr & 0x3f) {
        // Write-1-to-clear for COC|NDT|ERR only (MAME dma_w); CA is live.
        case kDmaCsr: ch.csr &= ~(v & 0xb0u); return;
        case kDmaCsr + 1: ch.cer = v; return;
        case kDmaOcr: ch.dcr = v; return;
        case kDmaOcr + 1: ch.ocr = v; return;
        case kDmaCcr: ch.scr = v; return;
        case kDmaCcr + 1:
            // CCR_SO is a strobe that sets CSR_COC (MAME dma_w); cdapdriv
            // busy-polls CSR for it after each CDIC DMA.
            if (v & 0x80) ch.csr |= 0x80u;
            ch.ccr = v & 0x7f;
            return;
        case kDmaMtc: ch.mtc = set_hi(ch.mtc, v); return;
        case kDmaMtc + 1: ch.mtc = set_lo(ch.mtc, v); return;
        case kDmaMac: ch.mac = set_byte(ch.mac, 24, v); return;
        case kDmaMac + 1: ch.mac = set_byte(ch.mac, 16, v); return;
        case kDmaMac + 2: ch.mac = set_byte(ch.mac, 8, v); return;
        case kDmaMac + 3: ch.mac = set_byte(ch.mac, 0, v); return;
        default: return;
    }
}

void Scc68070Periph::complete_dma_ch1_burst(uint32_t words_transferred) {
    // Once per burst, not per word: VMPEG holds req/rdy for the whole burst.
    DmaChannel& ch = dma_[1];
    uint32_t consumed = words_transferred < ch.mtc ? words_transferred : ch.mtc;
    ch.mtc = uint16_t(ch.mtc - consumed);
    if (ch.mtc == 0) ch.csr |= 0x80u; // CSR_COC
}

void Scc68070Periph::tick(int cpu_cycles) {
    timer0_cycle_accum_ += cpu_cycles;
    while (timer0_cycle_accum_ >= kCyclesPerTimerTick) {
        timer0_cycle_accum_ -= kCyclesPerTimerTick;
        if (timer0_ == 0xffff) {
            timer0_ = timer_reload_;
            timer_tsr_ |= kTsrOv0;
        } else {
            ++timer0_;
        }
        // Timers 1/2 wired as per the datasheet but likely unused: no ROM touches
        // them and scc68070.sv calls them unused in the CD-i. Only match mode is
        // modelled; capture and event counting need the T1/T2 pins.
        if (((timer_tcr_ >> 4) & 3) == kTcrMatch && timer0_ == timer1_) timer_tsr_ |= kTsrMa1;
        if ((timer_tcr_ & 3) == kTcrMatch && timer0_ == timer2_) timer_tsr_ |= kTsrMa2;
    }

    // UART transmit-complete, latched only while the transmitter is
    // enabled (MAME tx_callback).
    if (uart_tx_period_cycles_ > 0) {
        uart_tx_cycle_accum_ += cpu_cycles;
        while (uart_tx_cycle_accum_ >= uart_tx_period_cycles_) {
            uart_tx_cycle_accum_ -= uart_tx_period_cycles_;
            if (((uart_ucr_ >> 2) & 3) == 1) {
                uart_tx_int_ = true;
            }
        }
    }
}

void Scc68070Periph::set_int1(bool asserted) { int1_asserted_ = asserted; }

int Scc68070Periph::current_ipl() const {
    int int1_level = int1_asserted_ ? ((lir_ >> 4) & 7) : 0;
    // A level while a TSR flag is set, cleared by a TSR write (scc68070.sv, OV0 only).
    int timer_level = (timer_tsr_ & kTsrFlags) ? (picr1_ & 7) : 0;
    int uart_tx_level = uart_tx_int_ ? (picr2_ & 7) : 0;
    int uart_rx_level = uart_rx_valid_ && uart_receiver_enabled() ? ((picr2_ >> 4) & 7) : 0;
    int in2_level = in2_ ? 2 : 0; // fixed level, per MAME's dedicated external pins
    int in4_level = in4_ ? 4 : 0; // external CDIC pin

    // No DMA completion source, unlike MAME/same_cdi (scc68070.sv IPL logic).
    return std::max({int1_level, timer_level, uart_tx_level, uart_rx_level, in2_level, in4_level});
}

int Scc68070Periph::iack(int level) {
    if (in2_ && level == 2) return -1; // dedicated external pin: autovector, nothing to clear (level-sensitive)
    if (uart_tx_int_ && (picr2_ & 7) == level) uart_tx_int_ = false;
    return 0x38 + level;
}

void Scc68070Periph::set_in2(bool asserted) { in2_ = asserted; }
void Scc68070Periph::set_in4(bool asserted) { in4_ = asserted; }

void Scc68070Periph::uart_receive(uint8_t v) {
    uart_rhr_ = v;
    uart_rx_valid_ = true;
}

std::string Scc68070Periph::drain_uart_output() { return std::exchange(uart_output_, {}); }

} // namespace mamecdi
} // namespace zcdi
