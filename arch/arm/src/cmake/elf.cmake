# ##############################################################################
# arch/arm/src/cmake/elf.cmake
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

# Loadable and ELF module settings

nuttx_elf_compile_options(-fvisibility=hidden -mlong-calls)

nuttx_mod_compile_options(-fvisibility=hidden -mlong-calls)

nuttx_elf_compile_options_ifdef(CONFIG_UNWINDER_ARM -fno-unwind-tables
                                -fno-asynchronous-unwind-tables)

# A PIC module reaches its own data through r10, which -mpic-register=r10
# already reserves for it.  --fixed-r10 must not be added alongside: GCC rejects
# the combination with "unable to use 'r10' for PIC register" as soon as -fpic
# is in effect, and reserves r10 for nothing when it is not.
#
# --fixed-r10 belongs on the base firmware, not on the module -- it is what
# stops the firmware allocating r10, so that a callback from firmware into
# module code arrives with the module's data base intact.  The Makefile build
# adds it to CFLAGS for exactly that reason and filters it back out of the
# module flags.  The CMake build does not add it anywhere yet, which is a
# separate gap: adding it with add_compile_options() would reach these
# application targets too and reintroduce the same rejected combination.

nuttx_elf_compile_options_ifdef(CONFIG_PIC -mpic-register=r10)

nuttx_elf_link_options_ifdef(
  CONFIG_PIC --unresolved-symbols=ignore-in-object-files --emit-relocs)

nuttx_elf_link_options_ifdef(CONFIG_BINFMT_ELF_RELOCATABLE -r)

nuttx_mod_link_options(-r)

nuttx_elf_link_options_ifdef(CONFIG_BUILD_KERNEL -Bstatic)

if(CONFIG_DEBUG_OPT_UNUSED_SECTIONS)
  if("${CMAKE_LD}" MATCHES "gcc$")
    nuttx_elf_link_options(-Wl,--gc-sections)
  else()
    nuttx_elf_link_options(--gc-sections)
  endif()
endif()

nuttx_elf_link_options(-e _start)
