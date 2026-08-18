// heltec_fem.h — Heltec V4 RF front-end (PA/LNA) control.
//
// Copied from the MeshCore project's variants/heltec_v4/LoRaFEMControl.{h,cpp}
// (MIT, https://github.com/ripplebiz/MeshCore) so lib/MeshCore stays a
// pristine submodule — same convention as PunkSX1262Wrapper. The V4.2 board
// carries a GC1109 FEM, the V4.3 a KCT8103L; both hang off the same LDO
// (P_LORA_PA_POWER) and chip-enable line (GPIO2), and init() auto-detects
// which is fitted from GPIO2's default pull level.
//
// The FEM must be switched to TX mode before every transmit and back to RX
// mode after it, or the PA stays in the receive path and TX power collapses
// — PunkHeltecBoard wires this into onBeforeTransmit/onAfterTransmit.

#pragma once

#if defined(BOARD_HELTEC_V4)

#include <stdint.h>

typedef enum {
    GC1109_PA,
    KCT8103L_PA,
    OTHER_FEM_TYPES
} LoRaFEMType;

class LoRaFEMControl
{
  public:
    LoRaFEMControl(){ }
    virtual ~LoRaFEMControl(){ }
    void init(void);
    void setSleepModeEnable(void);
    void setTxModeEnable(void);
    void setRxModeEnable(void);
    void setRxModeEnableWhenMCUSleep(void);
    void setLNAEnable(bool enabled);
    bool isLnaCanControl(void) { return lna_can_control; }
    void setLnaCanControl(bool can_control) { lna_can_control = can_control; }
    LoRaFEMType getFEMType(void) const { return fem_type; }
  private:
    LoRaFEMType fem_type=OTHER_FEM_TYPES;
    bool lna_enabled=true;
    bool lna_can_control=false;
};

#endif // BOARD_HELTEC_V4
