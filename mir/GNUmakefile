PREFIX=/usr/local
SRC_DIR=.
BUILD_DIR=.

ADDITIONAL_INCLUDE_PATH:=
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
    XCRUN := $(shell xcrun --show-sdk-path >/dev/null 2>&1 && echo yes || echo no)
    ifeq ($(XCRUN),yes)
      ADDITIONAL_INCLUDE_PATH := $(shell xcrun --show-sdk-path)/usr/include
    endif
endif

LDFLAGS =
OBJO=-o #trailing space is important
EXEO=-o #trailing space is important
ifeq ($(OS),Windows_NT)
  EXE=.exe
  ifeq ($(CC),cc)
    ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
      CC=gcc
    else
      CC=cl
    endif
  endif
  ifeq ($(CC),gcc)
    CFLAGS += -fPIC -g -std=gnu11 -Wno-abi -fsigned-char
    CFLAGS += -fno-tree-sra
    COPTFLAGS = -O3 -DNDEBUG
    CDEBFLAGS =
    CDEB2FLAGS = -Wall -Wextra -g3 -dwarf4 -fsanitize=address -fsanitize=undefined -fno-sanitize=alignment
    CFLAGS += $(COPTFLAGS)
    LDFLAGS=-Wl,--stack,8388608
    LD2FLAGS= $(LDFLAGS)
    MIR_LIBS=-lm -lkernel32 -lpsapi
  else ifeq ($(CC),cl)
    COPTFLAGS = -O2 -DNDEBUG
    CDEBFLAGS = -Od -Z7
    CDEB2FLAGS = $(CDEBFLAGS)
    CFLAGS += -nologo $(COPTFLAGS)
    LDFLAGS= -nologo -F 8388608
    LD2FLAGS= $(LDFLAGS)
    MIR_LIBS=
    OBJO=-Fo:
    EXEO=-Fe:
  endif

  CPPFLAGS = -I$(SRC_DIR)
  LDLIBS   = $(MIR_LIBS)
  COMPILE = $(CC) $(CPPFLAGS) $(CFLAGS)
  ifeq ($(CC),gcc)
    COMPILE += -MMD -MP
  endif
  LINK = $(CC) $(LDFLAGS)
  COMPILE_AND_LINK = $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

else
  EXE=
  CC=gcc
  CFLAGS += -fPIC -g -std=gnu11 -Wno-abi -fsigned-char
  ifneq ($(ADDITIONAL_INCLUDE_PATH),)
    CFLAGS += -DADDITIONAL_INCLUDE_PATH=\"$(ADDITIONAL_INCLUDE_PATH)\"
  endif

  ifeq ($(shell $(CC) -v 2>&1 | grep -c "clang version"), 0)
    ifeq ($(shell $(CC) -fno-tree-sra 2>&1 | grep -c 'fno-tree-sra'), 0)
      CFLAGS += -fno-tree-sra
    endif
    ifeq ($(shell $(CC) -fno-ipa-cp-clone 2>&1 | grep -c 'fno-ipa-cp-clone'), 0)
      CFLAGS += -fno-ipa-cp-clone
    endif
  endif

  MIR_LIBS=-lm -ldl
  COPTFLAGS = -O3 -DNDEBUG
  CDEBFLAGS =
  CDEB2FLAGS = -Wall -Wextra -Wshadow -g3 -fsanitize=address -fsanitize=undefined -fno-sanitize=alignment
  LD2FLAGS =  -fsanitize=address -fsanitize=undefined  -fno-sanitize=alignment
  CFLAGS += $(COPTFLAGS)
  CPPFLAGS = -I$(SRC_DIR)
  LDLIBS   = $(MIR_LIBS)
  COMPILE = $(CC) $(CPPFLAGS) -MMD -MP $(CFLAGS)
  LINK = $(CC) $(LDFLAGS)
  COMPILE_AND_LINK = $(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)
endif

API_VERSION=1
MAJOR_VERSION=0
MINOR_VERSION=1
GITCOMMIT:= $(shell cd $(SRC_DIR) && git log -1 --pretty='%H')
CFLAGS += -DGITCOMMIT=$(GITCOMMIT)

ifeq ($(CC),cl)
  OBJSUFF=obj
  LIBSUFF=lib
  SOLIB=libmir.dll
else
  OBJSUFF=o
  LIBSUFF=a
  ifeq ($(OS),Windows_NT)
    ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
      SONAME=libmir.so.$(API_VERSION)
      SOLIBFLAGS=-shared -Wl,-soname,$(SONAME)
      SOLIB=$(SONAME).$(MAJOR_VERSION).$(MINOR_VERSION)
    else
      SOLIB=libmir.dll
    endif
  else
    ifeq ($(UNAME_S),Darwin)
      SOLIBVERSION=$(API_VERSION).$(MAJOR_VERSION)
      SOLIB=libmir.$(API_VERSION).dylib
      SOLIBFLAGS=-dynamiclib -install_name "$(SOLIB)" -current_version $(SOLIBVERSION).$(MINOR_VERSION) -compatibility_version $(SOLIBVERSION)
    else
      SONAME=libmir.so.$(API_VERSION)
      SOLIBFLAGS=-shared -Wl,-soname,$(SONAME)
      SOLIB=$(SONAME).$(MAJOR_VERSION).$(MINOR_VERSION)
    endif
  endif
endif

C2M_BOOTSTRAP_FLAGS = -DMIR_BOOTSTRAP
C2M_BOOTSTRAP_FLAGS0 := $(C2M_BOOTSTRAP_FLAGS)
ifeq ($(shell sh $(SRC_DIR)/check-threads.sh), ok)
  ifneq ($(CC),cl)
    MIR_LIBS += -lpthread
    CFLAGS += -DC2MIR_PARALLEL
    C2M_BOOTSTRAP_FLAGS += -DC2MIR_PARALLEL
  endif
endif

L2M_EXE=
L2M_TEST=
ifneq ($(shell test -f /usr/include/llvm-c/Core.h|echo 1), 1)
L2M_EXE += $(BUILD_DIR)/l2m$(EXE)
L2M_TEST += l2m-test$(EXE)
endif

EXECUTABLES=$(BUILD_DIR)/c2m$(EXE) $(BUILD_DIR)/m2b$(EXE) $(BUILD_DIR)/b2m$(EXE) $(BUILD_DIR)/b2ctab$(EXE) $(L2M_EXE) $(BUILD_DIR)/mir-bin-run$(EXE)

Q=@

# Entries should be used for building and installation
.PHONY: all debug install uninstall clean test bench

all: $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB) $(EXECUTABLES)

debug: CFLAGS:=$(subst $(COPTFLAGS),$(CDEBFLAGS),$(CFLAGS))
debug: $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB) $(EXECUTABLES)

debug2: CFLAGS:=$(subst $(COPTFLAGS),$(CDEB2FLAGS),$(CFLAGS))
debug2: LDFLAGS:=$(LD2FLAGS)
debug2: $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB) $(EXECUTABLES)

install: $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB) $(EXECUTABLES) | $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/bin
	install -m a+r $(SRC_DIR)/mir.h $(SRC_DIR)/mir-dlist.h $(SRC_DIR)/mir-varr.h $(SRC_DIR)/mir-htab.h\
		       $(SRC_DIR)/mir-gen.h $(SRC_DIR)/c2mir/c2mir.h $(PREFIX)/include
	install -m a+r $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB) $(PREFIX)/lib
ifeq ($(OS),Windows_NT)
else
    ifeq ($(UNAME_S),Darwin)
	rm -f $(PREFIX)/lib/libmir.dylib
	ln -s $(PREFIX)/lib/$(SOLIB) $(PREFIX)/lib/libmir.dylib
	install_name_tool -change "$(SOLIB)" "$(PREFIX)/lib/$(SOLIB)" $(PREFIX)/lib/$(SOLIB)
    else
	rm -f $(PREFIX)/lib/$(SONAME)
	ln -s $(PREFIX)/lib/$(SOLIB) $(PREFIX)/lib/$(SONAME)
    endif
endif
	install -m a+rx $(EXECUTABLES) $(PREFIX)/bin

$(PREFIX)/include $(PREFIX)/lib $(PREFIX)/bin:
	   mkdir -p $@

uninstall: $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB) $(EXECUTABLES) | $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/bin
	$(RM) $(PREFIX)/include/mir.h $(PREFIX)/include/mir-dlist.h $(PREFIX)/include/mir-varr.h $(PREFIX)/include/mir-htab.h\
		       $(PREFIX)/include/mir-gen.h $(PREFIX)/include/c2mir.h
	$(RM) $(PREFIX)/lib/libmir.$(LIBSUFF) $(PREFIX)/lib/$(SOLIB)
ifeq ($(OS),Windows_NT)
else
    ifeq ($(UNAME_S),Darwin)
	rm -f $(PREFIX)/lib/libmir.dylib
    else
	rm -f $(PREFIX)/lib/$(SONAME)
    endif
endif
	$(RM) $(EXECUTABLES:$(BUILD_DIR)/%=$(PREFIX)/bin/%)
	-rmdir $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/bin
	-rmdir $(PREFIX)

clean: clean-mir clean-c2m clean-utils clean-l2m clean-adt-tests clean-mir-tests clean-mir2c-test clean-bench
	$(RM) $(EXECUTABLES) $(BUILD_DIR)/libmir.$(LIBSUFF) $(BUILD_DIR)/$(SOLIB)

test: readme-example-test mir-bin-run-test c2mir-test

test-all: adt-test simplify-test io-test scan-test mir2c-test $(L2M-TEST) test

bench: interp-bench gen-bench gen-bench2 io-bench mir2c-bench c2mir-sieve-bench gen-speed c2mir-bench
	@echo ==============================Bench is done

# ------------------ MIR --------------------------
MIR_SRC:=$(SRC_DIR)/mir.c $(SRC_DIR)/mir-gen.c
MIR_BUILD:=$(MIR_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/%.$(OBJSUFF): $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(COMPILE) -c $< $(OBJO)$@

.PHONY: clean-mir
clean-mir:
	$(RM) $(MIR_BUILD) $(MIR_BUILD:.$(OBJSUFF)=.d)

-include $(MIR_BUILD:.$(OBJSUFF)=.d)

# ------------------ LIBMIR -----------------------
$(BUILD_DIR)/libmir.$(LIBSUFF): $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(BUILD_DIR)/c2mir/c2mir.$(OBJSUFF)
ifeq ($(CC),cl)
	lib -nologo $^ -OUT:$@
else
	$(AR) rcs $@ $^
endif

# ------------------ LIBMIR SO --------------------
$(BUILD_DIR)/$(SOLIB): $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(BUILD_DIR)/c2mir/c2mir.$(OBJSUFF)
ifeq ($(CC),cl)
	$(CC) -nologo -D_USRDLL -D_WINDLL $^ -link -DLL -OUT:$@
else
	$(CC) $(SOLIBFLAGS) -o $@ $^
endif

# ------------------ C2M --------------------------
C2M_SRC:=$(SRC_DIR)/c2mir/c2mir.c $(SRC_DIR)/c2mir/c2mir-driver.c
C2M_BUILD:=$(C2M_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/c2mir/%.$(OBJSUFF): $(SRC_DIR)/c2mir/%.c | $(BUILD_DIR)/c2mir
	$(COMPILE) -c $< $(OBJO)$@

$(BUILD_DIR)/c2m$(EXE): $(C2M_BUILD) $(BUILD_DIR)/libmir.$(LIBSUFF) | $(BUILD_DIR)
	$(LINK) $^ $(LDLIBS) $(EXEO)$@

$(BUILD_DIR)/c2mir:
	   mkdir -p $@

.PHONY: clean-c2m
clean-c2m:
	$(RM) $(C2M_BUILD) $(C2M_BUILD:.$(OBJSUFF)=.d)

-include $(C2M_BUILD:.$(OBJSUFF)=.d)

# ------------------ MIR RUN ----------------------

MIR_RUN_SRC:=$(SRC_DIR)/mir-bin-run.c
MIR_RUN_BUILD:=$(MIR_RUN_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/mir-bin-run$(EXE): $(MIR_RUN_BUILD) $(BUILD_DIR)/libmir.$(LIBSUFF) | $(BUILD_DIR)
	$(LINK) $^ $(LDLIBS) $(EXEO)$@ $(BUILD_DIR)/libmir.$(LIBSUFF)

.PHONY: clean-mir-bin-run
clean-mir-bin-run:
	$(RM) $(MIR_RUN_BUILD) $(MIR_RUN_BUILD:.$(OBJSUFF)=.d)

-include $(MIR_RUN_BUILD:.$(OBJSUFF)=.d)

# ------------------ L2M --------------------------
L2M_SRC:=$(SRC_DIR)/llvm2mir/llvm2mir.c $(SRC_DIR)/llvm2mir/llvm2mir-driver.c
L2M_BUILD:=$(L2M_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/llvm2mir/%.$(OBJSUFF): $(SRC_DIR)/llvm2mir/%.c | $(BUILD_DIR)/llvm2mir
	$(COMPILE) -c $< $(OBJO)$@

$(BUILD_DIR)/l2m$(EXE): $(L2M_BUILD) $(BUILD_DIR)/libmir.$(LIBSUFF) | $(BUILD_DIR)
	$(LINK) $^ $(LDLIBS) -lLLVM $(OBJO)$@ $(BUILD_DIR)/libmir.$(LIBSUFF)

$(BUILD_DIR)/llvm2mir:
	   mkdir -p $@

.PHONY: clean-l2m
clean-l2m:
	$(RM) $(L2M_BUILD) $(L2M_BUILD:.$(OBJSUFF)=.d)

-include $(L2M_BUILD:.$(OBJSUFF)=.d)

# ------------------ Common for utils -------------

$(BUILD_DIR)/mir-utils:
	   mkdir -p $@

$(BUILD_DIR)/mir-utils/%.$(OBJSUFF): $(SRC_DIR)/mir-utils/%.c | $(BUILD_DIR)/mir-utils
	$(COMPILE) -c $< $(OBJO)$@

.PHONY: clean-utils
clean-utils: clean-m2b clean-b2m clean-b2ctab

# ------------------ M2B --------------------------
M2B_SRC:=$(SRC_DIR)/mir-utils/m2b.c
M2B_BUILD:=$(M2B_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/m2b$(EXE): $(M2B_BUILD) $(BUILD_DIR)/libmir.$(LIBSUFF) | $(BUILD_DIR)
	$(LINK) $^ $(LDLIBS) $(EXEO)$@ $(BUILD_DIR)/libmir.$(LIBSUFF)

.PHONY: clean-m2b
clean-m2b:
	$(RM) $(M2B_BUILD) $(M2B_BUILD:.$(OBJSUFF)=.d)

-include $(M2B_BUILD:.$(OBJSUFF)=.d)

# ------------------ B2M --------------------------
B2M_SRC:=$(SRC_DIR)/mir-utils/b2m.c
B2M_BUILD:=$(B2M_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/b2m$(EXE): $(B2M_BUILD) $(BUILD_DIR)/libmir.$(LIBSUFF) | $(BUILD_DIR)
	$(LINK) $^ $(LDLIBS) $(EXEO)$@ $(BUILD_DIR)/libmir.$(LIBSUFF)

.PHONY: clean-b2m
clean-b2m:
	$(RM) $(B2M_BUILD) $(B2M_BUILD:.$(OBJSUFF)=.d)

-include $(B2M_BUILD:.$(OBJSUFF)=.d)

# ------------------ B2CTAB --------------------------
B2CTAB_SRC:=$(SRC_DIR)/mir-utils/b2ctab.c
B2CTAB_BUILD:=$(B2CTAB_SRC:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.$(OBJSUFF))

$(BUILD_DIR)/b2ctab$(EXE): $(B2CTAB_BUILD) $(BUILD_DIR)/libmir.$(LIBSUFF) | $(BUILD_DIR)
	$(LINK) $^ $(LDLIBS) $(EXEO)$@  $(BUILD_DIR)/libmir.$(LIBSUFF)

.PHONY: clean-b2ctab
clean-b2ctab:
	$(RM) $(B2CTAB_BUILD) $(B2CTAB_BUILD:.$(OBJSUFF)=.d)

-include $(B2CTAB_BUILD:.$(OBJSUFF)=.d)

# ------------------ ADT tests --------------------------

.PHONY: clean-adt-tests
.PHONY: adt-test varr-test dlist-test bitmap-test htab-test reduce-test

adt-test: varr-test dlist-test bitmap-test htab-test reduce-test

varr-test: $(BUILD_DIR)/adt-tests
	$(COMPILE_AND_LINK) $(SRC_DIR)/adt-tests/mir-varr-test.c $(EXEO)$(BUILD_DIR)/adt-tests/varr-test$(EXE)
	$(BUILD_DIR)/adt-tests/varr-test$(EXE)

dlist-test: $(BUILD_DIR)/adt-tests
	$(COMPILE_AND_LINK) $(SRC_DIR)/adt-tests/mir-dlist-test.c $(EXEO)$(BUILD_DIR)/adt-tests/dlist-test$(EXE)
	$(BUILD_DIR)/adt-tests/dlist-test$(EXE)

bitmap-test: $(BUILD_DIR)/adt-tests
	$(COMPILE_AND_LINK) $(SRC_DIR)/adt-tests/mir-bitmap-test.c $(EXEO)$(BUILD_DIR)/adt-tests/bitmap-test$(EXE)
	$(BUILD_DIR)/adt-tests/bitmap-test$(EXE)

htab-test: $(BUILD_DIR)/adt-tests
	$(COMPILE_AND_LINK) $(SRC_DIR)/adt-tests/mir-htab-test.c $(EXEO)$(BUILD_DIR)/adt-tests/htab-test$(EXE)
	$(BUILD_DIR)/adt-tests/htab-test$(EXE)

reduce-test: $(BUILD_DIR)/adt-tests
	$(COMPILE_AND_LINK) $(SRC_DIR)/adt-tests/mir-reduce-test.c $(EXEO)$(BUILD_DIR)/adt-tests/reduce-test$(EXE)
	$(BUILD_DIR)/adt-tests/reduce-test$(EXE) $(SRC_DIR)/c2mir/c2mir.c

$(BUILD_DIR)/adt-tests:
	mkdir -p $@

clean-adt-tests:
	$(RM) $(BUILD_DIR)/adt-tests/varr-test$(EXE) $(BUILD_DIR)/adt-tests/dlist-test$(EXE)
	$(RM) $(BUILD_DIR)/adt-tests/bitmap-test$(EXE)
	$(RM) $(BUILD_DIR)/adt-tests/htab-test$(EXE) $(BUILD_DIR)/adt-tests/reduce-test$(EXE)

# ------------------ Common for MIR tests ---------------------

$(BUILD_DIR)/mir-tests:
	mkdir -p $@

$(BUILD_DIR)/run-test$(EXE): $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/run-test.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) $^ $(LDLIBS) $(EXEO)$@

.PHONY: clean-mir-tests
clean-mir-tests: clean-mir-utility-tests clean-mir-interp-tests clean-mir-gen-tests clean-readme-example-test
	$(RM) $(BUILD_DIR)/run-test$(EXE)

# ------------------ MIR utility tests ------------------------

.PHONY: simplify-test scan-test io-test clean-mir-utility-tests

simplify-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/simplify.c
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/simplify-test $(LDLIBS) && $(BUILD_DIR)/simplify-test$(EXE)

hello-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/hello.c
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/hello-test $(LDLIBS) && $(BUILD_DIR)/hello-test$(EXE)

scan-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/scan-test.c
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/scan-test $(LDLIBS) && $(BUILD_DIR)/scan-test$(EXE)

io-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/io.c
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/io-test $(LDLIBS) && $(BUILD_DIR)/io-test$(EXE)

clean-mir-utility-tests:
	$(RM) $(BUILD_DIR)/run-test$(EXE) $(BUILD_DIR)/simplify-test$(EXE)
	$(RM) $(BUILD_DIR)/hello-test$(EXE)
	$(RM) $(BUILD_DIR)/scan-test$(EXE) $(BUILD_DIR)/io-test$(EXE)

# ------------------ MIR interp tests --------------------------

.PHONY: clean-mir-interp-tests
.PHONY: interp-test interp-test1 interp-test2 interp-test3 interp-test4 interp-test5 interp-test6 interp-test7
.PHONY: interp-test8 interp-test9 interp-test10 interp-test11 interp-test12 interp-test13 interp-test14
.PHONY: interp-test15 interp-test16

interp-test: interp-test1 interp-test2 interp-test3 interp-test4 interp-test5 interp-test6 interp-test7\
	     interp-test8 interp-test9 interp-test10 interp-test11 interp-test12 interp-test13 interp-test14\
	     interp-test15 interp-test16

interp-test1: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DMIR_INTERP_DEBUG=1 $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test1$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test1$(EXE)

interp-test2: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DMIR_INTERP_DEBUG=1 -DMIR_C_INTERFACE=1 $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test2$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test2$(EXE)

interp-test3: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/sieve-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DMIR_INTERP_DEBUG=1 $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test3$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test3$(EXE)

interp-test4:  $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/sieve-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DMIR_INTERP_DEBUG=1 -DMIR_C_INTERFACE=1 $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test4$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test4$(EXE)

interp-test5: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/hi-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DMIR_INTERP_DEBUG=1 $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test5$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test5$(EXE)

interp-test6: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/args-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test6$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test6$(EXE)

interp-test7: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/args-interp.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DMIR_C_INTERFACE=1 $^ $(EXEO)$(BUILD_DIR)/mir-tests/interp-test7$(EXE)
	$(BUILD_DIR)/mir-tests/interp-test7$(EXE)

interp-test8: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test8.mir

interp-test9: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test9.mir

interp-test10: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test10.mir

interp-test11: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test11.mir

interp-test12: $(BUILD_DIR)/run-test$(EXE)
ifeq ($(OS),Windows_NT)
	echo Skipping test with multiple returns
else
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test12.mir
endif

interp-test13: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test13.mir

interp-test14: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test14.mir

interp-test15: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test15.mir

interp-test16: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -i $(SRC_DIR)/mir-tests/test16.mir

clean-mir-interp-tests:
	$(RM) $(BUILD_DIR)/mir-tests/interp-test1$(EXE) $(BUILD_DIR)/mir-tests/interp-test2$(EXE)
	$(RM) $(BUILD_DIR)/mir-tests/interp-test3$(EXE) $(BUILD_DIR)/mir-tests/interp-test4$(EXE)
	$(RM) $(BUILD_DIR)/mir-tests/interp-test5$(EXE) $(BUILD_DIR)/mir-tests/interp-test6$(EXE)
	$(RM) $(BUILD_DIR)/mir-tests/interp-test7$(EXE)

# ------------------ MIR gen tests --------------------------

.PHONY: clean-mir-gen-tests
.PHONY: gen-test gen-test-loop gen-test-sieve gen-issue219-test gen-test-get-thunk-addr
.PHONY: gen-test1 gen-test2 gen-test3 gen-test4 gen-test5 gen-test6 gen-test7
.PHONY: gen-test8 gen-test9 gen-test10 gen-test11 gen-test12 gen-test13 gen-test14 gen-test15 gen-test16

gen-test: gen-test-loop gen-test-sieve gen-test-get-thunk-addr gen-issue219-test gen-test1 gen-test2 gen-test3 gen-test4 gen-test5 gen-test6 gen-test7\
          gen-test8 gen-test9 gen-test10 gen-test11 gen-test12 gen-test13 gen-test14 gen-test15 gen-test16

gen-test-loop: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-sieve-gen.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DTEST_GEN_LOOP -DTEST_GEN_DEBUG=1 $^ $(LDLIBS) $(EXEO)$(BUILD_DIR)/mir-tests/gen-loop-test$(EXE)
	$(BUILD_DIR)/mir-tests/gen-loop-test

gen-test-sieve: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-sieve-gen.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DTEST_GEN_SIEVE -DTEST_GEN_DEBUG=1 $^ $(LDLIBS) $(EXEO)$(BUILD_DIR)/mir-tests/gen-sieve-test$(EXE)
	$(BUILD_DIR)/mir-tests/gen-sieve-test

gen-issue219-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/issue219.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) -DTEST_GEN_SIEVE -DTEST_GEN_DEBUG=1 $^ $(LDLIBS) $(EXEO)$(BUILD_DIR)/mir-tests/issue219$(EXE)
	$(BUILD_DIR)/mir-tests/issue219

gen-test-get-thunk-addr: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/get-thunk-addr.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) $^ $(LDLIBS) $(EXEO)$(BUILD_DIR)/mir-tests/gen-get-thunk-addr-test$(EXE)
	$(BUILD_DIR)/mir-tests/gen-get-thunk-addr-test

gen-test1: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test1.mir

gen-test2: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test2.mir

gen-test3: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test3.mir

gen-test4: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test4.mir

gen-test5: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test5.mir

gen-test6: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test6.mir

gen-test7: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -d $(SRC_DIR)/mir-tests/test7.mir

gen-test8: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -g $(SRC_DIR)/mir-tests/test8.mir

gen-test9: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -gd $(SRC_DIR)/mir-tests/test9.mir

gen-test10: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -g $(SRC_DIR)/mir-tests/test10.mir

gen-test11: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -gd $(SRC_DIR)/mir-tests/test11.mir

gen-test12: $(BUILD_DIR)/run-test$(EXE)
ifeq ($(OS),Windows_NT)
	echo Skipping test with multiple returns
else
	$(BUILD_DIR)/run-test$(EXE) -gd $(SRC_DIR)/mir-tests/test12.mir
endif

gen-test13: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -g $(SRC_DIR)/mir-tests/test13.mir

gen-test14: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -g $(SRC_DIR)/mir-tests/test14.mir

gen-test15: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -g $(SRC_DIR)/mir-tests/test15.mir

gen-test16: $(BUILD_DIR)/run-test$(EXE)
	$(BUILD_DIR)/run-test$(EXE) -g $(SRC_DIR)/mir-tests/test16.mir

clean-mir-gen-tests:
	$(RM) $(BUILD_DIR)/mir-tests/gen-loop-test$(EXE) $(BUILD_DIR)/mir-tests/gen-sieve-test$(EXE)
	$(RM) $(BUILD_DIR)/mir-tests/issue219$(EXE) $(BUILD_DIR)/mir-tests/gen-get-thunk-addr-test

# ------------------ MIR run tests --------------------------

mir-bin-run-test: $(BUILD_DIR)/mir-bin-run$(EXE) $(BUILD_DIR)/c2m$(EXE)
	      $(BUILD_DIR)/c2m$(EXE) -c $(SRC_DIR)/sieve.c
	      $(BUILD_DIR)/mir-bin-run$(EXE) `pwd`/sieve.bmir sieve.bmir
	      MIR_TYPE=interp $(BUILD_DIR)/mir-bin-run$(EXE) `pwd`/sieve.bmir sieve.bmir
	      MIR_TYPE=gen $(BUILD_DIR)/mir-bin-run$(EXE) `pwd`/sieve.bmir sieve.bmir
	      MIR_TYPE=lazy $(BUILD_DIR)/mir-bin-run$(EXE) `pwd`/sieve.bmir sieve.bmir
	      rm sieve.bmir
	

# ------------------ readme example test ----------------

.PHONY: readme-example-test clean-readme-example-test

readme-example-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF)\
                           $(SRC_DIR)/mir-tests/readme-example.c | $(BUILD_DIR)/mir-tests
	$(COMPILE_AND_LINK) $^ $(LDLIBS) $(EXEO)$(BUILD_DIR)/mir-tests/readme-example-test$(EXE)
	$(BUILD_DIR)/mir-tests/readme-example-test$(EXE)

clean-readme-example-test:
	$(RM) $(BUILD_DIR)/mir-tests/readme-example-test$(EXE)

# ------------------ mir2c test -------------------------

.PHONY: mir2c-test clean-mir2c-test

mir2c-test: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir2c/mir2c.c
	$(COMPILE_AND_LINK) -DTEST_MIR2C $^ $(EXEO)$(BUILD_DIR)/mir2c-test $(LDLIBS) && $(BUILD_DIR)/mir2c-test$(EXE)

clean-mir2c-test:
	$(RM) $(BUILD_DIR)/mir2c-test$(EXE)

# ------------------ c2m tests --------------------------

.PHONY: c2mir-test c2mir-simple-test c2mir-full-test c2mir-interp-test
.PHONY: c2mir-gen-test c2mir-parallel-gen-test c2mir-gen-test0 c2mir-gen-test1 c2mir-gen-test3

c2mir-test: c2mir-simple-test c2mir-full-test

c2mir-simple-test: $(BUILD_DIR)/c2m$(EXE)
	$(BUILD_DIR)/c2m$(EXE) -v $(SRC_DIR)/sieve.c -ei

c2mir-full-test: c2mir-interp-test c2mir-gen-test c2mir-bb-gen-test c2mir-gen-test0 c2mir-gen-test1 c2mir-gen-test3 c2mir-bootstrap

c2mir-interp-test: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-interp $(BUILD_DIR)/c2m$(EXE)
c2mir-gen-test: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-gen $(BUILD_DIR)/c2m$(EXE)
c2mir-bb-gen-test: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-gen-bb $(BUILD_DIR)/c2m$(EXE)
c2mir-parallel-gen-test: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-parallel-gen $(BUILD_DIR)/c2m$(EXE)
c2mir-gen-test0: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-gen-O0 $(BUILD_DIR)/c2m$(EXE)
c2mir-gen-test1: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-gen-O1 $(BUILD_DIR)/c2m$(EXE)
c2mir-gen-test3: $(BUILD_DIR)/c2m$(EXE)
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-c2m-gen-O3 $(BUILD_DIR)/c2m$(EXE)

# ------------------ c2m bootstrap tests ----------------

.PHONY: c2mir-bootstrap c2mir-bootstrap-test0 c2mir-bootstrap-test1 c2mir-bootstrap-test c2mir-bootstrap-test3
.PHONY: c2mir-parallel-bootstrap-test c2mir-bootstrap-test4 c2mir-bootstrap-test5

c2mir-bootstrap: c2mir-bootstrap-test c2mir-bootstrap-test0 c2mir-bootstrap-test1 c2mir-bootstrap-test3 c2mir-parallel-bootstrap-test c2mir-bb-bootstrap-test

c2mir-bootstrap-test0: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap lazy func test with -O0 '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS) -O0 -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/1o0.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) $(C2M_BOOTSTRAP_FLAGS) -O0 $(BUILD_DIR)/1o0.bmir -el -w $(C2M_BOOTSTRAP_FLAGS) -O0\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/2o0.bmir
	$(Q) cmp  $(BUILD_DIR)/1o0.bmir $(BUILD_DIR)/2o0.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/1o0.bmir $(BUILD_DIR)/2o0.bmir

c2mir-bootstrap-test1: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap lazy func test with -O1 '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS) -O1 -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/1o1.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) $(C2M_BOOTSTRAP_FLAGS) -O1 $(BUILD_DIR)/1o1.bmir -el -w $(C2M_BOOTSTRAP_FLAGS) -O1\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/2o1.bmir
	$(Q) cmp $(BUILD_DIR)/1o1.bmir $(BUILD_DIR)/2o1.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/1o1.bmir $(BUILD_DIR)/2o1.bmir

c2mir-bootstrap-test: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap lazy func test with default optimize level '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS) -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/1o2.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) $(C2M_BOOTSTRAP_FLAGS) $(BUILD_DIR)/1o2.bmir -el -w $(C2M_BOOTSTRAP_FLAGS)\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			     $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/2o2.bmir
	$(Q) cmp $(BUILD_DIR)/1o2.bmir $(BUILD_DIR)/2o2.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/1o2.bmir $(BUILD_DIR)/2o2.bmir

c2mir-bb-bootstrap-test: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap lazy bb test with default optimize level '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS) -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/1o2.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) $(C2M_BOOTSTRAP_FLAGS) $(BUILD_DIR)/1o2.bmir -p4 -eb -w $(C2M_BOOTSTRAP_FLAGS)\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			     $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/2o2.bmir
	$(Q) cmp $(BUILD_DIR)/1o2.bmir $(BUILD_DIR)/2o2.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/1o2.bmir $(BUILD_DIR)/2o2.bmir

c2mir-bootstrap-test3: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap lazy func test with -O3 '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS) -O3 -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/1o3.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) $(C2M_BOOTSTRAP_FLAGS) -O3 $(BUILD_DIR)/1o3.bmir -el -w $(C2M_BOOTSTRAP_FLAGS) -O3\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/2o3.bmir
	$(Q) cmp $(BUILD_DIR)/1o3.bmir $(BUILD_DIR)/2o3.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/1o3.bmir $(BUILD_DIR)/2o3.bmir

c2mir-parallel-bootstrap-test: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Parallel Bootstrap Test with default optimize level '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w -p4 $(C2M_BOOTSTRAP_FLAGS) -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/1p2.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) -p4 $(C2M_BOOTSTRAP_FLAGS) $(BUILD_DIR)/1p2.bmir -eg -w -p4 $(C2M_BOOTSTRAP_FLAGS)\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/2p2.bmir
	$(Q) cmp $(BUILD_DIR)/1p2.bmir $(BUILD_DIR)/2p2.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/1p2.bmir $(BUILD_DIR)/2p2.bmir

c2mir-bootstrap-test4: $(BUILD_DIR)/c2m$(EXE) $(BUILD_DIR)/b2ctab$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap Test 2 '(usually it takes about 10-20 sec) ... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS) -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/t1.bmir
	$(Q) $(BUILD_DIR)/b2ctab$(EXE) < $(BUILD_DIR)/t1.bmir > $(BUILD_DIR)/mir-ctab
	$(Q) $(COMPILE_AND_LINK) -w $(SRC_DIR)/mir.c $(SRC_DIR)/mir-gen.c $(SRC_DIR)/mir-bin-driver.c\
	                         $(LDLIBS) -o $(BUILD_DIR)/c2m-test$(EXE)
	$(Q) $(BUILD_DIR)/c2m-test$(EXE) $(C2M_BOOTSTRAP_FLAGS) -w -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c\
	                         $(SRC_DIR)/c2mir/c2mir.c $(SRC_DIR)/c2mir/c2mir-driver.c\
				 $(SRC_DIR)/mir.c -o $(BUILD_DIR)/t2.bmir
	$(Q) cmp $(BUILD_DIR)/t1.bmir $(BUILD_DIR)/t2.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/t1.bmir $(BUILD_DIR)/t2.bmir $(BUILD_DIR)/mir-ctab $(BUILD_DIR)/c2m-test$(EXE)

c2mir-bootstrap-test5: $(BUILD_DIR)/c2m$(EXE)
	$(Q) echo -n +++++++ C2MIR Bootstrap Interpreter Test '... '
	$(Q) $(BUILD_DIR)/c2m$(EXE) -w $(C2M_BOOTSTRAP_FLAGS0) -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c\
	                    $(SRC_DIR)/c2mir/c2mir.c $(SRC_DIR)/c2mir/c2mir-driver.c\
			    $(SRC_DIR)/mir.c -o $(BUILD_DIR)/i1.bmir
	$(Q) $(BUILD_DIR)/c2m$(EXE) $(C2M_BOOTSTRAP_FLAGS0) $(BUILD_DIR)/i1.bmir -ei -w $(C2M_BOOTSTRAP_FLAGS0)\
	                    -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
			    $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -o $(BUILD_DIR)/i2.bmir
	$(Q) cmp $(BUILD_DIR)/i1.bmir $(BUILD_DIR)/i2.bmir && echo Passed || echo FAIL
	$(Q) rm -rf $(BUILD_DIR)/i1.bmir $(BUILD_DIR)/i2.bmir

# ------------------ l2m tests --------------------------

.PHONY:  l2m-test l2m-simple-test l2m-full-test l2m-interp-test l2m-gen-test l2m-test1 l2m-test2

l2m-test: l2m-simple-test # l2m-full-test

l2m-simple-test: l2m-test1 l2m-test2

l2m-full-test: l2m-interp-test l2m-gen-test

l2m-interp-test: $(BUILD_DIR)/l2m
	$(SHELL) c-tests/runtests.sh c-tests/use-l2m-interp $(BUILD_DIR)/c2m
l2m-gen-test: $(BUILD_DIR)/l2m
	$(SHELL) c-tests/runtests.sh c-tests/use-l2m-gen $(BUILD_DIR)/c2m

l2m-test1: $(BUILD_DIR)/l2m
	@echo +++++ LLVM to MIR translator test '(-O0)' +++++++
	clang -O0 -fno-vectorize -w -c -emit-llvm $(SRC_DIR)/sieve.c -o $(BUILD_DIR)/sieve.bc
	@echo +++++ Interpreter +++++++ && $(BUILD_DIR)/l2m -i $(BUILD_DIR)/sieve.bc
	@echo +++++ Generator +++++++ && $(BUILD_DIR)/l2m -g $(BUILD_DIR)/sieve.bc

l2m-test2: $(BUILD_DIR)/l2m
	@echo +++++ LLVM to MIR translator test '(-O2)' +++++++
	clang -O2 -fno-vectorize -w -c -emit-llvm $(SRC_DIR)/sieve.c -o $(BUILD_DIR)/sieve.bc
	@echo +++++ Interpreter +++++++ && $(BUILD_DIR)/l2m -i $(BUILD_DIR)/sieve.bc
	@echo +++++ Generator +++++++ && $(BUILD_DIR)/l2m -g $(BUILD_DIR)/sieve.bc

# ------------------ benchmarks -------------------------

.PHONY: clean-bench
.PHONY: io-bench interp-bench gen-bench gen-bench2 gen-speed c2mir-sieve-bench c2mir-bench mir2c-bench

io-bench: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/io-bench.c
	@echo ========io-bench can take upto 2 min===============
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/io-bench $(LDLIBS) && $(BUILD_DIR)/io-bench$(EXE)

interp-bench: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-interp.c
	$(COMPILE_AND_LINK) -DMIR_C_INTERFACE=1 $^ $(EXEO)$(BUILD_DIR)/interp-bench$(EXE) $(LDLIBS)
	$(BUILD_DIR)/interp-bench && size $(BUILD_DIR)/interp-bench
	$(COMPILE_AND_LINK) $^ $(EXEO)$(BUILD_DIR)/interp-bench$(EXE) $(LDLIBS)
	$(BUILD_DIR)/interp-bench && size $(BUILD_DIR)/interp-bench

gen-bench: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-sieve-gen.c
	$(COMPILE_AND_LINK) -DTEST_GEN_LOOP $^ $(EXEO)$(BUILD_DIR)/gen-bench$(EXE) $(LDLIBS)
	$(BUILD_DIR)/gen-bench && size $(BUILD_DIR)/gen-bench
	$(COMPILE_AND_LINK) -DTEST_GEN_SIEVE $^ $(EXEO)$(BUILD_DIR)/gen-bench$(EXE) $(LDLIBS)
	$(BUILD_DIR)/gen-bench && size $(BUILD_DIR)/gen-bench

gen-bench2: $(BUILD_DIR)/c2m # Ignore M1 MacOs as it needs another procedure to make code executable
	@if test $(UNAME_S) != Darwin || test $(UNAME_M) != arm64; then\
	  echo +++++ Compiling and generating all code for c2m: +++++;\
	  for i in 0 1 2 3;do \
	    echo === Optimization level $$i:;\
        echo 'int main () {return 0;}' > __a.c;\
	    time $(BUILD_DIR)/c2m -O$$i -Dx86_64 -I$(SRC_DIR) $(SRC_DIR)/mir-gen.c $(SRC_DIR)/c2mir/c2mir.c\
	                       $(SRC_DIR)/c2mir/c2mir-driver.c $(SRC_DIR)/mir.c -el -i -o __a.bmir < __a.c;\
	    rm -f __a.c __a.bmir;\
	  done;\
	fi

gen-speed: $(BUILD_DIR)/mir.$(OBJSUFF) $(BUILD_DIR)/mir-gen.$(OBJSUFF) $(SRC_DIR)/mir-tests/loop-sieve-gen.c
	if type valgrind  > /dev/null 2>&1; then \
	  $(COMPILE_AND_LINK) -DTEST_GEN_SIEVE -DTEST_GENERATION_ONLY $^ $(LDLIBS) $(EXEO)$(BUILD_DIR)/gen-speed$(EXE)\
	  && valgrind --tool=lackey $(BUILD_DIR)/gen-speed; \
	fi

c2mir-sieve-bench: $(BUILD_DIR)/c2m
	$(BUILD_DIR)/c2m -DSIEVE_BENCH -v $(SRC_DIR)/sieve.c -eg && size $(BUILD_DIR)/c2m

c2mir-bench: $(BUILD_DIR)/c2m
	$(SRC_DIR)/c-benchmarks/run-benchmarks.sh

c2mir-bench-short: $(BUILD_DIR)/c2m
	$(SRC_DIR)/c-benchmarks/run-benchmarks.sh short

mir2c-bench: $(BUILD_DIR)/mir.$(OBJSUFF) $(SRC_DIR)/mir2c/mir2c.c
	$(COMPILE_AND_LINK) -DTEST_MIR2C $^ $(EXEO)$(BUILD_DIR)/mir2c-bench$(EXE) $(LDLIBS)
	$(BUILD_DIR)/mir2c-bench -v && size $(BUILD_DIR)/mir2c-bench

clean-bench:
	$(RM) $(BUILD_DIR)/io-bench$(EXE) $(BUILD_DIR)/interp-bench$(EXE) $(BUILD_DIR)/gen-bench$(EXE)
	$(RM) $(BUILD_DIR)/gen-speed$(EXE) $(BUILD_DIR)/mir2c-bench$(EXE)

# ------------------ miscellaneous ----------------------

.PHONY: sloc gcc-test clang-test

sloc:
	@echo -n 'C2MIR: ' && wc -l $(SRC_DIR)/c2mir/c2mir.c | awk '{last=$$1} END {print last}'
	@echo -n 'ADT: ' && wc -l $(SRC_DIR)/mir-dlist.h $(SRC_DIR)/mir-hash.h $(SRC_DIR)/mir-htab.h\
	                          $(SRC_DIR)/mir-varr.h $(SRC_DIR)/mir-reduce.h $(SRC_DIR)/mir-bitmap.h\
				  | awk '{last=$$1} END {print last}'
	@echo -n 'MIR API: ' && wc -l $(SRC_DIR)/mir.[ch] | awk '{last=$$1} END {print last}'
	@echo -n 'MIR Interpreter: ' && wc -l $(SRC_DIR)/mir-interp.c | awk '{last=$$1} END {print last}'
	@echo -n 'MIR Generator: ' && wc -l $(SRC_DIR)/mir-gen.[ch] | awk '{last=$$1} END {print last}'
	@echo -n 'x86-64 machine dependent code: ' && wc -l $(SRC_DIR)/mir-x86_64.c $(SRC_DIR)/mir-gen-x86_64.c\
	| awk '{last=$$1} END {print last}'
	@echo -n 'aarch64 machine dependent code: ' && wc -l $(SRC_DIR)/mir-aarch64.c $(SRC_DIR)/mir-gen-aarch64.c\
	| awk '{last=$$1} END {print last}'
	@echo -n 'ppc64 machine dependent code: ' && wc -l $(SRC_DIR)/mir-ppc64.c $(SRC_DIR)/mir-gen-ppc64.c\
	| awk '{last=$$1} END {print last}'
	@echo -n 's390x machine dependent code: ' && wc -l $(SRC_DIR)/mir-s390x.c $(SRC_DIR)/mir-gen-s390x.c\
	| awk '{last=$$1} END {print last}'
	@echo -n 'riscv64 machine dependent code: ' && wc -l $(SRC_DIR)/mir-riscv64.c $(SRC_DIR)/mir-gen-riscv64.c\
	| awk '{last=$$1} END {print last}'
	@echo -n 'Overall: ' && wc -l $(SRC_DIR)/c2mir/c2mir.c $(SRC_DIR)/mir-dlist.h $(SRC_DIR)/mir-hash.h\
	                              $(SRC_DIR)/mir-htab.h $(SRC_DIR)/mir-varr.h $(SRC_DIR)/mir-reduce.h\
				      $(SRC_DIR)/mir-bitmap.h $(SRC_DIR)/mir.[ch]\
				      $(SRC_DIR)/mir-interp.c $(SRC_DIR)/mir-gen.[ch] $(SRC_DIR)/mir-x86_64.c\
				      $(SRC_DIR)/mir-gen-x86_64.c $(SRC_DIR)/mir-aarch64.c\
				      $(SRC_DIR)/mir-gen-aarch64.c $(SRC_DIR)/mir-ppc64.c $(SRC_DIR)/mir-gen-ppc64.c\
				      $(SRC_DIR)/mir-riscv64.c $(SRC_DIR)/mir-gen-riscv64.c\
				      $(SRC_DIR)/mir-riscv64.c $(SRC_DIR)/mir-gen-riscv64.c\
	| awk '{last=$$1} END {print last}'

gcc-test:
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-gcc

clang-test:
	$(SHELL) $(SRC_DIR)/c-tests/runtests.sh $(SRC_DIR)/c-tests/use-clang

# Comparison of gcc, c2m, wasmer, wasmtime on ogg encoding.  You need wasi-clang script calling clang from wasi-sdk.
oggenc-bench:
	$(SHELL) $(SRC_DIR)/c-benchmarks/run-oggenc.sh

# Very long testing (hours and hours):
csmith: csmith-c2m-gcc csmith-c2m

csmith-c2m-gcc:
	$(SHELL) csmith-c2m-gcc.sh

csmith-c2m:
	$(SHELL) csmith-c2m.sh
