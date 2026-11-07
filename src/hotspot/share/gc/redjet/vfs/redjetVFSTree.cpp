/*
 * Copyright (c) 2025-2026 Tungsten Labs, Final Technologies Company; WLFT. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact WLFT or visit https://github.com/ahmetenesay/oneJDK-25
 * if you need additional information or have any questions.
 */

#include <cstdio>
#include <cstdlib>
#include "gc/redjet/vfs/redjetVFSTree.hpp"
#ifdef REDJET_STANDALONE_BUILD
#include "gc/redjet/runtime/redjetFlags.hpp"
static const bool RedjetVFSStrict = true;
static const bool RedjetVFSPanicDialog = false;
#else
#include "gc/shared/gc_globals.hpp"
#include "logging/log.hpp"
#include "runtime/globals.hpp"
#endif

RedjetVFSTree::RedjetVFSTree() : _root(nullptr), _lock(false) {}

RedjetVFSTree::~RedjetVFSTree() {
  destroy();
}

void RedjetVFSTree::init() {
  lock();
  if (_root != nullptr) {
    delete _root;
    _root = nullptr;
  }
  _root = new RedjetVFSNode("/", "/");
  unlock();

  // 1. Motherline Egg (Newborn unoptimized generation)
  mkdir_p("/heap/on_heap/runway/motherline/egg/young/tank");
  mkdir_p("/heap/on_heap/runway/motherline/egg/young/demand");
  mkdir_p("/heap/on_heap/runway/motherline/egg/young/middle");
  mkdir_p("/heap/on_heap/runway/motherline/egg/young/hotfield");
  mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/tank");
  mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/demand");
  mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/middle");
  mkdir_p("/heap/on_heap/runway/motherline/egg/adolescent/hotfield");
  mkdir_p("/heap/on_heap/runway/motherline/egg/senior/tank");
  mkdir_p("/heap/on_heap/runway/motherline/egg/senior/demand");
  mkdir_p("/heap/on_heap/runway/motherline/egg/senior/middle");
  mkdir_p("/heap/on_heap/runway/motherline/egg/senior/hotfield");

  // 2. Motherline Fledgling (Optimized generation)
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/tank");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/demand");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/middle");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/young/hotfield");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/tank");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/demand");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/middle");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/adolescent/hotfield");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/tank");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/demand");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/middle");
  mkdir_p("/heap/on_heap/runway/motherline/fledgling/senior/hotfield");

  // 3. Optimization Lobby
  mkdir_p("/heap/on_heap/runway/pitstop");

  // 4. Pretrashing Coffin
  mkdir_p("/heap/on_heap/runway/falline");

  // 5. Puzzle(Coach) Arena - Optimization Race(measuring)
  mkdir_p("/heap/on_heap/puzzle_arena/stagnant");
  mkdir_p("/heap/on_heap/puzzle_arena/eval");

  // 6. Landfill (Matured, Rehabilitation & Meta Storage)
  mkdir_p("/heap/on_heap/landfill/trash_pit/matured");
  mkdir_p("/heap/on_heap/landfill/trash_pit/rehab");
  mkdir_p("/heap/on_heap/landfill/header_pane");

  // 7. Reference Harbingers(TailTraces Signals) Storage - Reference Predicting Signal Storage
  mkdir_p("/heap/on_heap/rpss");

  // 8. Off-Heap Pieces
  mkdir_p("/heap/off_heap/code_boot/rawcode_cell");
  mkdir_p("/heap/off_heap/code_boot/vac_processed_code_chunk");
  mkdir_p("/heap/off_heap/code_boot/mirc_execution_ready_page/lvl1");
  mkdir_p("/heap/off_heap/code_boot/mirc_execution_ready_page/lvl2");
  mkdir_p("/heap/off_heap/log_boot/hotlog");
  mkdir_p("/heap/off_heap/log_boot/coldlog");
  mkdir_p("/heap/off_heap/reserved_boot");

  log_info(gc, init)("Redjet VFS Memory Tree: Initialized 39 standard hierarchical heap branches");
}

void RedjetVFSTree::destroy() {
  lock();
  if (_root != nullptr) {
    delete _root;
    _root = nullptr;
  }
  unlock();
}

RedjetVFSNode* RedjetVFSTree::_find_node_unlocked(const char* path) {
  if (path == nullptr || _root == nullptr) return nullptr;

  const char* p = path;
  while (*p == '/' || *p == '\\') ++p;
  if (*p == '\0') return _root;

  RedjetVFSNode* curr = _root;
  while (*p != '\0') {
    const char* start = p;
    while (*p != '\0' && *p != '/' && *p != '\\') ++p;
    size_t len = p - start;
    if (len == 0) continue;

    char part[64];
    size_t copy_len = len < sizeof(part) - 1 ? len : sizeof(part) - 1;
    strncpy(part, start, copy_len);
    part[copy_len] = '\0';

    RedjetVFSNode* found = nullptr;
    for (int i = 0; i < curr->_children.length(); ++i) {
      RedjetVFSNode* child = curr->_children.at(i);
      if (strcmp(child->_name, part) == 0) {
        found = child;
        break;
      }
    }
    if (found == nullptr) return nullptr;
    curr = found;

    while (*p == '/' || *p == '\\') ++p;
  }
  return curr;
}

RedjetVFSNode* RedjetVFSTree::_mkdir_p_unlocked(const char* path, size_t quota_bytes) {
  if (path == nullptr || _root == nullptr) return nullptr;

  const char* p = path;
  while (*p == '/' || *p == '\\') ++p;
  if (*p == '\0') return _root;

  RedjetVFSNode* curr = _root;
  char cur_path[192] = "";

  while (*p != '\0') {
    const char* start = p;
    while (*p != '\0' && *p != '/' && *p != '\\') ++p;
    size_t len = p - start;
    if (len == 0) continue;

    char part[64];
    size_t copy_len = len < sizeof(part) - 1 ? len : sizeof(part) - 1;
    strncpy(part, start, copy_len);
    part[copy_len] = '\0';

    size_t cur_len = strlen(cur_path);
    snprintf(cur_path + cur_len, sizeof(cur_path) - cur_len, "/%s", part);

    RedjetVFSNode* found = nullptr;
    for (int i = 0; i < curr->_children.length(); ++i) {
      RedjetVFSNode* child = curr->_children.at(i);
      if (strcmp(child->_name, part) == 0) {
        found = child;
        break;
      }
    }

    if (found == nullptr) {
      found = new RedjetVFSNode(part, cur_path, curr, quota_bytes);
      curr->_children.append(found);
    }
    curr = found;

    while (*p == '/' || *p == '\\') ++p;
  }

  if (quota_bytes > 0 && curr != nullptr) {
    curr->_quota_bytes = quota_bytes;
  }
  return curr;
}

RedjetVFSNode* RedjetVFSTree::mkdir_p(const char* path, size_t quota_bytes) {
  if (path == nullptr) return nullptr;
  lock();
  RedjetVFSNode* node = _mkdir_p_unlocked(path, quota_bytes);
  unlock();
  return node;
}

RedjetVFSNode* RedjetVFSTree::find_node(const char* path) {
  if (path == nullptr) return nullptr;
  lock();
  RedjetVFSNode* node = _find_node_unlocked(path);
  unlock();
  if (node == nullptr && RedjetVFSStrict) {
    log_error(gc)("Strict VFS validation failure: branch '%s' not found!", path);
  }
  return node;
}

size_t RedjetVFSTree::get_used_bytes(const char* path) {
  RedjetVFSNode* n = find_node(path);
  return n != nullptr ? n->used() : 0;
}

void RedjetVFSTree::print_node_recursive(outputStream* st, RedjetVFSNode* node, int indent) const {
  if (node == nullptr) return;
  st->print("%*s- %s (used: %zu", indent * 2, "", node->_name, node->used());
  if (node->_quota_bytes > 0) {
    st->print(", quota: %zu", node->_quota_bytes);
  }
  st->print_cr(")");
  for (int i = 0; i < node->_children.length(); ++i) {
    print_node_recursive(st, node->_children.at(i), indent + 1);
  }
}

void RedjetVFSTree::print_on(outputStream* st) const {
  st->print_cr("Redjet VFS Hierarchy:");
  if (_root != nullptr) {
    print_node_recursive(st, _root, 0);
  }
}
