#pragma once

#include <furi.h>
#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SignalTx SignalTx;

typedef enum {
    SignalTypeUnknown = 0,
    SignalTypeSubGhz,
    SignalTypeInfrared,
    SignalTypeRfid,
    SignalTypeIButton,
    SignalTypeNfc,
} SignalType;

typedef enum {
    SignalTxOk,
    SignalTxErrorLoad, // could not open / parse the file
    SignalTxErrorUnsupported, // file type not handled
    SignalTxErrorTx, // nothing was transmitted
    SignalTxStopped, // aborted via the stop flag
} SignalTxResult;

/** Short info about the signal that was just played, for UI display. */
typedef struct {
    char label[48]; // e.g. "433.92 Princeton", "3 signal(s)", "EM4100"
    uint32_t frequency; // Sub-GHz only, 0 otherwise
} SignalTxInfo;

/** Allocate the multi-format transmit engine. */
SignalTx* signal_tx_alloc(void);

/** Free the transmit engine. */
void signal_tx_free(SignalTx* instance);

/** Map a file name to its signal type by extension (case-insensitive). */
SignalType signal_tx_type_from_name(const char* name);

/** Short human-readable name for a signal type ("SubGHz", "IR", ...). */
const char* signal_tx_type_str(SignalType type);

/** True for types that emulate for a dwell time rather than firing a one-shot. */
bool signal_tx_is_emulation(SignalType type);

/** Power up shared hardware for a run (currently the Sub-GHz radio). */
void signal_tx_session_begin(SignalTx* instance);

/** Release hardware after a run. */
void signal_tx_session_end(SignalTx* instance);

/**
 * Play a single signal file once. One-shot types (Sub-GHz, IR) transmit a
 * single pass; emulation types (RFID, iButton, NFC) emulate for dwell_ms.
 * @param instance  engine instance
 * @param type      signal type (from signal_tx_type_from_name)
 * @param path      full path to the file
 * @param dwell_ms  emulation duration for emulation types (ignored otherwise)
 * @param info      optional, filled with display metadata (may be NULL)
 * @param stop      optional abort flag, polled during playback (may be NULL)
 */
SignalTxResult signal_tx_play(
    SignalTx* instance,
    SignalType type,
    const char* path,
    uint32_t dwell_ms,
    SignalTxInfo* info,
    volatile bool* stop);

/** Human-readable name for a result code. */
const char* signal_tx_result_str(SignalTxResult result);

#ifdef __cplusplus
}
#endif
