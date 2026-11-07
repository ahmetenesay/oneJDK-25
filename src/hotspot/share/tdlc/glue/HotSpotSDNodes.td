//
// Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
// https://github.com/ahmetenesay/oneJDK-25
//
// HotSpotSDNodes.td — HotSpot C2 Ideal Node and GC Barrier SDNodes
// ===============================================================
// This file defines the SelectionDAG / MachNode node types used by
// Hotspot C2 compiler and Garbage Collectors across all architectures.

#ifndef HOTSPOT_SDNODES_TD
#define HOTSPOT_SDNODES_TD

// Generic profile for HotSpot GC operations
def SDTHotspotGC : SDTypeProfile<0, -1, []>;
def SDTHotspotGCOut1 : SDTypeProfile<1, -1, []>;

// ============================================================================
// G1 GC Node Definitions
// ============================================================================
def store_p       : SDNode<"HotSpotISD::STORE_P", SDTHotspotGC>;
def store_n       : SDNode<"HotSpotISD::STORE_N", SDTHotspotGC>;
def encode_p      : SDNode<"HotSpotISD::ENCODE_P", SDTHotspotGCOut1>;
def g1_cae_p      : SDNode<"HotSpotISD::G1_CAE_P", SDTHotspotGCOut1>;
def g1_cae_n      : SDNode<"HotSpotISD::G1_CAE_N", SDTHotspotGCOut1>;
def g1_cas_p      : SDNode<"HotSpotISD::G1_CAS_P", SDTHotspotGCOut1>;
def g1_cas_n      : SDNode<"HotSpotISD::G1_CAS_N", SDTHotspotGCOut1>;
def g1_xchg_p     : SDNode<"HotSpotISD::G1_XCHG_P", SDTHotspotGCOut1>;
def g1_xchg_n     : SDNode<"HotSpotISD::G1_XCHG_N", SDTHotspotGCOut1>;

// ============================================================================
// Shenandoah GC Node Definitions
// ============================================================================
def shenandoah_cas_p : SDNode<"HotSpotISD::SHENANDOAH_CAS_P", SDTHotspotGCOut1>;
def shenandoah_cas_n : SDNode<"HotSpotISD::SHENANDOAH_CAS_N", SDTHotspotGCOut1>;
def shenandoah_cae_p : SDNode<"HotSpotISD::SHENANDOAH_CAE_P", SDTHotspotGCOut1>;
def shenandoah_cae_n : SDNode<"HotSpotISD::SHENANDOAH_CAE_N", SDTHotspotGCOut1>;

// ============================================================================
// ZGC Node Definitions
// ============================================================================
def z_load_p        : SDNode<"HotSpotISD::Z_LOAD_P", SDTHotspotGCOut1>;
def z_null_check_p  : SDNode<"HotSpotISD::Z_NULL_CHECK_P", SDTHotspotGCOut1>;
def z_store_p       : SDNode<"HotSpotISD::Z_STORE_P", SDTHotspotGC>;
def z_store_null_p  : SDNode<"HotSpotISD::Z_STORE_NULL_P", SDTHotspotGC>;
def z_cae_p         : SDNode<"HotSpotISD::Z_CAE_P", SDTHotspotGCOut1>;
def z_cas_p         : SDNode<"HotSpotISD::Z_CAS_P", SDTHotspotGCOut1>;
def z_xchg_p        : SDNode<"HotSpotISD::Z_XCHG_P", SDTHotspotGCOut1>;

// ============================================================================
// Redjet GC Node Definitions
// ============================================================================
def redjet_load_p        : SDNode<"HotSpotISD::REDJET_LOAD_P", SDTHotspotGCOut1>;
def redjet_load_n        : SDNode<"HotSpotISD::REDJET_LOAD_N", SDTHotspotGCOut1>;
def redjet_store_p       : SDNode<"HotSpotISD::REDJET_STORE_P", SDTHotspotGC>;
def redjet_store_n       : SDNode<"HotSpotISD::REDJET_STORE_N", SDTHotspotGC>;
def redjet_encode_p      : SDNode<"HotSpotISD::REDJET_ENCODE_P", SDTHotspotGCOut1>;
def redjet_cae_p         : SDNode<"HotSpotISD::REDJET_CAE_P", SDTHotspotGCOut1>;
def redjet_cae_n         : SDNode<"HotSpotISD::REDJET_CAE_N", SDTHotspotGCOut1>;
def redjet_cas_p         : SDNode<"HotSpotISD::REDJET_CAS_P", SDTHotspotGCOut1>;
def redjet_cas_n         : SDNode<"HotSpotISD::REDJET_CAS_N", SDTHotspotGCOut1>;
def redjet_xchg_p        : SDNode<"HotSpotISD::REDJET_XCHG_P", SDTHotspotGCOut1>;
def redjet_xchg_n        : SDNode<"HotSpotISD::REDJET_XCHG_N", SDTHotspotGCOut1>;

// Redjet SFVM 4-Point Coordination & SIMD Dispatch SDNodes
def redjet_safepoint_poll   : SDNode<"HotSpotISD::REDJET_SAFEPOINT_POLL", SDTHotspotGC>;
def redjet_pausepoint_poll  : SDNode<"HotSpotISD::REDJET_PAUSEPOINT_POLL", SDTHotspotGC>;
def redjet_hookpoint_pin    : SDNode<"HotSpotISD::REDJET_HOOKPOINT_PIN", SDTHotspotGCOut1>;
def redjet_controlpoint_chk : SDNode<"HotSpotISD::REDJET_CONTROLPOINT_CHK", SDTHotspotGCOut1>;
def redjet_simd_dispatch    : SDNode<"HotSpotISD::REDJET_SIMD_DISPATCH", SDTHotspotGCOut1>;

// Redjet 2-Point Zero-STW Hardware Sync & Universal MachPollPoint
def redjet_dockpoint        : SDNode<"HotSpotISD::REDJET_DOCKPOINT", SDTHotspotGC>;
def redjet_parkpoint        : SDNode<"HotSpotISD::REDJET_PARKPOINT", SDTHotspotGC>;
def mach_poll_point         : SDNode<"HotSpotISD::MACH_POLL_POINT", SDTHotspotGC>;

#endif // HOTSPOT_SDNODES_TD
