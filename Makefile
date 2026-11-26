# FTVIEW.WMF: Wild Commander plugin, the FT812 viewer of TS-Conf (Karabas Go / Mini G, ZX Evolution + VDAC2)
SRC=$(wildcard src/*.c src/*.h src/*.s) $(wildcard vendor/*/*)
BINARY=FTVIEW.WMF
# python with intelhex, z80, Pillow (the tests): make venv
PYTHON?=$(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

all: $(BINARY)

$(BINARY): $(SRC) tools/build.py
	nice -n 19 python3 -I tools/build.py -o $@

# SDRTEST.WMF: board check of the SD reader (diag/sdrdiag.c), not released
diag: build/SDRTEST.WMF

build/SDRTEST.WMF: diag/sdrdiag.c diag/sdrdiag_h.c src/sdr.c src/sdr.h tools/build.py
	nice -n 19 python3 -I tools/build.py --build-dir build/diag --header diag/sdrdiag_h.c -o $@ diag/sdrdiag.c src/sdr.c

venv:
	python3 -m venv .venv
	.venv/bin/pip install -q -r requirements.txt

# needs mkfs.fat (dosfstools) and mtools for the card images
test: $(BINARY)
	@for t in test/test_*.py; do nice -n 19 $(PYTHON) $$t || exit 1; done

clean:
	rm -rf build

.PHONY: all diag venv test clean
