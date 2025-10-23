obj-$(CONFIG_BBG) += baseband_guard.o

ABI_FILE := ../abi.c

ifneq ($(wildcard $(ABI_FILE)),)
  COMMIT_SHA := $(shell grep -E '#define[[:space:]]+SUMMER_POCKETS_ABI_VERSION[[:space:]]+"[^"]+"' $(ABI_FILE) | sed -E 's/.*"([^"]+)".*/\1/')
else
  $(error ABI file $(ABI_FILE) not found)
endif

ifeq ($(strip $(COMMIT_SHA)),)
  COMMIT_SHA := unknown
endif

$(info -- BBG was enabled!)
$(info -- BBG version: $(COMMIT_SHA))

ccflags-y += -DBBG_VERSION=$(COMMIT_SHA)
