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
// Redlet GC Node Definitions
// ============================================================================
def redlet_load_p        : SDNode<"HotSpotISD::REDLET_LOAD_P", SDTHotspotGCOut1>;
def redlet_load_n        : SDNode<"HotSpotISD::REDLET_LOAD_N", SDTHotspotGCOut1>;
def redlet_store_p       : SDNode<"HotSpotISD::REDLET_STORE_P", SDTHotspotGC>;
def redlet_store_n       : SDNode<"HotSpotISD::REDLET_STORE_N", SDTHotspotGC>;
def redlet_encode_p      : SDNode<"HotSpotISD::REDLET_ENCODE_P", SDTHotspotGCOut1>;
def redlet_cae_p         : SDNode<"HotSpotISD::REDLET_CAE_P", SDTHotspotGCOut1>;
def redlet_cae_n         : SDNode<"HotSpotISD::REDLET_CAE_N", SDTHotspotGCOut1>;
def redlet_cas_p         : SDNode<"HotSpotISD::REDLET_CAS_P", SDTHotspotGCOut1>;
def redlet_cas_n         : SDNode<"HotSpotISD::REDLET_CAS_N", SDTHotspotGCOut1>;
def redlet_xchg_p        : SDNode<"HotSpotISD::REDLET_XCHG_P", SDTHotspotGCOut1>;
def redlet_xchg_n        : SDNode<"HotSpotISD::REDLET_XCHG_N", SDTHotspotGCOut1>;

// Redlet SFVM 4-Point Coordination & SIMD Dispatch SDNodes
def redlet_safepoint_poll   : SDNode<"HotSpotISD::REDLET_SAFEPOINT_POLL", SDTHotspotGC>;
def redlet_pausepoint_poll  : SDNode<"HotSpotISD::REDLET_PAUSEPOINT_POLL", SDTHotspotGC>;
def redlet_hookpoint_pin    : SDNode<"HotSpotISD::REDLET_HOOKPOINT_PIN", SDTHotspotGCOut1>;
def redlet_controlpoint_chk : SDNode<"HotSpotISD::REDLET_CONTROLPOINT_CHK", SDTHotspotGCOut1>;
def redlet_simd_dispatch    : SDNode<"HotSpotISD::REDLET_SIMD_DISPATCH", SDTHotspotGCOut1>;

#endif // HOTSPOT_SDNODES_TD
