.PHONY: test test-python test-bridge public-check prompts

PYTHON ?= python3
GO ?= go

test: test-python test-bridge public-check

test-python:
	$(PYTHON) scripts/run_tests.py

test-bridge:
	cd bridge && $(GO) test -race ./...

public-check:
	$(PYTHON) scripts/generate_prompts.py --check
	$(PYTHON) scripts/check_public_tree.py

prompts:
	$(PYTHON) scripts/generate_prompts.py
