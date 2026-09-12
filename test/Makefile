# Host-native tests for the store-and-forward engine. Needs only cc,
# make and python3; the chaos stage additionally needs mosquitto and
# paho-mqtt (CI installs both; without them `make` stops after the gate
# and `make chaos` says what is missing).
#
#   make          - unit tests, then the branch-coverage gate
#   make chaos    - the full chaos suite against a real mosquitto
#
# saf_core.c is pure logic with every dependency injected, so 02's 100%
# branch gate means something here: every torn-record rejection, every
# backoff rung, every storage refusal must be exercised both ways. The
# instrumented object is compiled inside build/ (02's lesson) so Apple
# and GNU toolchains drop .gcno/.gcda in the same place and the gate
# works on macOS too.

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

BUILD := build
CORE  := ../main/saf_core.c ../main/saf_core.h

.PHONY: all run functional coverage chaos clean

all: run

run: functional coverage
	@echo
	@echo "ALL TEST STAGES PASS"

$(BUILD):
	@mkdir -p $(BUILD)

# ---------------------------------------------------------------- functional
$(BUILD)/test_saf: test_saf.c $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_saf.c ../main/saf_core.c

functional: $(BUILD)/test_saf
	@echo "--- functional: queue, backoff, recovery, torn-write + control"
	./$(BUILD)/test_saf

# ------------------------------------------------------------------- coverage
# --coverage at -O0 so gcov's branch view matches the source. The gate is
# Taken-at-least-once == 100.00% for every branch in saf_core.c.
coverage: | $(BUILD)
	@echo "--- coverage: every branch in saf_core.c must be taken both ways"
	cd $(BUILD) \
	    && $(CC) -O0 --coverage -std=c11 -Wall -Wextra -Werror \
	           -c ../../main/saf_core.c -o cov_core.o \
	    && $(CC) -O0 --coverage -std=c11 -Wall -Wextra -Werror \
	           -c ../test_saf.c -o cov_test.o \
	    && $(CC) --coverage cov_core.o cov_test.o -o cov_test \
	    && ./cov_test > /dev/null \
	    && gcov -b cov_core.gcda > gcov_summary.txt
	@awk -v RS='' '/saf_core\.c/ { found = 1;                                 \
	        if ($$0 ~ /Taken at least once:100\.00%/) ok = 1;                 \
	        print }                                                           \
	    END {                                                                 \
	        if (!found) { print "no gcov stanza for saf_core.c - if this is"; \
	            print "macOS gcov formatting, the Linux CI run is the";       \
	            print "authoritative check"; exit 2 }                         \
	        exit ok ? 0 : 1 }' $(BUILD)/gcov_summary.txt

# ---------------------------------------------------------------------- chaos
# host_device is the engine bound to the file store and a real TCP MQTT
# client; chaos.py owns mosquitto, the cuttable proxy and the verdict.
$(BUILD)/host_device: host_device.c mqtt_mini.c mqtt_mini.h \
                      file_store.c file_store.h $(CORE) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ host_device.c mqtt_mini.c file_store.c \
	    ../main/saf_core.c

chaos: $(BUILD)/host_device
	@echo "--- chaos: broker kills, mid-publish cuts, reboots, power cuts"
	python3 chaos.py

clean:
	rm -rf $(BUILD) run *.gcno *.gcda *.gcov
