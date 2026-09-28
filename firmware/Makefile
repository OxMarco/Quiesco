.PHONY: build build-debug build-trace check licences smoke test verify

build:
	./scripts/build.sh

build-debug:
	./scripts/build.sh --debug

build-trace:
	./scripts/build.sh --debug --trace

check:
	./scripts/check-source-policy.sh

licences:
	./scripts/collect-licences.sh

smoke:
	./scripts/build-smoke.sh

test:
	./scripts/run-host-tests.sh

verify: check test
