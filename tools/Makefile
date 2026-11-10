# Makefile for n12-monterey.
#
# Builds every command-line tool from source. Nothing here is signed or
# installed system-wide; `make` just produces the executables next to the
# sources, and `install.sh` (see INSTALL.md) is the friendly front end.
#
# Requirements: the Xcode command line tools (they provide clang). Check with
#     xcode-select -p
#
# Usage:
#     make            # build all tools
#     make n12_agent  # build just one
#     make clean      # remove the built tools

CC      ?= clang
CFLAGS  ?= -Wall -O2

# Each macOS framework a tool links against, grouped so the rules stay readable.
FW_COREAUDIO = -framework CoreAudio -framework CoreFoundation
FW_IOKIT     = -framework IOKit -framework CoreFoundation
FW_AUDIOQ    = -framework AudioToolbox -framework CoreAudio -framework CoreFoundation
FW_AUDIOUNIT = -framework AudioUnit -framework CoreAudio -framework CoreFoundation
FW_COREMIDI  = -framework CoreMIDI -framework CoreFoundation
FW_MIDI_IOKIT= -framework IOKit -framework CoreMIDI -framework CoreFoundation
FW_WATCH     = -framework IOKit -framework CoreAudio -framework CoreFoundation

# The runtime parts of the solution (what the user actually needs).
CORE = n12_agent n12_layout n12_default n12_fmt
# Helpers for testing and diagnosing.
TEST = n12_play n12_capture sysplay n12_watch n12_aqtest
# Reverse-engineering / probing tools kept for reference.
RE   = fw_read fw_wtest mlan_probe n12_ports n12_diag n12_midi n12_midi_dev \
       audiovol setout yamaha_fw_start

ALL = $(CORE) $(TEST) $(RE)

.PHONY: all clean
all: $(ALL)

# --- core ---
n12_agent:   n12_agent.c   ; $(CC) $(CFLAGS) -o $@ $< $(FW_MIDI_IOKIT)
n12_layout:  n12_layout.c  ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)
n12_default: n12_default.c ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)
n12_fmt:     n12_fmt.c     ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)

# --- testing / diagnosis ---
n12_play:    n12_play.c    ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)
n12_capture: n12_capture.c ; $(CC) $(CFLAGS) -o $@ $< $(FW_AUDIOQ)
sysplay:     sysplay.c     ; $(CC) $(CFLAGS) -o $@ $< $(FW_AUDIOUNIT)
n12_watch:   n12_watch.c   ; $(CC) $(CFLAGS) -o $@ $< $(FW_WATCH)
n12_aqtest:  n12_aqtest.c  ; $(CC) $(CFLAGS) -o $@ $< $(FW_AUDIOQ)

# --- reverse-engineering / probing ---
fw_read:         fw_read.c         ; $(CC) $(CFLAGS) -o $@ $< $(FW_IOKIT)
fw_wtest:        fw_wtest.c        ; $(CC) $(CFLAGS) -o $@ $< $(FW_IOKIT)
mlan_probe:      mlan_probe.c      ; $(CC) $(CFLAGS) -o $@ $< -framework CoreFoundation
n12_ports:       n12_ports.c       ; $(CC) $(CFLAGS) -o $@ $< $(FW_IOKIT)
n12_diag:        n12_diag.c        ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)
n12_midi:        n12_midi.c        ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREMIDI)
n12_midi_dev:    n12_midi_dev.c    ; $(CC) $(CFLAGS) -o $@ $< $(FW_MIDI_IOKIT)
audiovol:        audiovol.c        ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)
setout:          setout.c          ; $(CC) $(CFLAGS) -o $@ $< $(FW_COREAUDIO)
yamaha_fw_start: yamaha_fw_start.c ; $(CC) $(CFLAGS) -o $@ $< $(FW_IOKIT)

clean:
	rm -f $(ALL)
