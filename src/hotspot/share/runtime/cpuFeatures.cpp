/*
 * Copyright (c) 2025, oneJDK-25 Authors. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * CPU Feature Dependency Chain detection implementation.
 * 67 features across 10 CPU families with prerequisite chains.
 */

#include "precompiled.hpp"
#include "runtime/cpuFeatures.hpp"
#include "runtime/os.hpp"
#include "utilities/growableArray.hpp"
#include "utilities/macros.hpp"

// Feature dependency chain table — 55 features, 6 CPU families (Hotspot-supported only)
// Order: highest features first, base features (SCALAR) last
static const CpuFeatureEntry g_cpu_feature_table[] = {
  // x86 (32 features) — from AMX_COMPLEX down to MMX
  { "AMX_COMPLEX",      "x86",  1024, "AMX_FP16" },
  { "AMX_FP16",         "x86",  1024, "AMX_TILE" },
  { "AMX_TILE",         "x86",  1024, "AVX512F" },
  { "AMX_BF16",         "x86",  1024, "AVX512_BF16" },
  { "AMX_INT8",         "x86",  1024, "AVX512VNNI" },
  { "AVX10",            "x86",   512, "AVX512F" },
  { "APX_F",            "x86",     0, "AVX2" },
  { "AVX_IFMA",         "x86",   512, "AVX512F" },
  { "AVX512_VP2INTERSECT","x86", 512, "AVX512F" },
  { "AVX512_BF16",      "x86",   512, "AVX512BW" },
  { "AVX512VBMI2",      "x86",   512, "AVX512VBMI" },
  { "AVX512VBMI",       "x86",   512, "AVX512BW" },
  { "INTEL_DLBOOST",    "x86",   512, "AVX512VNNI" },
  { "AVX512VNNI",       "x86",   512, "AVX512BW" },
  { "AVX512BITALG",     "x86",   512, "AVX512BW" },
  { "AVX512VPOPCNTDQ",  "x86",   512, "AVX512BW" },
  { "AVX512FP16",       "x86",   512, "AVX512BW" },
  { "AVX512BW",         "x86",   512, "AVX512DQ" },
  { "AVX512DQ",         "x86",   512, "AVX512F" },
  { "AVX512CD",         "x86",   512, "AVX512F" },
  { "AVX512VL",         "x86",   256, "AVX2" },
  { "AVX512F",          "x86",   512, "AVX2" },
  { "AVX_VNNI",         "x86",   256, "AVX2" },
  { "FMA3",             "x86",   256, "AVX" },
  { "FMA4",             "x86",   256, "AVX" },
  { "XOP",              "x86",   128, "AVX" },
  { "AVX2",             "x86",   256, "AVX" },
  { "F16C",             "x86",   128, "AVX" },
  { "AVX",              "x86",   256, "SSE4_2" },
  { "BMI2",             "x86",     0, "BMI1" },
  { "BMI1",             "x86",     0, "SSE4_2" },
  { "SSE4_2",           "x86",   128, "SSE4_1" },
  { "SSE4_1",           "x86",   128, "SSSE3" },
  { "SSE4A",            "x86",   128, "SSE3" },
  { "SSSE3",            "x86",   128, "SSE3" },
  { "SSE3",             "x86",   128, "SSE2" },
  { "SSE2",             "x86",   128, "SSE" },
  { "SSE",              "x86",   128, "MMX" },
  { "MMX",              "x86",    64, "SCALAR" },

  // ARM (17 features) — from SME2 down to ARM_V8
  { "SME2",             "ARM",  2048, "SME" },
  { "SME",              "ARM",  1024, "SVE2" },
  { "SVE2",             "ARM",   512, "SVE" },
  { "ARM_SVE_512",      "ARM",   512, "SVE" },
  { "ARM_SVE_256",      "ARM",   256, "SVE" },
  { "SVE",              "ARM",   512, "ARM_DOTPROD" },
  { "ARM_I8MM",         "ARM",   128, "NEON" },
  { "ARM_BF16",         "ARM",   128, "NEON" },
  { "ARM_DOTPROD",      "ARM",   128, "NEON" },
  { "ARM_FP16",         "ARM",   128, "NEON" },
  { "ARM_ASIMD_HP",     "ARM",   128, "NEON" },
  { "ASIMD",            "ARM",   128, "NEON" },
  { "NEON",             "ARM",   128, "ARM_V8" },
  { "ARM_LSE_ATOMICS",  "ARM",     0, "ARM_V8" },
  { "ARM_CRYPTO",       "ARM",   128, "NEON" },
  { "ARM_V81",          "ARM",     0, "ARM_V8" },
  { "ARM_V8",           "ARM",     0, "SCALAR" },
  { "APPLE_AMX",        "ARM",   512, "NEON" },

  // RISC-V (4 features)
  { "TENSTORRENT_TENSIX","RISC-V",1024, "RVV" },
  { "ESPERANTO_ET_MAX",  "RISC-V", 512, "RVV" },
  { "RVV",              "RISC-V", 256, "RV64" },
  { "RV64",             "RISC-V",   0, "SCALAR" },

  // PowerPC (3 features)
  { "VSX3",             "POWER", 128, "VSX" },
  { "VSX",              "POWER", 128, "ALTIVEC" },
  { "ALTIVEC",          "POWER", 128, "SCALAR" },

  // s390x (4 features)
  { "S390_NNPA",        "S390X", 1024, "S390_VXE2" },
  { "S390_VXE2",        "S390X",  128, "S390_VXE"  },
  { "S390_VXE",         "S390X",  128, "S390_VX"   },
  { "S390_VX",          "S390X",  128, "SCALAR"    },

  // Universal base feature
  { "SCALAR",           "UNIVERSAL", 0, nullptr }
};

static const int g_cpu_feature_table_length = sizeof(g_cpu_feature_table) / sizeof(g_cpu_feature_table[0]);

// Static member initialization
bool CpuFeatures::_features_initialized = false;
GrowableArray<const char*>* CpuFeatures::_available_features = nullptr;

// Get the complete feature dependency chain table
const CpuFeatureEntry* CpuFeatures::feature_table() {
  return g_cpu_feature_table;
}

int CpuFeatures::feature_table_length() {
  return g_cpu_feature_table_length;
}

// Find a feature entry by name
static const CpuFeatureEntry* find_feature(const char* name) {
  for (int i = 0; i < g_cpu_feature_table_length; i++) {
    if (strcmp(g_cpu_feature_table[i]._name, name) == 0) {
      return &g_cpu_feature_table[i];
    }
  }
  return nullptr;
}

// Check if a feature is available on the current CPU
bool CpuFeatures::is_available(const char* feature_name) {
  if (!_features_initialized) {
    initialize();
  }
  if (_available_features == nullptr) {
    return false;
  }
  for (int i = 0; i < _available_features->length(); i++) {
    if (strcmp(_available_features->at(i), feature_name) == 0) {
      return true;
    }
  }
  return false;
}

// Check if a feature's prerequisite chain is satisfied
bool CpuFeatures::is_chain_satisfied(const char* feature_name) {
  const CpuFeatureEntry* entry = find_feature(feature_name);
  if (entry == nullptr) {
    return false;
  }
  // Check prerequisite chain recursively
  const char* prereq = entry->_prerequisite;
  while (prereq != nullptr) {
    if (!is_available(prereq)) {
      return false;
    }
    const CpuFeatureEntry* prereq_entry = find_feature(prereq);
    if (prereq_entry == nullptr) {
      return false;
    }
    prereq = prereq_entry->_prerequisite;
  }
  return true;
}

// Get the vector width for a feature
int CpuFeatures::vector_bits(const char* feature_name) {
  const CpuFeatureEntry* entry = find_feature(feature_name);
  if (entry == nullptr) {
    return 0;
  }
  return entry->_vector_bits;
}

// Get the CPU family for a feature
const char* CpuFeatures::cpu_family(const char* feature_name) {
  const CpuFeatureEntry* entry = find_feature(feature_name);
  if (entry == nullptr) {
    return "UNKNOWN";
  }
  return entry->_cpu_family;
}

// Get the prerequisite for a feature
const char* CpuFeatures::prerequisite(const char* feature_name) {
  const CpuFeatureEntry* entry = find_feature(feature_name);
  if (entry == nullptr) {
    return nullptr;
  }
  return entry->_prerequisite;
}

// Detect x86 CPU features using CPUID
bool CpuFeatures::detect_x86_features() {
  // On x86, use CPUID to detect features
  // This is a placeholder — actual CPUID implementation would go here
  // For now, we detect the base features that are always available

  // SCALAR is always available
  if (_available_features == nullptr) {
    _available_features = new GrowableArray<const char*>(64);
  }
  _available_features->append("SCALAR");

  // x86_64 always has MMX, SSE, SSE2
  _available_features->append("MMX");
  _available_features->append("SSE");
  _available_features->append("SSE2");

  // TODO: Add actual CPUID detection for SSE3, SSSE3, SSE4_1, SSE4_2, AVX, AVX2, AVX512, etc.
  // This will be implemented using os::is_cpu_feature_supported() or CPUID intrinsics

  return true;
}

// Detect ARM CPU features using HWCAP
bool CpuFeatures::detect_arm_features() {
  // On ARM, use HWCAP to detect features
  if (_available_features == nullptr) {
    _available_features = new GrowableArray<const char*>(64);
  }
  _available_features->append("SCALAR");
  _available_features->append("ARM_V8");

  // TODO: Add actual HWCAP detection for NEON, SVE, SVE2, SME, etc.

  return true;
}

// Detect RISC-V CPU features using ISA extension strings
bool CpuFeatures::detect_riscv_features() {
  // On RISC-V, use ISA extension strings to detect features
  if (_available_features == nullptr) {
    _available_features = new GrowableArray<const char*>(64);
  }
  _available_features->append("SCALAR");
  _available_features->append("RV64");

  // TODO: Add actual RISC-V ISA extension detection for V (vector), B (bitmanip), etc.

  return true;
}

// Initialize CPU features at startup
void CpuFeatures::initialize() {
  if (_features_initialized) {
    return;
  }

  _available_features = new GrowableArray<const char*>(64);

  // Detect features based on CPU architecture
#if defined(AMD64) || defined(X86)
  detect_x86_features();
#elif defined(AARCH64)
  detect_arm_features();
#elif defined(RISCV64)
  detect_riscv_features();
#else
  // Unknown architecture — only SCALAR is available
  _available_features->append("SCALAR");
#endif

  _features_initialized = true;
}

// Get the highest available feature for a CPU family
const char* CpuFeatures::highest_available_feature(const char* cpu_family) {
  if (!_features_initialized) {
    initialize();
  }

  // Table is ordered from highest to lowest, so first match is the highest
  for (int i = 0; i < g_cpu_feature_table_length; i++) {
    const CpuFeatureEntry* entry = &g_cpu_feature_table[i];
    if (strcmp(entry->_cpu_family, cpu_family) == 0 ||
        strcmp(entry->_cpu_family, "UNIVERSAL") == 0) {
      if (is_available(entry->_name)) {
        return entry->_name;
      }
    }
  }
  return "SCALAR";
}

// Print all available features (for debugging)
void CpuFeatures::print_available_features() {
  if (!_features_initialized) {
    initialize();
  }

  tty->print_cr("=== CPU Feature Detection ===");
  tty->print_cr("Available features:");
  for (int i = 0; i < _available_features->length(); i++) {
    const char* name = _available_features->at(i);
    const CpuFeatureEntry* entry = find_feature(name);
    if (entry != nullptr) {
      tty->print("  %s (family=%s, vector_bits=%d, prereq=%s)",
                 entry->_name, entry->_cpu_family,
                 entry->_vector_bits, entry->_prerequisite);
      tty->cr();
    }
  }
  tty->print_cr("===============================");
}
