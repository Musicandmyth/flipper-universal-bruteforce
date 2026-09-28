#include "signal_tx.h"
#include "subghz_tx.h"

#include <infrared.h>
#include <infrared_transmit.h>
#include <lib/flipper_format/flipper_format.h>

#include <lfrfid/lfrfid_worker.h>
#include <lfrfid/lfrfid_dict_file.h>
#include <lfrfid/protocols/lfrfid_protocols.h>
#include <toolbox/protocols/protocol_dict.h>

#include <ibutton/ibutton_protocols.h>
#include <ibutton/ibutton_key.h>

#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_listener.h>

#include <string.h>
#include <stdlib.h>

#define TAG "SignalTx"

struct SignalTx {
    Storage* storage;
    SubGhzBfTx* subghz;
};

// ---------------------------------------------------------------------------
// Type detection
// ---------------------------------------------------------------------------

// Case-insensitive check that `name` ends with the (lowercase) `ext`.
static bool signal_tx_ends_with(const char* name, const char* ext) {
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    if(nl < el) return false;
    const char* p = name + (nl - el);
    for(size_t i = 0; i < el; i++) {
        char c = p[i];
        if(c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if(c != ext[i]) return false;
    }
    return true;
}

SignalType signal_tx_type_from_name(const char* name) {
    if(signal_tx_ends_with(name, ".sub")) return SignalTypeSubGhz;
    if(signal_tx_ends_with(name, ".ir")) return SignalTypeInfrared;
    if(signal_tx_ends_with(name, ".rfid")) return SignalTypeRfid;
    if(signal_tx_ends_with(name, ".ibtn")) return SignalTypeIButton;
    if(signal_tx_ends_with(name, ".nfc")) return SignalTypeNfc;
    return SignalTypeUnknown;
}

const char* signal_tx_type_str(SignalType type) {
    switch(type) {
    case SignalTypeSubGhz:
        return "SubGHz";
    case SignalTypeInfrared:
        return "IR";
    case SignalTypeRfid:
        return "RFID";
    case SignalTypeIButton:
        return "iBtn";
    case SignalTypeNfc:
        return "NFC";
    default:
        return "?";
    }
}

bool signal_tx_is_emulation(SignalType type) {
    return type == SignalTypeRfid || type == SignalTypeIButton || type == SignalTypeNfc;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

SignalTx* signal_tx_alloc(void) {
    SignalTx* instance = malloc(sizeof(SignalTx));
    instance->storage = furi_record_open(RECORD_STORAGE);
    instance->subghz = subghz_tx_alloc();
    return instance;
}

void signal_tx_free(SignalTx* instance) {
    furi_assert(instance);
    subghz_tx_free(instance->subghz);
    furi_record_close(RECORD_STORAGE);
    free(instance);
}

void signal_tx_session_begin(SignalTx* instance) {
    furi_assert(instance);
    subghz_tx_session_begin(instance->subghz);
}

void signal_tx_session_end(SignalTx* instance) {
    furi_assert(instance);
    subghz_tx_session_end(instance->subghz);
}

// Sleep for `ms`, returning early (Stopped) if the abort flag is raised.
static SignalTxResult signal_tx_dwell(uint32_t ms, volatile bool* stop) {
    uint32_t waited = 0;
    while(waited < ms) {
        if(stop && *stop) return SignalTxStopped;
        furi_delay_ms(10);
        waited += 10;
    }
    return SignalTxOk;
}

// ---------------------------------------------------------------------------
// Sub-GHz (delegates to subghz_tx)
// ---------------------------------------------------------------------------

static SignalTxResult signal_tx_play_subghz(
    SignalTx* instance,
    const char* path,
    SignalTxInfo* info,
    volatile bool* stop) {
    SubGhzTxFileInfo si = {0};
    SubGhzTxResult r = subghz_tx_transmit_file(instance->subghz, path, &si, stop);

    if(info) {
        info->frequency = si.frequency;
        snprintf(
            info->label,
            sizeof(info->label),
            "%lu.%02lu %s",
            (unsigned long)(si.frequency / 1000000),
            (unsigned long)((si.frequency % 1000000) / 10000),
            si.protocol);
    }

    switch(r) {
    case SubGhzTxResultOk:
        return SignalTxOk;
    case SubGhzTxResultStopped:
        return SignalTxStopped;
    case SubGhzTxResultErrorOpen:
    case SubGhzTxResultErrorHeader:
        return SignalTxErrorLoad;
    default:
        return SignalTxErrorTx;
    }
}

// ---------------------------------------------------------------------------
// Infrared (.ir): send every signal in the file
// ---------------------------------------------------------------------------

static void signal_tx_ir_send_parsed(FlipperFormat* ff, uint32_t* sent) {
    FuriString* proto = furi_string_alloc();
    uint8_t addr[4] = {0};
    uint8_t cmd[4] = {0};
    if(flipper_format_read_string(ff, "protocol", proto) &&
       flipper_format_read_hex(ff, "address", addr, sizeof(addr)) &&
       flipper_format_read_hex(ff, "command", cmd, sizeof(cmd))) {
        InfraredMessage msg;
        msg.protocol = infrared_get_protocol_by_name(furi_string_get_cstr(proto));
        msg.address = (uint32_t)addr[0] | ((uint32_t)addr[1] << 8) | ((uint32_t)addr[2] << 16) |
                      ((uint32_t)addr[3] << 24);
        msg.command = (uint32_t)cmd[0] | ((uint32_t)cmd[1] << 8) | ((uint32_t)cmd[2] << 16) |
                      ((uint32_t)cmd[3] << 24);
        msg.repeat = false;
        if(infrared_is_protocol_valid(msg.protocol)) {
            infrared_send(&msg, 1);
            (*sent)++;
        }
    }
    furi_string_free(proto);
}

static void signal_tx_ir_send_raw(FlipperFormat* ff, uint32_t* sent) {
    uint32_t frequency = INFRARED_COMMON_CARRIER_FREQUENCY;
    float duty_cycle = INFRARED_COMMON_DUTY_CYCLE;
    flipper_format_read_uint32(ff, "frequency", &frequency, 1);
    flipper_format_read_float(ff, "duty_cycle", &duty_cycle, 1);

    uint32_t count = 0;
    if(flipper_format_get_value_count(ff, "data", &count) && count > 0) {
        uint32_t* timings = malloc(count * sizeof(uint32_t));
        if(timings && flipper_format_read_uint32(ff, "data", timings, count)) {
            infrared_send_raw_ext(timings, count, true, frequency, duty_cycle);
            (*sent)++;
        }
        if(timings) free(timings);
    }
}

static SignalTxResult signal_tx_play_ir(
    SignalTx* instance,
    const char* path,
    SignalTxInfo* info,
    volatile bool* stop) {
    FlipperFormat* ff = flipper_format_file_alloc(instance->storage);
    FuriString* header = furi_string_alloc();
    FuriString* name = furi_string_alloc();
    FuriString* type = furi_string_alloc();
    uint32_t sent = 0;
    SignalTxResult result = SignalTxOk;

    do {
        if(!flipper_format_file_open_existing(ff, path)) {
            result = SignalTxErrorLoad;
            break;
        }
        uint32_t version = 0;
        if(!flipper_format_read_header(ff, header, &version)) {
            result = SignalTxErrorLoad;
            break;
        }
        // Each signal is a "name" + "type" block, in file order.
        while(flipper_format_read_string(ff, "name", name)) {
            if(stop && *stop) {
                result = SignalTxStopped;
                break;
            }
            if(!flipper_format_read_string(ff, "type", type)) break;
            if(furi_string_cmp_str(type, "parsed") == 0) {
                signal_tx_ir_send_parsed(ff, &sent);
            } else if(furi_string_cmp_str(type, "raw") == 0) {
                signal_tx_ir_send_raw(ff, &sent);
            }
        }
    } while(false);

    if(info) {
        info->frequency = 0;
        if(result == SignalTxStopped) {
            snprintf(info->label, sizeof(info->label), "IR x%lu", (unsigned long)sent);
        } else {
            snprintf(info->label, sizeof(info->label), "%lu signal(s)", (unsigned long)sent);
        }
    }
    if(result == SignalTxOk && sent == 0) result = SignalTxErrorTx;

    flipper_format_free(ff);
    furi_string_free(header);
    furi_string_free(name);
    furi_string_free(type);
    return result;
}

// ---------------------------------------------------------------------------
// LF RFID (.rfid): emulate for a dwell time
// ---------------------------------------------------------------------------

static SignalTxResult signal_tx_play_rfid(
    const char* path,
    uint32_t dwell_ms,
    SignalTxInfo* info,
    volatile bool* stop) {
    ProtocolDict* dict = protocol_dict_alloc(lfrfid_protocols, LFRFIDProtocolMax);
    ProtocolId protocol = lfrfid_dict_file_load(dict, path);
    SignalTxResult result = SignalTxOk;

    if(protocol == PROTOCOL_NO) {
        result = SignalTxErrorLoad;
        if(info) snprintf(info->label, sizeof(info->label), "load err");
    } else {
        if(info) {
            info->frequency = 0;
            const char* name = protocol_dict_get_name(dict, (size_t)protocol);
            snprintf(info->label, sizeof(info->label), "%s", name ? name : "RFID");
        }
        LFRFIDWorker* worker = lfrfid_worker_alloc(dict);
        lfrfid_worker_start_thread(worker);
        lfrfid_worker_emulate_start(worker, (LFRFIDProtocol)protocol);
        result = signal_tx_dwell(dwell_ms, stop);
        lfrfid_worker_stop(worker);
        lfrfid_worker_stop_thread(worker);
        lfrfid_worker_free(worker);
    }

    protocol_dict_free(dict);
    return result;
}

// ---------------------------------------------------------------------------
// iButton (.ibtn): emulate for a dwell time
// ---------------------------------------------------------------------------

static SignalTxResult signal_tx_play_ibutton(
    const char* path,
    uint32_t dwell_ms,
    SignalTxInfo* info,
    volatile bool* stop) {
    iButtonProtocols* protocols = ibutton_protocols_alloc();
    iButtonKey* key = ibutton_key_alloc(ibutton_protocols_get_max_data_size(protocols));
    SignalTxResult result = SignalTxOk;

    if(!ibutton_protocols_load(protocols, key, path) ||
       !ibutton_protocols_is_valid(protocols, key)) {
        result = SignalTxErrorLoad;
        if(info) snprintf(info->label, sizeof(info->label), "load err");
    } else {
        if(info) {
            info->frequency = 0;
            const char* name =
                ibutton_protocols_get_name(protocols, ibutton_key_get_protocol_id(key));
            snprintf(info->label, sizeof(info->label), "%s", name ? name : "iButton");
        }
        ibutton_protocols_emulate_start(protocols, key);
        result = signal_tx_dwell(dwell_ms, stop);
        ibutton_protocols_emulate_stop(protocols, key);
    }

    ibutton_key_free(key);
    ibutton_protocols_free(protocols);
    return result;
}

// ---------------------------------------------------------------------------
// NFC (.nfc): emulate for a dwell time
// ---------------------------------------------------------------------------

static NfcCommand signal_tx_nfc_callback(NfcGenericEvent event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return NfcCommandContinue;
}

static SignalTxResult signal_tx_play_nfc(
    const char* path,
    uint32_t dwell_ms,
    SignalTxInfo* info,
    volatile bool* stop) {
    NfcDevice* device = nfc_device_alloc();
    SignalTxResult result = SignalTxOk;

    if(!nfc_device_load(device, path)) {
        result = SignalTxErrorLoad;
        if(info) snprintf(info->label, sizeof(info->label), "load err");
    } else {
        NfcProtocol protocol = nfc_device_get_protocol(device);
        const NfcDeviceData* data = nfc_device_get_data(device, protocol);
        if(info) {
            info->frequency = 0;
            const char* name = nfc_device_get_protocol_name(protocol);
            snprintf(info->label, sizeof(info->label), "%s", name ? name : "NFC");
        }
        Nfc* nfc = nfc_alloc();
        NfcListener* listener = nfc_listener_alloc(nfc, protocol, data);
        nfc_listener_start(listener, signal_tx_nfc_callback, NULL);
        result = signal_tx_dwell(dwell_ms, stop);
        nfc_listener_stop(listener);
        nfc_listener_free(listener);
        nfc_free(nfc);
    }

    nfc_device_free(device);
    return result;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

SignalTxResult signal_tx_play(
    SignalTx* instance,
    SignalType type,
    const char* path,
    uint32_t dwell_ms,
    SignalTxInfo* info,
    volatile bool* stop) {
    furi_assert(instance);
    if(info) {
        info->label[0] = '\0';
        info->frequency = 0;
    }

    switch(type) {
    case SignalTypeSubGhz:
        return signal_tx_play_subghz(instance, path, info, stop);
    case SignalTypeInfrared:
        return signal_tx_play_ir(instance, path, info, stop);
    case SignalTypeRfid:
        return signal_tx_play_rfid(path, dwell_ms, info, stop);
    case SignalTypeIButton:
        return signal_tx_play_ibutton(path, dwell_ms, info, stop);
    case SignalTypeNfc:
        return signal_tx_play_nfc(path, dwell_ms, info, stop);
    default:
        return SignalTxErrorUnsupported;
    }
}

const char* signal_tx_result_str(SignalTxResult result) {
    switch(result) {
    case SignalTxOk:
        return "OK";
    case SignalTxErrorLoad:
        return "Load err";
    case SignalTxErrorUnsupported:
        return "Unsupported";
    case SignalTxErrorTx:
        return "TX err";
    case SignalTxStopped:
        return "Stopped";
    default:
        return "?";
    }
}
