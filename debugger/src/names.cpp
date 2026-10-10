#include "names.h"

#include <cstdio>

namespace {

struct Os9Call {
    uint16_t code;
    const char* name;
};

constexpr Os9Call kOs9Calls[] = {
    {0x00, "F$Link"},   {0x01, "F$Load"},    {0x02, "F$UnLink"},  {0x03, "F$Fork"},   {0x04, "F$Wait"},
    {0x05, "F$Chain"},  {0x06, "F$Exit"},    {0x07, "F$Mem"},     {0x08, "F$Send"},   {0x09, "F$Icpt"},
    {0x0a, "F$Sleep"},  {0x0b, "F$SSpd"},    {0x0c, "F$ID"},      {0x0d, "F$SPrior"}, {0x0e, "F$STrap"},
    {0x0f, "F$PErr"},   {0x10, "F$PrsNam"},  {0x11, "F$CmpNam"},  {0x12, "F$SchBit"}, {0x13, "F$AllBit"},
    {0x14, "F$DelBit"}, {0x15, "F$Time"},    {0x16, "F$STime"},   {0x17, "F$CRC"},    {0x18, "F$GPrDsc"},
    {0x19, "F$GBlkMp"}, {0x1a, "F$GModDr"},  {0x1b, "F$CpyMem"},  {0x1c, "F$SUser"},  {0x1d, "F$UnLoad"},
    {0x1e, "F$RTE"},    {0x1f, "F$GPrDBT"},  {0x20, "F$Julian"},  {0x21, "F$TLink"},  {0x22, "F$DFork"},
    {0x23, "F$DExec"},  {0x24, "F$DExit"},   {0x25, "F$DatMod"},  {0x26, "F$SetCRC"}, {0x27, "F$SetSys"},
    {0x28, "F$SRqMem"}, {0x29, "F$SRtMem"},  {0x2a, "F$IRQ"},     {0x2b, "F$IOQu"},   {0x2c, "F$AProc"},
    {0x2d, "F$NProc"},  {0x2e, "F$VModul"},  {0x2f, "F$FindPD"},  {0x30, "F$AllPD"},  {0x31, "F$RetPD"},
    {0x32, "F$SSvc"},   {0x33, "F$IODel"},   {0x37, "F$GProcP"},  {0x38, "F$Move"},   {0x39, "F$AllRAM"},
    {0x3a, "F$Permit"}, {0x3b, "F$Protect"}, {0x3f, "F$AllTsk"},  {0x40, "F$DelTsk"}, {0x4b, "F$AllPrc"},
    {0x4c, "F$DelPrc"}, {0x4e, "F$FModul"},  {0x52, "F$SysDbg"},  {0x53, "F$Event"},  {0x54, "F$Gregor"},
    {0x55, "F$SysID"},  {0x56, "F$Alarm"},   {0x57, "F$SigMask"}, {0x58, "F$ChkMem"}, {0x59, "F$UAcct"},
    {0x5a, "F$CCtl"},   {0x5b, "F$GSPUMp"},  {0x5c, "F$SRqCMem"}, {0x5d, "F$POSK"},   {0x5e, "F$Panic"},
    {0x5f, "F$MBuf"},   {0x60, "F$Trans"},   {0x80, "I$Attach"},  {0x81, "I$Detach"}, {0x82, "I$Dup"},
    {0x83, "I$Create"}, {0x84, "I$Open"},    {0x85, "I$MakDir"},  {0x86, "I$ChgDir"}, {0x87, "I$Delete"},
    {0x88, "I$Seek"},   {0x89, "I$Read"},    {0x8a, "I$Write"},   {0x8b, "I$ReadLn"}, {0x8c, "I$WritLn"},
    {0x8d, "I$GetStt"}, {0x8e, "I$SetStt"},  {0x8f, "I$Close"},
};

} // namespace

const char* os9_call_name(uint16_t code) {
    for (const Os9Call& call : kOs9Calls) {
        if (call.code == code) return call.name;
    }
    return nullptr;
}

std::string vector_name(uint8_t v) {
    switch (v) {
        case 2: return "bus error";
        case 3: return "address error";
        case 4: return "illegal instruction";
        case 5: return "zero divide";
        case 6: return "CHK";
        case 7: return "TRAPV";
        case 8: return "privilege violation";
        case 9: return "trace";
        case 10: return "line A";
        case 11: return "line F";
        case 15: return "uninitialised IRQ";
        case 24: return "spurious IRQ";
        default: break;
    }
    char text[32]{};
    if (v >= 25 && v <= 31) {
        std::snprintf(text, sizeof(text), "IRQ %d autovector", v - 24);
    } else if (v >= 32 && v <= 47) {
        std::snprintf(text, sizeof(text), "TRAP #%d", v - 32);
    } else if (v >= 57 && v <= 63) {
        std::snprintf(text, sizeof(text), "on-chip IRQ %d", v - 56);
    } else {
        std::snprintf(text, sizeof(text), "vector %d", v);
    }
    return text;
}
