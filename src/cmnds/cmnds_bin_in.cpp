// SPDX-FileCopyrightText: 2021 Sebastian Serewa <neos32.project@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

#include "my_common.h"

#if 1 == N32_CFG_BIN_IN_ENABLED

static debug_level_t uDebugLevel = DEBUG_WARN;
static bool bModuleInitialised = false;

#define BIN_MAX (0xFF)
#define BIN_IN_CHECK_INTERVAL_IN_SECS (1)
#define BIN_IN_LETTER 'I'

static bool bin_in_getPinFromChannelNum(u8 i_LogicalChannelNum, u8& o_PhysicalPinNumber) {

    if (i_LogicalChannelNum < BIN_IN_LINE_DH_COUNT) {
        o_PhysicalPinNumber = BIN_IN_LINE_DH_FIRST + i_LogicalChannelNum;
        return true;
    }
    else if (i_LogicalChannelNum < BIN_IN_NUM_OF_AVAIL_CHANNELS) {
        // TODO: why DL must always be after DH even if  BIN_IN_LINE_DL_FIRST is defined?
        o_PhysicalPinNumber = BIN_IN_LINE_DL_FIRST + i_LogicalChannelNum - BIN_IN_LINE_DH_COUNT;
        return true;
    }

    THROW_ERROR();
    return false; // error, means function failed to execute command properly or there is an error in logic
}

static volatile u8 portBstatus = 0;
static void pin_change() {
    portBstatus = PINB;
}

/// @brief Sets up a logical channel
/// @param i_LogChannel 
/// @param i_PinType input channel type. If not speficied, it's set to BIN_IN_PIN_TYPE_DH
/// @return false in case of error
static bool bin_in_SetupChannel(u8 i_LogChannel, binn_in_pin_type_t i_PinType = BIN_IN_PIN_TYPE_DH) {
    u8 uPhysicalPinNumber;
    int irq = -1;

    // a conversion from the logical (relative number) to the physical pin
    if (true == bin_in_getPinFromChannelNum(i_LogChannel, uPhysicalPinNumber)) {
        switch (i_PinType)
        {
        case BIN_IN_PIN_TYPE_DL:
            pinMode(uPhysicalPinNumber, INPUT);  // it's essential to have pulldown resistor connected
            break;

        case BIN_IN_PIN_TYPE_DH:
            pinMode(uPhysicalPinNumber, INPUT_PULLUP);
            break;

        case BIN_IN_LOGICAL_LOW_TRIGGERS_INTERRUPT:
            pinMode(uPhysicalPinNumber, INPUT_PULLUP);
            irq = digitalPinToInterrupt(uPhysicalPinNumber);
            if (NOT_AN_INTERRUPT == irq)
                goto ERROR_IN_PARAM;
            attachInterrupt(irq, pin_change, LOW);
            break;

        case BIN_IN_LOGICAL_CHANGE_TRIGGERS_INTERRUPT:
            pinMode(uPhysicalPinNumber, INPUT_PULLUP);
            irq = digitalPinToInterrupt(uPhysicalPinNumber);
            if (NOT_AN_INTERRUPT == irq)
                goto ERROR_IN_PARAM;
            attachInterrupt(irq, pin_change, CHANGE);
            break;

        case BIN_IN_FALLING_EDGE_TRIGGERS_INTERRUPT:
            pinMode(uPhysicalPinNumber, INPUT_PULLUP);
            irq = digitalPinToInterrupt(uPhysicalPinNumber);
            if (NOT_AN_INTERRUPT == irq)
                goto ERROR_IN_PARAM;
            attachInterrupt(irq, pin_change, FALLING);
            break;

        case BIN_IN_RISING_EDGE_TRIGGERS_INTERRUPT:
            pinMode(uPhysicalPinNumber, INPUT); // it's essential to have pulldown resistor connected
            irq = digitalPinToInterrupt(uPhysicalPinNumber);
            if (NOT_AN_INTERRUPT == irq)
                goto ERROR_IN_PARAM;
            attachInterrupt(irq, pin_change, RISING);
            break;

        default:
            goto ERROR_IN_PARAM;
        }


        return(true);
    }

ERROR_IN_PARAM:

    THROW_ERROR();

    return false; // error
}

static u16 readMask(void) {
    u16 ret = 0;
    u8 PhysicalPinNum;

    _FOR(i, 0, BIN_IN_NUM_OF_AVAIL_CHANNELS) {
        bin_in_getPinFromChannelNum(i, PhysicalPinNum);
        if (HIGH == digitalRead(PhysicalPinNum))
            ret += 1 << i;
    }

    return(ret);
}

static u16 prev_mask = 0;
static bool bin_in_UpdateStatus(bool i_bForced = false) {
    u16 current_mask = readMask();
    bool bState;

    if (current_mask != prev_mask || true == i_bForced) {
        u16 diffs = current_mask ^ prev_mask;

        String strCurrent, strDiff, strOut, str;
        bool bHasChanged;
        _FOR(i, 0, BIN_IN_NUM_OF_AVAIL_CHANNELS) {
            bHasChanged = (0x0 != ((1 << i) & diffs));

            if (true == bHasChanged) {
                // first we have to change states
                strDiff += F("1");

                String stringPath(MQTT_SENSORS_BIN_IN);
                stringPath += i;

                bState = 0x0 != ((1 << i) & current_mask);

#if 1 == N32_CFG_QUICK_ACTIONS_ENABLED
                // thne, is there a QA registered? If yes, let's trigger a command.
                u8 slot_num;
                if (true == QA_isStateTracked(BIN_IN_LETTER, i, bState, slot_num))
                    QA_ExecuteCommand(slot_num); // in e2prom we may have only validated data, so no extra command checking
                else {
                    IF_DEB_L() {
                        String str(F("BIN_IN: State not tracked, ignoring"));
                        MSG_Publish_Debug(str.c_str());
                    }
                }
#endif // N32_CFG_QUICK_ACTIONS_ENABLED

                if (true == bState)
                    str = F("OPENED");
                else
                    str = F("CLOSED");

                // and finally publish state to the broker
                MSG_Publish(stringPath.c_str(), str.c_str());
            }
            else
                strDiff += F("0");

            if (true == bHasChanged)
                strCurrent += F("1");
            else
                strCurrent += F("0");
        }

        strOut = F("Current: ");
        strOut += strCurrent;
        strOut += F(", Diff: ");
        strOut += strDiff;
        String stringPath(MQTT_SENSORS_BIN_IN);
        stringPath += F("state");
        MSG_Publish(stringPath.c_str(), strOut.c_str());

        prev_mask = current_mask;
    }

    return(true);
}

static void bin_in_AlarmFun(void) {
    bin_in_UpdateStatus();
}

void BIN_IN_ModuleInit(void) {

    // sanity check first
    if (true == bModuleInitialised) {
        IF_DEB_W() {
            String str(F("BIN IN: module initialized twice!"));
            DEBLN(str);
        }

        THROW_ERROR();
        return;
    }

    u8 uLogChannel;
    if (BIN_IN_LINE_DH_COUNT > 0) {
        // DH is always first, so offset is "0" - see the last parameter
        PIN_RegisterPins(bin_in_getPinFromChannelNum, BIN_IN_LINE_DH_COUNT, F("BIN_IN_DH"), 0);
        for (uLogChannel = 0; uLogChannel < BIN_IN_LINE_DH_COUNT; uLogChannel++)
            bin_in_SetupChannel(uLogChannel, BIN_IN_PIN_TYPE_DH);
    }

    if (BIN_IN_LINE_DL_COUNT > 0) {
        // DL is always second, so offset is BIN_OUT_LINE_DH_COUNT - see the last parameter
        PIN_RegisterPins(bin_in_getPinFromChannelNum, BIN_IN_LINE_DL_COUNT, F("BIN_IN_DL"), BIN_IN_LINE_DH_COUNT);
        for (uLogChannel = BIN_IN_LINE_DH_COUNT;
            uLogChannel < BIN_IN_NUM_OF_AVAIL_CHANNELS; uLogChannel++)
            bin_in_SetupChannel(uLogChannel, BIN_IN_PIN_TYPE_DL); // requires external pulldown do GND if floating
    }

    // Mask first reading
    prev_mask = readMask();

    // BIN_IN monitoring task
    SETUP_RegisterTimer(BIN_IN_CHECK_INTERVAL_IN_SECS, bin_in_AlarmFun);

    bModuleInitialised = true;
}

bool BIN_IN_ExecuteCommand(const state_t& s) {
    CHECK_MODULE_SANITY();

    switch (s.command) {
    case CMND_BIN_IN_CHANNELS_GET: // DONE
        return (bin_in_UpdateStatus(true));

    case CMND_BIN_IN_RESET_ALL_CHANNELS: // DONE
        return false;
    }

    return false; // error
}

/**
 * I0S - Update the state (forced)
 * I1S - Update the state (forced)
 */
bool decode_CMND_I(const byte* payload, state_t& s, u8* o_CmndLen) {
    CHECK_MODULE_SANITY();

    const byte* cmndStart = payload;

    s.command = (*payload++) - '0'; // [0..8] - command
    s.sum = (*payload++) - '0'; // sum = 1

    bool sanity_ok = false;
    if (s.command >= 0 && s.command <= CMND_BIN_IN_MAX_VALUE)
        if (true == isSumOk(s))
            sanity_ok = true;

    // setting decoded and valid cmnd length
    if (NULL != o_CmndLen)
        *o_CmndLen = payload - cmndStart;

    // Info display
    IF_DEB_L() {
        String str(F("BIN IN: sanity: "));
        str += sanity_ok;
        str += F(", Sum: ");
        u8 i = s.sum + '0';
        str += i;
        if (NULL != o_CmndLen) {
            str += F(", decoded cmnd len: ");
            str += *o_CmndLen;
        }
        DEBLN(str);
    }

    return (sanity_ok);
}

module_caps_t BIN_IN_getCapabilities(void) {
    module_caps_t mc = {
        .m_is_input = true,
        .m_number_of_channels = BIN_IN_NUM_OF_AVAIL_CHANNELS,
        .m_module_name = F("BIN_IN"),
        .m_mod_init = BIN_IN_ModuleInit,
        .m_cmnd_decoder = decode_CMND_I,
        .m_cmnd_executor = BIN_IN_ExecuteCommand
    };

    return(mc);
}

#endif // N32_CFG_BIN_IN_ENABLED
