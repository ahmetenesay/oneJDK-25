/*
 * Copyright (c) 2025, oneJDK-25 Authors. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This file is part of the CPU Feature Dependency Chain detection mechanism.
 * It defines 67 CPU features across 10 CPU families with their prerequisite chains.
 *
 * C2 compiler uses this table to determine which instruction sets are available
 * on the current CPU at runtime, enabling AVX2, AVX512, AVX10, AMX, and more.
 */

#ifndef SHARE_RUNTIME_CPUFEATURES_HPP
#define SHARE_RUNTIME_CPUFEATURES_HPP

#include "utilities/globalDefinitions.hpp"

// CPU Feature Dependency Chain entry
// Each entry: feature name, CPU family, vector width (bits), prerequisite feature
struct CpuFeatureEntry {
  const char* _name;          // Feature name (e.g., "AVX512F")
  const char* _cpu_family;    // CPU family (e.g., "x86", "ARM", "RISC-V")
  int          _vector_bits;  // Vector width in bits (0 = scalar)
  const char* _prerequisite;  // Prerequisite feature name (nullptr = base feature)
};

// CPU Feature Detection
class CpuFeatures : AllStatic {
 public:
  // Get the complete feature dependency chain table
  static const CpuFeatureEntry* feature_table();
  static int feature_table_length();

  // Check if a feature is available on the current CPU
  static bool is_available(const char* feature_name);

  // Check if a feature's prerequisite chain is satisfied
  static bool is_chain_satisfied(const char* feature_name);

  // Get the vector width for a feature
  static int vector_bits(const char* feature_name);

  // Get the CPU family for a feature
  static const char* cpu_family(const char* feature_name);

  // Get the prerequisite for a feature
  static const char* prerequisite(const char* feature_name);

  // Detect and initialize CPU features at startup
  static void initialize();

  // Get the highest available feature for a CPU family
  static const char* highest_available_feature(const char* cpu_family);

  // Print all available features (for debugging)
  static void print_available_features();

 private:
  // Internal feature detection functions
  static bool detect_x86_features();
  static bool detect_arm_features();
  static bool detect_riscv_features();

  // Cache of detected features
  static bool _features_initialized;
  static GrowableArray<const char*>* _available_features;
};

#endif // SHARE_RUNTIME_CPUFEATURES_HPP
