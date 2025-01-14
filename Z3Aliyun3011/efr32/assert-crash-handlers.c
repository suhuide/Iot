/***************************************************************************//**
 * @file
 * @brief EM3XX-specific Assert and Crash HAL functions
 *******************************************************************************
 * # License
 * <b>Copyright 2018 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/
#include PLATFORM_HEADER
#include "stack/include/ember.h"
#include "hal/hal.h"
#include "serial/com.h"
#include "serial/serial.h"
#include "hal/micro/cortexm3/diagnostic.h"
#include "hal/micro/cortexm3/efm32/mpu.h"

#include <kernel/include/os.h>


#ifdef RTOS
  #include "rtos/rtos.h"
  #define freeRTOS 1
#else  // RTOS
  #define freeRTOS 0
// extern uint32_t* xTaskGetCurrentTaskStackTop(void);
// extern uint32_t* xTaskGetCurrentTaskStackBottom(void);
#endif // RTOS

#if !defined(WDOG0)
#define WDOG0      WDOG
#define WDOG0_IRQn WDOG_IRQn
#endif

//------------------------------------------------------------------------------
// Preprocessor definitions

// Reserved instruction executed after a failed assert to cause a usage fault
#define ASSERT_USAGE_OPCODE 0xDE42

//------------------------------------------------------------------------------
// Forward Declarations

extern void emRadioSleep(void);

//------------------------------------------------------------------------------
// Functions

#if defined (__ICCARM__)
// Cause a usage fault by executing a special UNDEFINED instruction.
// The high byte (0xDE) is reserved to be undefined - the low byte (0x42)
// is arbitrary and distiguishes a failed assert from other usage faults.
// the fault handler with then decode this, grab the filename and linenumber
// parameters from R0 and R1 and save the information for display after
// a reset
static void halInternalAssertFault(PGM_P filename, int linenumber)
{
  asm ("DC16 0DE42h");
}
#endif

void halInternalAssertFailed(PGM_P filename, int linenumber)
{
 #ifndef PHY_PRO2PLUS
#if !defined (_SILICON_LABS_32B_SERIES_2) // emRadioSleep
  emRadioSleep();
#endif // !defined (_SILICON_LABS_32B_SERIES_2) // emRadioSleep
 #endif//PHY_PRO2PLUS
  halResetWatchdog();              // In case we're close to running out.
  INTERRUPTS_OFF();

  #if DEBUG_LEVEL >= BASIC_DEBUG
  emberDebugAssert(filename, linenumber);
  #endif

  #if !defined(EMBER_ASSERT_OUTPUT_DISABLED)
  (void) emberSerialGuaranteedPrintf(EMBER_ASSERT_SERIAL_PORT,
                                     "\r\n[ASSERT:%p:%d]\r\n",
                                     filename,
                                     linenumber);
  #endif

  #if defined (__ICCARM__)
  // With IAR, we can use the special fault mechanism to preserve more assert
  //  information for display after a crash
  halInternalAssertFault(filename, linenumber);
  #else
  // Other toolchains don't handle the inline assembly correctly, so
  // we just call the internal reset
  halResetInfo.crash.data.assertInfo.file = filename;
  halResetInfo.crash.data.assertInfo.line = linenumber;
  halInternalSysReset(RESET_CRASH_ASSERT);
  #endif
}

// Returns the bytes used in the main stack area.
static uint32_t halInternalGetMainStackBytesUsed(uint32_t *p)
{
  for (; p < (uint32_t *)_CSTACK_SEGMENT_END; p++) {
    if (*p != STACK_FILL_VALUE) {
      break;
    }
  }
  return (uint32_t)((uint8_t *)_CSTACK_SEGMENT_END - (uint8_t *)p);
}

// After the low-level fault handler (in faults.s79) has saved the processor
// registers (R0-R12, LR and both MSP an PSP), it calls halInternalCrashHandler
// to finish saving additional crash data. This function returns the reason for
// the crash to the low-level fault handler that then calls
// halInternalSystsemReset() to reset the processor.
//
// NOTE:
// This function should not use more than 16 words on the stack to avoid
// overwriting halResetInfo at the bottom of the stack segment.
// The 16 words include this function's return address, plus any stack
// used by functions called by this one. The stack size allowed is defined
// by the symbol CRASH_STACK_SIZE in faults.s79.
// As compiled by IAR V6.21.1, it now uses 8 words (1 for its return address,
// 6 for registers pushed onto the stack and 1 for the return address of
// halInternalGetMainStackBytesUsed().
//
uint16_t halInternalCrashHandler(void)
{
  uint32_t activeException;
  uint16_t reason = RESET_FAULT_UNKNOWN;
  HalCrashInfoType *c = &halResetInfo.crash;
  uint8_t i, j;
  uint32_t *sp, *s, *sEnd, *stackBottom, *stackTop;
  uint32_t data;

  c->icsr.word = SCB->ICSR;
  c->shcsr.word = SCB->SHCSR;
#if (__CORTEX_M >= 0x03)
  c->intActive.word[0] = NVIC->IABR[0];
  c->intActive.word[1] = NVIC->IABR[1];
#else
  // No IABR on Cortex-M0+
  uint32_t icsr_reg = (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) >= 16 ? (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) : 0;
  if (icsr_reg > 0) {
    uint64_t activeVector = 1 << (icsr_reg - 16);
    c->intActive.word[0] = (uint32_t) (activeVector & 0xFFFFFFFF);
    c->intActive.word[1] = (activeVector >> 32) & 0xFFFFFFFF;
  }
#endif

#if (__CORTEX_M >= 0x03)
  c->cfsr.word = SCB->CFSR;
  c->hfsr.word = SCB->HFSR;
  c->dfsr.word = SCB->DFSR;
  c->faultAddress = SCB->MMFAR;
  c->afsr.word = SCB->AFSR;
#endif

  // Examine B2 of the saved LR to know the stack in use when the fault occurred
  sp = (uint32_t *)((c->LR & 4) ? c->processSP : c->mainSP);
  sEnd = sp; // Keep a copy around for walking the stack later

  // Get the bottom of the stack since we allow stack resizing
  c->mainStackBottom = (uint32_t)halInternalGetCStackBottom();

  // If we're running FreeRTOS and this is a process stack then add
  // extra diagnostic information
  if ((freeRTOS != 0) && ((c->LR & 4U) != 0U)) {
    // FreeRTOS doesn't provide the diagnostic functions we need
    // so for now just lie to get some diagnostics
    // stackBottom = (uint32_t*)xTaskGetCurrentTaskStackBottom();
    // stackTop = (uint32_t*)xTaskGetCurrentTaskStackTop();
    stackBottom = sp;
    stackTop = sp + 8;
    c->processSPUsed = stackTop - sp;
  } else {
    #if defined(MICRIUMOS) && (1 == MICRIUMOS)
    stackBottom = (uint32_t*)(OSTCBCurPtr->StkBasePtr);
    stackTop = (uint32_t*)(OSTCBCurPtr->StkBasePtr + OSTCBCurPtr->StkSize);
    c->processSPUsed = (uint32_t*)(OSTCBCurPtr->StkBasePtr + OSTCBCurPtr->StkSize) - sp;
    #else
    stackBottom = (uint32_t*)c->mainStackBottom;
    stackTop = (uint32_t*)_CSTACK_SEGMENT_END;
    c->processSPUsed = 0;   // process stack not in use
    #endif //MICRIUMOS
  }

  // If the stack pointer is valid, read and save the stacked PC and xPSR
  if ((sp >= stackBottom)
      && (sp <= (stackTop - 8))) {
    sp += 6; // Skip over R0,R1,R2,R3,R12,LR
    c->PC = *sp++;
    c->xPSR.word = *sp++;

    // See if fault was due to a failed assert. This is indicated by
    // a usage fault caused by executing a reserved instruction.
    if ( c->icsr.bits.VECTACTIVE == USAGE_FAULT_VECTOR_INDEX
         && ((void *)c->PC >= (void*)_TEXT_SEGMENT_BEGIN)
         && ((void *)c->PC < (void*)_TEXT_SEGMENT_END)
         && *(uint16_t *)(c->PC) == ASSERT_USAGE_OPCODE ) {
      // Copy halInternalAssertFailed() arguments into data member specific
      // to asserts.
      c->data.assertInfo.file = (const char *)c->R0;
      c->data.assertInfo.line = c->R1;
#ifdef PUSH_REGS_BEFORE_ASSERT
      // Just before calling halInternalAssertFailed(), R0, R1, R2 and LR were
      // pushed onto the stack - copy these values into the crash data struct.
      c->R0 = *sp++;
      c->R1 = *sp++;
      c->R2 = *sp++;
      c->LR = *sp++;
#endif
      reason = RESET_CRASH_ASSERT;
    }
    // If a bad stack pointer, PC and xPSR to 0 to indicate they are not known.
  } else {
    c->PC = 0;
    c->xPSR.word = 0;
    sEnd = stackBottom;
  }

  // Fault handler has already started filling in halResetInfo{}
  // prior to calling this routine, so want to make sure _not_
  // to include halResetInfo in the stack assessment when crashing
  // to avoid a self-fulfilling prophesy of a full stack!  BugzId:13403
  uint32_t safeStackBottom = c->mainStackBottom;
  if (safeStackBottom < (uint32_t) _RESETINFO_SEGMENT_END) {
    safeStackBottom = (uint32_t) _RESETINFO_SEGMENT_END;
  }
  c->mainSPUsed = halInternalGetMainStackBytesUsed((uint32_t*)safeStackBottom);

  for (i = 0; i < NUM_RETURNS; i++) {
    c->returns[i] = 0;
  }

  // Search the stack downward for probable return addresses. A probable
  // return address is a value in the CODE segment that also has bit 0 set
  // (since we're in Thumb mode).
  i = 0U;
  s = stackTop;
  while (s > sEnd) {
    data = *(--s);
    if (((void *)data >= (void*)_TEXT_SEGMENT_BEGIN)
        && ((void *)data < (void*)_TEXT_SEGMENT_END)
        && (data & 1)) {
      // Only record the first occurrence of a return - other copies could
      // have been in registers that then were pushed.
      for (j = 0; j < NUM_RETURNS; j++) {
        if (c->returns[j] == data) {
          break;
        }
      }
      if (j != NUM_RETURNS) {
        continue;
      }
      // Save the return in the returns array managed as a circular buffer.
      // This keeps only the last NUM_RETURNS in the event that there are more.
      i = i ? i - 1 : NUM_RETURNS - 1;
      c->returns[i] = data;
    }
  }
  // Shuffle the returns array so returns[0] has last probable return found.
  // If there were fewer than NUM_RETURNS, unused entries will contain zero.
  while (i-- != 0U) {
    data = c->returns[0];
    for (j = 0; j < NUM_RETURNS - 1; j++ ) {
      c->returns[j] = c->returns[j + 1];
    }
    c->returns[NUM_RETURNS - 1] = data;
  }

  // Read the highest priority active exception to get reason for fault
  activeException = c->icsr.bits.VECTACTIVE;
  switch (activeException) {
    #if defined(WDOG_IF_WARN) && !defined(BOOTLOADER)
    case IRQ_TO_VECTOR_NUMBER(WDOG0_IRQn):
      if (WDOG0->IF & WDOG_IF_WARN) {
        reason = RESET_WATCHDOG_CAUGHT;
      }
      break;
    #endif
    // case NMI_VECTOR_INDEX
    //   if (INT_NMIFLAG_REG & INT_NMICLK24M_MASK) {
    //     reason = RESET_FATAL_CRYSTAL;
    //   }
    //   break;
    case HARD_FAULT_VECTOR_INDEX:
      reason = RESET_FAULT_HARD;
      break;
    case MEMORY_FAULT_VECTOR_INDEX:
      reason = RESET_FAULT_MEM;
      break;
    case BUS_FAULT_VECTOR_INDEX:
      reason = RESET_FAULT_BUS;
      break;
    case USAGE_FAULT_VECTOR_INDEX:
      // make sure we didn't already identify the usage fault as an assert
      if (reason == RESET_FAULT_UNKNOWN) {
        reason = RESET_FAULT_USAGE;
      }
      break;
    case DEBUG_MONITOR_VECTOR_INDEX:
      reason = RESET_FAULT_DBGMON;
      break;
    default:
      if (activeException && (activeException < VECTOR_TABLE_LENGTH)) {
        reason = RESET_FAULT_BADVECTOR;
      }
      break;
  }
  return reason;
}
