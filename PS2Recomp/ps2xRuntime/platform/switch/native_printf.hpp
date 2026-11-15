#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
struct R5900Context;
namespace celosia::native {
class DataMemory;
inline constexpr size_t PrintfFormatLimit=1024;
inline constexpr size_t PrintfStringLimit=4096;
inline constexpr size_t PrintfOutputLimit=16384;
// The sink receives a fully validated rendered string exactly once. Return the
// entire byte count or a negative error. Default uses fwrite to native stdout.
using PrintSink=std::function<int(std::string_view)>;
// EE integer slots r4..r11, then SP+16 in four-byte steps, as the original
// printf provider. Reentrant printf has two fixed arguments instead of one.
// Supports 32-bit integers, narrow chars/strings and numeric data pointers;
// floating/64-bit/wide/positional/%n formats are rejected, never guessed.
// Calls read ordinary data only; caller coordinates concurrent data mutation.
int32_t nativePrintf(DataMemory &memory,R5900Context &context,bool reentrant=false,PrintSink sink={});
// sprintf fixed arguments: destination r4, format r5. vsprintf adds r6
// pointing to eight-byte SCUS argument slots; integer values use their low32.
// Validates all input and the entire output including NUL before writing.
int32_t nativeSprintf(DataMemory &memory,R5900Context &context,bool vaList=false);
int32_t nativeWriteText(DataMemory &memory,uint32_t address,PrintSink sink={});
}
