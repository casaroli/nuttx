############################################################################
# tools/bcm2712/Config.mk
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

ifeq ($(CONFIG_ARCH_BOARD_RASPBERRYPI_5B),y)

CONFIG_TXT = config.txt

# The load address is stated rather than left to the firmware default, so the
# linker script and config.txt cannot drift apart.

define POSTBUILD
	$(Q)echo "Generating $(CONFIG_TXT)";
	$(Q)echo "kernel=nuttx.bin" > $(CONFIG_TXT);
	$(Q)echo "arm_64bit=1" >> $(CONFIG_TXT);
	$(Q)echo "device_tree=" >> $(CONFIG_TXT);
	$(Q)echo "os_check=0" >> $(CONFIG_TXT);
	$(if $(CONFIG_RPI5B_DEBUG_BOOT),$(Q)echo "uart_2ndstage=1" >> $(CONFIG_TXT);)
	$(if $(CONFIG_RPI5B_ENABLE_JTAG),$(Q)echo "enable_jtag_gpio=1" >> $(CONFIG_TXT);)
endef

endif
