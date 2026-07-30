/****************************************************************************
 * boards/arm64/qemu/qemu-armv8a/scripts/nuttx_user.ld
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include "memory.ld"

/* The read-only part of the blob and the writable part are given different
 * MMU permissions at boot, so the boundary between them -- and the end of
 * .bss, where the kernel heap begins -- must fall on a page boundary.  See
 * qemu_userspace_mmuregions().
 */

OUTPUT_ARCH(aarch64)
SECTIONS
{
    .userspace : {
        KEEP(*(.userspace))
    } > usram

    .text : {
        _stext = ABSOLUTE(.);
        *(.text .text.*)
        *(.fixup)
        *(.gnu.warning)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.t.*)
        *(.glue_7)
        *(.glue_7t)
        *(.got)
        *(.gcc_except_table)
        *(.gnu.linkonce.r.*)
        _etext = ABSOLUTE(.);
    } > usram

    .ARM.extab : {
        *(.ARM.extab*)
    } > usram

    __exidx_start = ABSOLUTE(.);
    .ARM.exidx : {
        *(.ARM.exidx*)
    } > usram

    __exidx_end = ABSOLUTE(.);

    /* The alignment belongs to the output section, not to the location
     * counter between sections:  with a MEMORY region in play the region
     * allocator decides where the next section lands, and a bare
     * ". = ALIGN(4096)" here is quietly ignored.
     *
     * .data is already in place -- the blob is loaded into DRAM, not copied
     * out of ROM -- so _eronly, the source .data would be copied from, is
     * simply where .data already is.
     */

    .data ALIGN(4096) : {
        _eronly = ABSOLUTE(.);
        _sdata = ABSOLUTE(.);
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        CONSTRUCTORS
        . = ALIGN(8);
        _edata = ABSOLUTE(.);
    } > usram

    .init_section : {
        _sinit = ABSOLUTE(.);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE(*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        _einit = ABSOLUTE(.);
    } > usram

    .bss : {
        . = ALIGN(64);
        _sbss = ABSOLUTE(.);
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(4096);
        _ebss = ABSOLUTE(.);
    } > usram

    /* Stabs debugging sections */

    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .stab.excl 0 : { *(.stab.excl) }
    .stab.exclstr 0 : { *(.stab.exclstr) }
    .stab.index 0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment 0 : { *(.comment) }

    /* DWARF debug sections.
         Symbols in the DWARF debugging sections are relative to the beginning
         of the section so we begin them at 0.  */

    /* DWARF 1 */

    .debug 0            : { *(.debug) }
    .line 0             : { *(.line) }

    /* GNU DWARF 1 extensions */

    .debug_srcinfo 0    : { *(.debug_srcinfo) }
    .debug_sfnames 0    : { *(.debug_sfnames) }

    /* DWARF 1.1 and DWARF 2 */

    .debug_aranges 0    : { *(.debug_aranges) }
    .debug_pubnames 0   : { *(.debug_pubnames) }

    /* DWARF 2 */

    .debug_info 0       : { *(.debug_info) *(.gnu.linkonce.wi.*) }
    .debug_abbrev 0     : { *(.debug_abbrev) }
    .debug_line 0       : { *(.debug_line) }
    .debug_frame 0      : { *(.debug_frame) }
    .debug_str 0        : { *(.debug_str) }
    .debug_loc 0        : { *(.debug_loc) }
    .debug_macinfo 0    : { *(.debug_macinfo) }
}
