#pragma once
#include <cstdint>

namespace zcdi {
namespace dvc {

// VMPEG (DVC cartridge) register values.
namespace vmpeg {

// Register word indices (byte address 0xe00000 + 2 * index).
enum Register : uint32_t {
    kFmaCmd = 0x1800, // 0xe03000
    kFmaStatus = 0x1801, // 0xe03002
    kFmaFixed04 = 0x1802, // 0xe03004, purpose unknown, reads a constant
    kFmaFixed06 = 0x1803, // 0xe03006, same
    kFmaStreamW = 0x1804, // 0xe03008 (write: wanted stream)
    kFmaStreamR = 0x1805, // 0xe0300a (read: current stream, same reg here)
    kFmaIvec = 0x1806, // 0xe0300c
    kFmaFixed0e = 0x1807, // 0xe0300e, same
    kFmaDclkH = 0x1808, // 0xe03010
    kFmaDclkL = 0x1809, // 0xe03012
    kFmaAudioHdrH = 0x180a, // 0xe03014
    kFmaAudioHdrL = 0x180b, // 0xe03016
    kFmaRun = 0x180c, // 0xe03018
    kFmaIsr = 0x180d, // 0xe0301a
    kFmaIer = 0x180e, // 0xe0301c
    kFmaDspa = 0x1811, // 0xe03022
    kFmaDspd = 0x1812, // 0xe03024

    kFmvImageWidth = 0x2001, // 0xe04002
    kFmvImageHeight = 0x2002, // 0xe04004
    kFmvImageRt = 0x2003, // 0xe04006
    kFmvDecoderTimecodeHi = 0x2004, // 0xe04008
    kFmvDecoderTimecodeLo = 0x2005, // 0xe0400a
    kFmvDisplayW = 0x2029, // 0xe04052
    kFmvDisplayH = 0x202a, // 0xe04054
    kFmvPicRt = 0x202b, // 0xe04056, displayed picture
    kFmvDisplayTimecodeHi = 0x202c, // 0xe04058, fmvdrv MV_Info time code
    kFmvDisplayTimecodeLo = 0x202d, // 0xe0405a
    kFmvTempref = 0x202e, // 0xe0405c
    kFmvSysSts = 0x202f, // 0xe0405e
    kFmvIer = 0x2030, // 0xe04060
    kFmvIsr = 0x2031, // 0xe04062
    kFmvTcnt = 0x2032, // 0xe04064
    kFmvTrld = 0x2057, // 0xe040ae (write-only reload)
    kFmvVoff = 0x2036, // 0xe0406c
    kFmvHoff = 0x2037, // 0xe0406e
    kFmvVpix = 0x2038, // 0xe04070
    kFmvHpix = 0x2039, // 0xe04072
    kFmvScrposY = 0x203a, // 0xe04074
    kFmvScrposX = 0x203b, // 0xe04076
    kFmvDecwinH = 0x203c, // 0xe04078
    kFmvDecwinW = 0x203d, // 0xe0407a
    kFmvDecoffY = 0x203e, // 0xe0407c
    kFmvDecoffX = 0x203f, // 0xe0407e
    kFmvGenDecCmd = 0x2044, // 0xe04088
    kFmvGenVdiCmd = 0x2046, // 0xe0408c
    kFmvGenSyscr = 0x204c, // 0xe04098
    kFmvGenSyncDiff = 0x204e, // 0xe0409c
    kFmvGenDecDelay = 0x204f, // 0xe0409e
    kFmvDts = 0x2050, // 0xe040a0
    kFmvPicsInFifo = 0x2052, // 0xe040a4
    kFmvPicRate = 0x2054, // 0xe040a8
    kFmvDisplayRate = 0x2055, // 0xe040aa
    kFmvFrameRate = 0x2056, // 0xe040ac
    kFmvSyscmd = 0x2060, // 0xe040c0
    kFmvVidcmd = 0x2061, // 0xe040c2
    kFmvStreamNum = 0x2062, // 0xe040c4
    kFmvSysScr = 0x2063, // 0xe040c6
    kFmvXfer = 0x206f, // 0xe040de
    kFmvIvec = 0x206e, // 0xe040dc
    kFmvMmuBase = 0x2073, // 0xe040e6
};

// FMA_CMD.
enum FmaCommand : uint16_t {
    kFmaCmdStop = 0x0001,
    kFmaCmdStartDma = 0x8000, // to audio
};

// FMA status, low byte.
enum FmaStatus : uint8_t {
    kFmaStatusFinished = 0x01,
    kFmaStatusFrame = 0x04,
    kFmaStatusUnderflow = 0x08,
    kFmaStatusDecoding = 0x10,
};

// FMA_ISR and FMA_IER bits.
enum FmaInterruptBit : uint16_t {
    kFmaIntIsoEnd = 1u << 0,
    kFmaIntCsu = 1u << 1, // stream change
    kFmaIntFrame = 1u << 2, // frame header updated
    kFmaIntUnf = 1u << 3, // underflow
    kFmaIntDecoding = 1u << 4, // decoding started
    kFmaIntTim = 1u << 8,
};

// FMA DSP: DSPA 0/1 select mode and target; with these,
// four DSPA 7 writes set the volumes in dB.
constexpr uint8_t kDspModeVolume = 0x80; // DSPD written at DSPA 0
constexpr uint8_t kDspTargetVolume = 0x93; // DSPD written at DSPA 1
constexpr uint8_t kDspaVolume = 7;

// FMV_ISR and FMV_IER bits.
enum InterruptBit : uint16_t {
    kIntSeq = 1u << 0,
    kIntGop = 1u << 1,
    kIntPic = 1u << 2,
    kIntEod = 1u << 3,
    kIntRfb = 1u << 4,
    kIntNdat = 1u << 5,
    kIntOvf = 1u << 6,
    kIntDcl = 1u << 7,
    kIntTim = 1u << 8,
    kIntEsi = 1u << 9,
    kIntEii = 1u << 10,
    kIntVsync = 1u << 11,
    kIntPai = 1u << 12,
    kIntVcup = 1u << 13,
    kIntErdd = 1u << 14,
    kIntErdv = 1u << 15,
};

// GEN_DEC_CMD bits Play sets and Decoder Off clears (bit 5 = RUN_RYTHM).
constexpr uint16_t kDecCmdRunning = 0x0062;

// FMV_SYSCMD.
enum SysCmd : uint16_t {
    kSysCmdSlowMotion = 0x0007, // period x (n + 1)
    kSysCmdPlay = 0x0008,
    kSysCmdPause = 0x0010,
    kSysCmdContinue = 0x0020,
    kSysCmdStep = 0x0040,
    kSysCmdStop = 0x0080,
    kSysCmdClearFifo = 0x0100,
    kSysCmdDecoderOn = 0x1000,
    kSysCmdDecoderOff = 0x2000,
    kSysCmdStartDma = 0x8000, // to video
};

// FMV_VIDCMD.
enum VidCmd : uint16_t {
    kVidCmdScroll = 0x0004, // RegsUpd at the next vsync, not the next picture
    kVidCmdRegsUpd = 0x0008,
    kVidCmdHide = 0x0100,
    kVidCmdShow = 0x0200,
    kVidCmdShowOnChange = 0x0400,
};

constexpr uint16_t kSysScrSynchronous = 0x0004; // SYSSCR: display follows the DTS
constexpr uint16_t kSysStsFifoNotFull = 0x2000;
constexpr uint16_t kVdiTimestampUpdated = 0x4000; // GEN_VDI_CMD bit 14
// SYSSTS reports the input FIFO (64 KB) full above this many bytes.
constexpr uint32_t kFmvInputFifoFullBytes = 62000;

// GEN_DISPLAY_RATE.
enum DisplayRate : uint16_t {
    kDisplayRateNtsc = 0x05dc,
    kDisplayRatePal = 0x0708,
};

// MPEG RAM is enabled for good on the 64th register write.
constexpr int kMpegRamEnableWrites = 64;

} // namespace vmpeg

} // namespace dvc
} // namespace zcdi
