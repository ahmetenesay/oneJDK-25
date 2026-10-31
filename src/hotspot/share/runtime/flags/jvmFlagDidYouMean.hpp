/*
 * Copyright (c) 2026, oneJDK-25 project. All rights reserved.
 * Global HotSpot Option Recognition & "Did You Mean?" Engine.
 */

#ifndef SHARE_RUNTIME_FLAGS_JVMFLAGDIDYOUMEAN_HPP
#define SHARE_RUNTIME_FLAGS_JVMFLAGDIDYOUMEAN_HPP

#include "memory/allStatic.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "utilities/globalDefinitions.hpp"

// Represents a semantic intent keyword, abbreviation, or technical alias
// mapped to a canonical HotSpot JVM flag along with developer documentation.
struct HotSpotIntentEntry {
  const char* alias;           // Search alias/intent keyword (e.g. "hugepages", "pretouch", "simd_gc")
  const char* canonical_flag;  // Canonical HotSpot flag name (e.g. "UseLargePages", "AlwaysPreTouch")
  const char* technical_doc;   // Technical rationale and human-readable hint explaining the flag
};

class JVMFlagDidYouMean : public AllStatic {
private:
  static const HotSpotIntentEntry INTENT_REGISTRY[];
  static const size_t INTENT_REGISTRY_COUNT;

public:
  // Normalizes an input string: strips '_', '-', '+', ':', '=', and converts to lowercase
  static void normalize_string(const char* src, size_t src_len, char* dst, size_t dst_sz);

  // String distance & similarity metrics ported from high-precision vector VM runtime
  static int    damerau_levenshtein_distance(const char* s1, size_t l1, const char* s2, size_t l2);
  static double jaro_winkler_similarity(const char* s1, size_t l1, const char* s2, size_t l2);

  // Unified global resolution engine:
  // Evaluates semantic intent registry, case/separator normalization, affix ('Use') detection,
  // Damerau-Levenshtein edit distance, and Jaro-Winkler similarity across all JVM flags.
  static JVMFlag* resolve(const char* name, size_t length, bool allow_locked, const char** out_hint = nullptr);
};

#endif // SHARE_RUNTIME_FLAGS_JVMFLAGDIDYOUMEAN_HPP
