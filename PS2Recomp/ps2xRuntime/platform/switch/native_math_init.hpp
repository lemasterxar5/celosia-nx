#pragma once
namespace celosia::native {
class DataMemory;
class SemaphoreSet;
// Initialize the game's quaternion and quarter-wave lookup DATA using native
// libm, plus a real mutex protecting shared matrix-stack data. No VIF upload,
// vector processor reset or microcode execution occurs. The semaphore belongs
// to SemaphoreSet and is registered for the game's external-resource cleanup.
// Caller owns the execution gate; both arguments outlive all use of the data.
void initializeNativeMath(DataMemory &data,SemaphoreSet &semaphores);
}
