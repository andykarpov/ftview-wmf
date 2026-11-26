# FTVIEW.WMF: Wild Commander plugin, the FT812 viewer of TS-Conf (Karabas Go / Mini G, ZX Evolution + VDAC2)
SRC=$(wildcard src/*.c src/*.h src/*.s) $(wildcard vendor/*/*)
BINARY=FTVIEW.WMF
# python with intelhex, z80, Pillow (the tests): make venv
PYTHON?=$(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

all: $(BINARY)

$(BINARY): $(SRC) tools/build.py
	nice -n 19 python3 -I tools/build.py -o $@

venv:
	python3 -m venv .venv
	.venv/bin/pip install -q -r requirements.txt

test: $(BINARY)
	@if [ -d test ]; then for t in test/test_*.py; do $(PYTHON) $$t || exit 1; done; else echo "no tests yet"; fi

clean:
	rm -rf build

.PHONY: all venv test clean
