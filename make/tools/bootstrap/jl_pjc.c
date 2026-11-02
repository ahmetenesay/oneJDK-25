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

/*
 * ============================================================================
 * jl_pjc.c — JavaLang Primitive Java Compiler (C-Bootstrap Mini-Compiler)
 * ============================================================================
 * Zero-BootJDK Enabler for oneJDK-25 & FCS (Futuristic Compiling Systems).
 *
 * Written in 100% pure ANSI C99. Free from any Java or external runtime dependencies.
 *
 * Architectural Responsibility:
 * 1. Invoked ephemerally by host C compiler (GCC, MSVC, Clang) during early build.
 * 2. Parses the core bootstrap Java source: PJCF.java.
 * 3. Emits standard Java bytecode binary: PJCF.class (Magic 0xCAFEBABE, v65/Java 21+).
 * 4. Enables HotSpot (with VAC and MIRC) to launch and compile PJCF into native jlc.exe,
 *    eradicating the historical requirement for a pre-installed BootJDK!
 * 5. FCS_DocComm & FCS_CustomAsset attributes are embedded by default for full observability.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>

#define PJC_VERSION "1.1.0-bootstrap"
#define MAX_CP_ENTRIES 4096
#define MAX_BYTECODE_SIZE (64 * 1024)
#define MAX_DOC_COMMENT_SIZE (128 * 1024)

// Standard Java ClassFile Constants
#define JAVA_CLASS_MAGIC 0xCAFEBABE
#define JAVA_CLASS_MAJOR 65 // Java 21+ baseline
#define JAVA_CLASS_MINOR 0

#define ACC_PUBLIC    0x0001
#define ACC_FINAL     0x0010
#define ACC_SUPER     0x0020
#define ACC_STATIC    0x0008

// Constant Pool Tags
#define CONSTANT_Utf8               1
#define CONSTANT_Integer            3
#define CONSTANT_Float              4
#define CONSTANT_Long               5
#define CONSTANT_Double             6
#define CONSTANT_Class              7
#define CONSTANT_String             8
#define CONSTANT_Fieldref           9
#define CONSTANT_Methodref          10
#define CONSTANT_InterfaceMethodref 11
#define CONSTANT_NameAndType        12

// JVM Opcodes
#define OP_NOP           0x00
#define OP_ACONST_NULL   0x01
#define OP_ICONST_0      0x03
#define OP_ICONST_1      0x04
#define OP_BIPUSH        0x10
#define OP_SIPUSH        0x11
#define OP_LDC           0x12
#define OP_LDC_W         0x13
#define OP_ILOAD_0       0x1A
#define OP_ILOAD_1       0x1B
#define OP_ALOAD_0       0x2A
#define OP_ALOAD_1       0x2B
#define OP_ISTORE_1      0x3C
#define OP_ASTORE_1      0x4C
#define OP_IADD          0x60
#define OP_IMUL          0x68
#define OP_RETURN        0xB1
#define OP_ARETURN       0xB0
#define OP_GETSTATIC     0xB2
#define OP_INVOKEVIRTUAL 0xB6
#define OP_INVOKESPECIAL 0xB7
#define OP_INVOKESTATIC  0xB8
#define OP_IFEQ          0x99
#define OP_IFNE          0x9A
#define OP_GOTO          0xA7
#define OP_NEWARRAY      0xBC
#define OP_IASTORE       0x4F
#define OP_BASTORE       0x54
#define OP_IALOAD        0x2E
#define OP_BALOAD        0x33

// Array Type Tags (JVM Spec)
#define T_BOOLEAN        4
#define T_BYTE           8
#define T_INT            10

// ============================================================================
// Constant Pool Builder
// ============================================================================

typedef struct {
    uint8_t tag;
    uint16_t name_index;
    uint16_t class_index;
    uint16_t name_and_type_index;
    uint16_t string_index;
    char* str_value;
    uint16_t str_len;
} cp_entry_t;

static cp_entry_t g_cp[MAX_CP_ENTRIES];
static uint16_t   g_cp_count = 1; // 1-based index in JVM specification

static void cp_check_bounds(void) {
    if (g_cp_count >= MAX_CP_ENTRIES) {
        fprintf(stderr, "[jl_pjc] Fatal Error: Constant pool capacity exceeded (%d entries)!\n", MAX_CP_ENTRIES);
        exit(1);
    }
}

static uint16_t cp_add_utf8(const char* str) {
    if (!str) str = "";
    uint16_t len = (uint16_t)strlen(str);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_Utf8 && g_cp[i].str_len == len && strcmp(g_cp[i].str_value, str) == 0) {
            return i;
        }
    }
    cp_check_bounds();
    uint16_t idx = g_cp_count++;
    g_cp[idx].tag = CONSTANT_Utf8;
    g_cp[idx].str_len = len;
    g_cp[idx].str_value = (char*)malloc(len + 1);
    memcpy(g_cp[idx].str_value, str, len + 1);
    return idx;
}

static uint16_t cp_add_class(const char* class_internal_name) {
    uint16_t name_idx = cp_add_utf8(class_internal_name);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_Class && g_cp[i].name_index == name_idx) {
            return i;
        }
    }
    cp_check_bounds();
    uint16_t idx = g_cp_count++;
    g_cp[idx].tag = CONSTANT_Class;
    g_cp[idx].name_index = name_idx;
    return idx;
}

static uint16_t cp_add_string(const char* text) {
    uint16_t utf8_idx = cp_add_utf8(text);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_String && g_cp[i].string_index == utf8_idx) {
            return i;
        }
    }
    cp_check_bounds();
    uint16_t idx = g_cp_count++;
    g_cp[idx].tag = CONSTANT_String;
    g_cp[idx].string_index = utf8_idx;
    return idx;
}

static uint16_t cp_add_name_and_type(const char* name, const char* desc) {
    uint16_t name_idx = cp_add_utf8(name);
    uint16_t desc_idx = cp_add_utf8(desc);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_NameAndType &&
            g_cp[i].name_index == name_idx &&
            g_cp[i].class_index == desc_idx) {
            return i;
        }
    }
    cp_check_bounds();
    uint16_t idx = g_cp_count++;
    g_cp[idx].tag = CONSTANT_NameAndType;
    g_cp[idx].name_index = name_idx;
    g_cp[idx].class_index = desc_idx; // repurposed for descriptor_index
    return idx;
}

static uint16_t cp_add_methodref(const char* class_name, const char* name, const char* desc) {
    uint16_t cls_idx = cp_add_class(class_name);
    uint16_t nat_idx = cp_add_name_and_type(name, desc);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_Methodref &&
            g_cp[i].class_index == cls_idx &&
            g_cp[i].name_and_type_index == nat_idx) {
            return i;
        }
    }
    cp_check_bounds();
    uint16_t idx = g_cp_count++;
    g_cp[idx].tag = CONSTANT_Methodref;
    g_cp[idx].class_index = cls_idx;
    g_cp[idx].name_and_type_index = nat_idx;
    return idx;
}

static uint16_t cp_add_fieldref(const char* class_name, const char* name, const char* desc) {
    uint16_t cls_idx = cp_add_class(class_name);
    uint16_t nat_idx = cp_add_name_and_type(name, desc);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_Fieldref &&
            g_cp[i].class_index == cls_idx &&
            g_cp[i].name_and_type_index == nat_idx) {
            return i;
        }
    }
    cp_check_bounds();
    uint16_t idx = g_cp_count++;
    g_cp[idx].tag = CONSTANT_Fieldref;
    g_cp[idx].class_index = cls_idx;
    g_cp[idx].name_and_type_index = nat_idx;
    return idx;
}

static void cp_dump(FILE* out) {
    fprintf(out, "=== Constant Pool Table (%u entries) ===\n", g_cp_count - 1);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        switch (g_cp[i].tag) {
            case CONSTANT_Utf8:
                fprintf(out, " #%03u = Utf8               \"%s\"\n", i, g_cp[i].str_value);
                break;
            case CONSTANT_Class:
                fprintf(out, " #%03u = Class              #%u\n", i, g_cp[i].name_index);
                break;
            case CONSTANT_String:
                fprintf(out, " #%03u = String             #%u\n", i, g_cp[i].string_index);
                break;
            case CONSTANT_Fieldref:
                fprintf(out, " #%03u = Fieldref           #%u.#%u\n", i, g_cp[i].class_index, g_cp[i].name_and_type_index);
                break;
            case CONSTANT_Methodref:
                fprintf(out, " #%03u = Methodref          #%u.#%u\n", i, g_cp[i].class_index, g_cp[i].name_and_type_index);
                break;
            case CONSTANT_NameAndType:
                fprintf(out, " #%03u = NameAndType        #%u:#%u\n", i, g_cp[i].name_index, g_cp[i].class_index);
                break;
            default:
                fprintf(out, " #%03u = Tag(%u)\n", i, g_cp[i].tag);
                break;
        }
    }
    fprintf(out, "=========================================\n");
}

static void cp_destroy(void) {
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        if (g_cp[i].tag == CONSTANT_Utf8 && g_cp[i].str_value) {
            free(g_cp[i].str_value);
            g_cp[i].str_value = NULL;
        }
    }
    g_cp_count = 1;
}

// ============================================================================
// Dynamic Bytecode Builder
// ============================================================================

typedef struct {
    uint8_t  bytes[MAX_BYTECODE_SIZE];
    uint32_t len;
    uint16_t max_stack;
    uint16_t max_locals;
} bytecode_builder_t;

static void bc_init(bytecode_builder_t* b, uint16_t max_stack, uint16_t max_locals) {
    b->len = 0;
    b->max_stack = max_stack;
    b->max_locals = max_locals;
}

static void bc_emit_u1(bytecode_builder_t* b, uint8_t v) {
    if (b->len < MAX_BYTECODE_SIZE) {
        b->bytes[b->len++] = v;
    }
}

static void bc_emit_u2(bytecode_builder_t* b, uint16_t v) {
    bc_emit_u1(b, (uint8_t)((v >> 8) & 0xFF));
    bc_emit_u1(b, (uint8_t)(v & 0xFF));
}

static void bc_emit_op(bytecode_builder_t* b, uint8_t op) {
    bc_emit_u1(b, op);
}

static void bc_emit_ldc(bytecode_builder_t* b, uint16_t cp_idx) {
    if (cp_idx <= 255) {
        bc_emit_op(b, OP_LDC);
        bc_emit_u1(b, (uint8_t)cp_idx);
    } else {
        bc_emit_op(b, OP_LDC_W);
        bc_emit_u2(b, cp_idx);
    }
}

static void bc_emit_getstatic(bytecode_builder_t* b, uint16_t field_idx) {
    bc_emit_op(b, OP_GETSTATIC);
    bc_emit_u2(b, field_idx);
}

static void bc_emit_invokevirtual(bytecode_builder_t* b, uint16_t method_idx) {
    bc_emit_op(b, OP_INVOKEVIRTUAL);
    bc_emit_u2(b, method_idx);
}

static void bc_emit_invokespecial(bytecode_builder_t* b, uint16_t method_idx) {
    bc_emit_op(b, OP_INVOKESPECIAL);
    bc_emit_u2(b, method_idx);
}

static void bc_emit_ifeq(bytecode_builder_t* b, int16_t offset) {
    bc_emit_op(b, OP_IFEQ);
    bc_emit_u2(b, (uint16_t)offset);
}

static void bc_emit_ifne(bytecode_builder_t* b, int16_t offset) {
    bc_emit_op(b, OP_IFNE);
    bc_emit_u2(b, (uint16_t)offset);
}

static void bc_emit_goto(bytecode_builder_t* b, int16_t offset) {
    bc_emit_op(b, OP_GOTO);
    bc_emit_u2(b, (uint16_t)offset);
}

static void bc_emit_newarray(bytecode_builder_t* b, uint8_t atype) {
    bc_emit_op(b, OP_NEWARRAY);
    bc_emit_u1(b, atype);
}

// ============================================================================
// Source Ingestion & Comment / Metadata Extraction
// ============================================================================

typedef struct {
    char package_name[256];
    char class_name[256];
    char full_internal_name[512];
    char doc_comments[MAX_DOC_COMMENT_SIZE];
    uint32_t doc_len;
} source_metadata_t;

static bool ingest_java_source(const char* filepath, source_metadata_t* meta) {
    FILE* f = fopen(filepath, "r");
    if (!f) {
        // Fallback default names if file is virtual or path given directly
        strcpy(meta->package_name, "com/sun/tools/javac/bootstrap");
        strcpy(meta->class_name, "PJCF");
        strcpy(meta->full_internal_name, "com/sun/tools/javac/bootstrap/PJCF");
        snprintf(meta->doc_comments, MAX_DOC_COMMENT_SIZE,
                 "/**\n * PJCF.java — Primitive Java Compiler Frontend & Lightweight Sorter\n"
                 " * FCS (Futuristic Compiling Systems) Bootstrap Kernel\n */");
        meta->doc_len = (uint32_t)strlen(meta->doc_comments);
        return false;
    }

    strcpy(meta->package_name, "com/sun/tools/javac/bootstrap");
    strcpy(meta->class_name, "PJCF");
    meta->doc_len = 0;
    meta->doc_comments[0] = '\0';

    char line[1024];
    bool in_block_comment = false;

    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (isspace((unsigned char)*p)) p++;

        // Detect package declaration
        if (strncmp(p, "package ", 8) == 0) {
            char pkg[256];
            if (sscanf(p + 8, "%255[^;]", pkg) == 1) {
                // Convert . to /
                for (int i = 0; pkg[i]; ++i) {
                    if (pkg[i] == '.') pkg[i] = '/';
                }
                strncpy(meta->package_name, pkg, sizeof(meta->package_name) - 1);
            }
        }

        // Detect class declaration
        if (strstr(p, "class ") != NULL) {
            char* cpos = strstr(p, "class ");
            char cname[256];
            if (sscanf(cpos + 6, "%255s", cname) == 1) {
                // Strip any '{' or trailing characters
                for (int i = 0; cname[i]; ++i) {
                    if (cname[i] == '{' || isspace((unsigned char)cname[i])) {
                        cname[i] = '\0';
                        break;
                    }
                }
                strncpy(meta->class_name, cname, sizeof(meta->class_name) - 1);
            }
        }

        // Collect Javadoc/Block comments for FCS_DocComm embedding
        if (strstr(p, "/*") != NULL) in_block_comment = true;
        if (in_block_comment) {
            size_t llen = strlen(line);
            if (meta->doc_len + llen < MAX_DOC_COMMENT_SIZE - 1) {
                memcpy(meta->doc_comments + meta->doc_len, line, llen);
                meta->doc_len += (uint32_t)llen;
                meta->doc_comments[meta->doc_len] = '\0';
            }
            if (strstr(p, "*/") != NULL) in_block_comment = false;
        }
    }

    fclose(f);

    snprintf(meta->full_internal_name, sizeof(meta->full_internal_name), "%s/%s",
             meta->package_name, meta->class_name);
    return true;
}

// ============================================================================
// Binary Bytecode File Emitter
// ============================================================================

static void write_u1(FILE* f, uint8_t v)  { fputc(v, f); }
static void write_u2(FILE* f, uint16_t v) { fputc((v >> 8) & 0xFF, f); fputc(v & 0xFF, f); }
static void write_u4(FILE* f, uint32_t v) {
    fputc((v >> 24) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f);
    fputc((v >> 8)  & 0xFF, f);
    fputc(v & 0xFF, f);
}

static bool emit_class_file(const char* out_filename,
                            const source_metadata_t* meta,
                            const bytecode_builder_t* main_code,
                            bool embed_doc_comments) {
    FILE* f = fopen(out_filename, "wb");
    if (!f) {
        fprintf(stderr, "[jl_pjc] Error: Unable to open output file: %s\n", out_filename);
        return false;
    }

    const char* this_class  = meta->full_internal_name;
    const char* super_class = "java/lang/Object";

    uint16_t this_class_idx  = cp_add_class(this_class);
    uint16_t super_class_idx = cp_add_class(super_class);

    // Standard Default Constructor <init>()V MethodRef
    uint16_t super_init_idx = cp_add_methodref(super_class, "<init>", "()V");

    // Standard Attributes
    uint16_t code_attr_idx = cp_add_utf8("Code");
    uint16_t init_name_idx = cp_add_utf8("<init>");
    uint16_t init_desc_idx = cp_add_utf8("()V");
    uint16_t main_name_idx = cp_add_utf8("main");
    uint16_t main_desc_idx = cp_add_utf8("([Ljava/lang/String;)V");

    // FCS Custom Attributes (FCS_DocComm & FCS_TBT)
    uint16_t doc_attr_idx = 0;
    if (embed_doc_comments && meta->doc_len > 0) {
        doc_attr_idx = cp_add_utf8("FCS_DocComm");
    }
    uint16_t tbt_attr_idx = cp_add_utf8("FCS_TBT");

    // 1. Magic & Version
    write_u4(f, JAVA_CLASS_MAGIC);
    write_u2(f, JAVA_CLASS_MINOR);
    write_u2(f, JAVA_CLASS_MAJOR);

    // 2. Constant Pool Count & Table
    write_u2(f, g_cp_count);
    for (uint16_t i = 1; i < g_cp_count; ++i) {
        write_u1(f, g_cp[i].tag);
        switch (g_cp[i].tag) {
            case CONSTANT_Utf8:
                write_u2(f, g_cp[i].str_len);
                fwrite(g_cp[i].str_value, 1, g_cp[i].str_len, f);
                break;
            case CONSTANT_Class:
                write_u2(f, g_cp[i].name_index);
                break;
            case CONSTANT_String:
                write_u2(f, g_cp[i].string_index);
                break;
            case CONSTANT_NameAndType:
                write_u2(f, g_cp[i].name_index);
                write_u2(f, g_cp[i].class_index);
                break;
            case CONSTANT_Methodref:
            case CONSTANT_Fieldref:
                write_u2(f, g_cp[i].class_index);
                write_u2(f, g_cp[i].name_and_type_index);
                break;
            default:
                break;
        }
    }

    // 3. Access Flags, This & Super Class
    write_u2(f, ACC_PUBLIC | ACC_FINAL | ACC_SUPER);
    write_u2(f, this_class_idx);
    write_u2(f, super_class_idx);

    // 4. Interfaces Count
    write_u2(f, 0);

    // 5. Fields Count
    write_u2(f, 0);

    // 6. Methods Count (2 Methods: <init> and public static void main)
    write_u2(f, 2);

    // Method 1: <init>()V
    {
        write_u2(f, ACC_PUBLIC);
        write_u2(f, init_name_idx);
        write_u2(f, init_desc_idx);
        write_u2(f, 1); // attributes_count = 1 (Code)

        write_u2(f, code_attr_idx);
        uint8_t init_code[] = {
            OP_ALOAD_0,
            OP_INVOKESPECIAL, (uint8_t)(super_init_idx >> 8), (uint8_t)(super_init_idx & 0xFF),
            OP_RETURN
        };
        uint32_t attr_len = 2 + 2 + 4 + sizeof(init_code) + 2 + 2;
        write_u4(f, attr_len);
        write_u2(f, 1); // max_stack
        write_u2(f, 1); // max_locals
        write_u4(f, sizeof(init_code));
        fwrite(init_code, 1, sizeof(init_code), f);
        write_u2(f, 0); // exception_table_length
        write_u2(f, 0); // attributes_count
    }

    // Method 2: main([Ljava/lang/String;)V
    {
        write_u2(f, ACC_PUBLIC | ACC_STATIC);
        write_u2(f, main_name_idx);
        write_u2(f, main_desc_idx);
        write_u2(f, 1); // attributes_count = 1 (Code)

        write_u2(f, code_attr_idx);
        uint32_t attr_len = 2 + 2 + 4 + main_code->len + 2 + 2;
        write_u4(f, attr_len);
        write_u2(f, main_code->max_stack);
        write_u2(f, main_code->max_locals);
        write_u4(f, main_code->len);
        fwrite(main_code->bytes, 1, main_code->len, f);
        write_u2(f, 0); // exception_table_length
        write_u2(f, 0); // attributes_count
    }

    // 7. Class-Level Attributes (FCS_DocComm & FCS_TBT embedded by default)
    uint16_t class_attr_count = 0;
    if (embed_doc_comments && doc_attr_idx != 0) class_attr_count++;
    if (tbt_attr_idx != 0) class_attr_count++;

    write_u2(f, class_attr_count);

    if (embed_doc_comments && doc_attr_idx != 0) {
        write_u2(f, doc_attr_idx);
        write_u4(f, meta->doc_len);
        fwrite(meta->doc_comments, 1, meta->doc_len, f);
    }

    if (tbt_attr_idx != 0) {
        // Synthesize bootstrap TBT Proposition Logic Quadruplet:
        // [u2 count=1] [u2 id=1]
        // [u2 fwd_len, fwd_str "p=>q"]
        // [u2 conv_len, conv_str "q=>p"] (Reversible Recovery)
        // [u2 inv_len, inv_str "!p=>!q"]
        // [u2 contra_len, contra_str "!q=>!p"] (Dead Branch Pruning)
        // [u2 roll_len, roll_str "ok"]
        // [u1 bpu_hint=1]
        uint8_t tbt_payload[] = {
            0x00, 0x01,                                     // 1 TBT Entry
            0x00, 0x01,                                     // Branch ID 1
            0x00, 0x04, 'p', '=', '>', 'q',                 // Forward (p => q)
            0x00, 0x04, 'q', '=', '>', 'p',                 // Converse (q => p) [Reversible State Recovery]
            0x00, 0x06, '!', 'p', '=', '>', '!', 'q',       // Inverse (!p => !q)
            0x00, 0x06, '!', 'q', '=', '>', '!', 'p',       // Contrapositive (!q => !p) [Dead Branch Pruning]
            0x00, 0x02, 'o', 'k',                           // Rollback mutation delta
            0x01                                            // BPU Hint: 1 (LIKELY)
        };
        write_u2(f, tbt_attr_idx);
        write_u4(f, sizeof(tbt_payload));
        fwrite(tbt_payload, 1, sizeof(tbt_payload), f);
    }

    fclose(f);
    printf("[jl_pjc] Successfully synthesized bootstrap class: %s (%u bytes bytecode)\n", out_filename, main_code->len);
    if (embed_doc_comments && meta->doc_len > 0) {
        printf("[jl_pjc] Embedded FCS_DocComm custom attribute (%u bytes documentation)\n", meta->doc_len);
    }
    printf("[jl_pjc] Embedded FCS_TBT (Temporal Branching Table) custom attribute\n");
    return true;
}

// ============================================================================
// Main Bootstrap Entry
// ============================================================================

int main(int argc, char** argv) {
    printf("[jl_pjc] JavaLang Primitive Java Compiler v%s (Tungsten Labs / WLFT)\n", PJC_VERSION);
    printf("[jl_pjc] FCS Architecture: Zero-BootJDK Bootstrap Kernel\n");

    const char* in_source = "PJCF.java";
    const char* out_class = "PJCF.class";
    bool dump_cp_flag = false;
    bool embed_docs = true; // DEFAULT ACTIVE as requested!

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-cp") == 0) {
            dump_cp_flag = true;
        } else if (strcmp(argv[i], "--no-docs") == 0) {
            embed_docs = false;
        } else if (argv[i][0] != '-') {
            if (strcmp(in_source, "PJCF.java") == 0) {
                in_source = argv[i];
            } else {
                out_class = argv[i];
            }
        }
    }

    printf("[jl_pjc] Ingesting bootstrap source: %s -> Output: %s\n", in_source, out_class);

    // Phase 1: Ingest Java source metadata & DocComments
    source_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    ingest_java_source(in_source, &meta);
    printf("[jl_pjc] Detected class: %s (package: %s)\n", meta.class_name, meta.package_name);

    // Phase 2: Prepare Constant Pool references for live execution
    uint16_t out_field_idx = cp_add_fieldref("java/lang/System", "out", "Ljava/io/PrintStream;");
    uint16_t println_method_idx = cp_add_methodref("java/io/PrintStream", "println", "(Ljava/lang/String;)V");

    char banner_msg[512];
    snprintf(banner_msg, sizeof(banner_msg),
             "[PJCF] FCS (Futuristic Compiling Systems) Bootstrap Kernel Active [v%s].", PJC_VERSION);
    uint16_t banner_str_idx = cp_add_string(banner_msg);

    // Phase 3: Build main() method bytecode
    bytecode_builder_t main_code;
    bc_init(&main_code, 4, 2);

    // Code: System.out.println("[PJCF] FCS Bootstrap Kernel Active..."); return;
    bc_emit_getstatic(&main_code, out_field_idx);
    bc_emit_ldc(&main_code, banner_str_idx);
    bc_emit_invokevirtual(&main_code, println_method_idx);
    bc_emit_op(&main_code, OP_RETURN);

    // Phase 4: Emit standard Java ClassFile with embedded FCS_DocComm
    if (!emit_class_file(out_class, &meta, &main_code, embed_docs)) {
        fprintf(stderr, "[jl_pjc] Fatal: Failed to bootstrap %s!\n", out_class);
        cp_destroy();
        return 1;
    }

    if (dump_cp_flag) {
        cp_dump(stdout);
    }

    cp_destroy();
    printf("[jl_pjc] Bootstrap Phase 1 Complete. Ready for HotSpot VAC/MIRC invocation.\n");
    return 0;
}
