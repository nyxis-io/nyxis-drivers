# Nyxis language drivers (MIT). Core compiler and conformance vectors: ../nyxis

CORE ?= ../nyxis
FIXTURE_DIR     ?= ../nyxis/bench/fixtures
FIXTURE_COUNT   ?= 1000
FIXTURE_OUT     := $(shell d="$(FIXTURE_DIR)"; mkdir -p "$$d" out/fixtures 2>/dev/null; \
	if touch "$$d/.nxs_wprobe" 2>/dev/null; then rm -f "$$d/.nxs_wprobe"; printf '%s' "$$d"; \
	elif touch out/fixtures/.nxs_wprobe 2>/dev/null; then rm -f out/fixtures/.nxs_wprobe; \
	echo "nyxis: not writable: $$d — using out/fixtures" 1>&2; printf '%s' out/fixtures; \
	else printf '%s' "$$d"; fi)
JAVA_HOME       ?= /opt/homebrew/opt/openjdk@21
DOTNET_FRAMEWORK ?= net10.0

.PHONY: lint fix test lint-js fix-js test-js lint-py fix-py test-py test-py-ci \
        lint-go fix-go test-go lint-ruby fix-ruby test-ruby test-ruby-ci \
        lint-php fix-php test-php test-php-ci lint-c fix-c test-c \
        lint-swift fix-swift test-swift lint-kotlin test-kotlin \
        lint-csharp fix-csharp test-csharp fixtures

lint: lint-js lint-py lint-go lint-ruby lint-php lint-c lint-swift lint-kotlin lint-csharp

fix: fix-js fix-py fix-go fix-ruby fix-php fix-c fix-swift fix-csharp

test: test-js test-py test-go test-ruby test-php test-c test-swift test-kotlin test-csharp

fixtures:
	$(MAKE) -C $(CORE) fixtures FIXTURE_DIR=$(abspath $(FIXTURE_DIR)) FIXTURE_COUNT=$(FIXTURE_COUNT)

lint-js:
	cd js && npm install --ignore-scripts --no-fund --no-audit
	cd js && npm run lint

fix-js:
	cd js && npm install --ignore-scripts --no-fund --no-audit
	cd js && npx eslint --fix --max-warnings 0 nxs.js nxs_writer.js wasm.js test.js test_wasm.js

test-js:
	node js/test.js $(FIXTURE_OUT)

lint-py:
	@command -v ruff >/dev/null 2>&1 || python3 -m pip install --user ruff
	cd py && ruff check --select E,W,F --ignore E501,E701,E702 .

fix-py:
	@command -v ruff >/dev/null 2>&1 || python3 -m pip install --user ruff
	cd py && ruff check --select E,W,F --ignore E501,E701,E702 --fix .

test-py:
	cd py && python test_nxs.py ../$(FIXTURE_OUT)

test-py-ci: test-py
	cd py && bash build_ext.sh
	cd py && python test_c_ext.py ../$(FIXTURE_OUT)

lint-go:
	@cd go && { fmt=$$(gofmt -l .); [ -z "$$fmt" ] || { printf 'run gofmt -w on:\n%s\n' "$$fmt"; exit 1; }; }
	cd go && go vet ./...

fix-go:
	cd go && gofmt -w .

test-go:
	cd go && go test ./...

lint-ruby:
	@command -v rubocop >/dev/null 2>&1 || gem install rubocop --no-document
	rubocop ruby/nxs.rb ruby/test.rb ruby/bench.rb --config ruby/.rubocop.yml --no-color --cache false

fix-ruby:
	@command -v rubocop >/dev/null 2>&1 || gem install rubocop --no-document
	rubocop ruby/nxs.rb ruby/test.rb ruby/bench.rb --config ruby/.rubocop.yml --no-color --cache false -A

test-ruby:
	ruby ruby/test.rb $(FIXTURE_OUT)

test-ruby-ci: test-ruby
	bash ruby/ext/build.sh
	ruby ruby/bench_c.rb $(FIXTURE_OUT)

lint-php:
	@command -v composer >/dev/null 2>&1 || { echo "Install Composer: https://getcomposer.org/" >&2; exit 1; }
	cd php && composer install --no-interaction --prefer-dist --no-progress
	cd php && ./vendor/bin/phpstan analyse Nxs.php --level=5 --no-progress

fix-php: lint-php

test-php:
	php php/test.php $(FIXTURE_OUT)

test-php-ci: test-php
	bash php/nxs_ext/build.sh
	php -d extension=php/nxs_ext/modules/nxs.so php/test.php $(FIXTURE_OUT)

lint-c:
	@command -v cppcheck >/dev/null 2>&1 || brew install cppcheck
	cppcheck --error-exitcode=1 --suppress=missingIncludeSystem c/nxs.c c/nxs.h

test-c:
	cd c && make test -s && ./test ../$(FIXTURE_OUT)

lint-swift:
	@command -v swiftlint >/dev/null 2>&1 || brew install swiftlint
	cd swift && swiftlint lint --strict --cache-path .swiftlint-cache Sources/NXS

fix-swift:
	@command -v swiftlint >/dev/null 2>&1 || brew install swiftlint
	cd swift && swiftlint --fix --strict --cache-path .swiftlint-cache Sources/NXS

test-swift:
	cd swift && swift run nxs-test ../$(FIXTURE_OUT)

lint-kotlin:
	cd kotlin && JAVA_HOME=$(JAVA_HOME) PATH="$(JAVA_HOME)/bin:$$PATH" ./gradlew ktlintCheck -q

test-kotlin:
	cd kotlin && JAVA_HOME=$(JAVA_HOME) PATH=$(JAVA_HOME)/bin:$$PATH ./gradlew run --args="../$(FIXTURE_OUT)" -q

lint-csharp:
	cd csharp && DOTNET_FRAMEWORK=$(DOTNET_FRAMEWORK) dotnet format nxs.csproj --verify-no-changes --severity warn

fix-csharp:
	cd csharp && DOTNET_FRAMEWORK=$(DOTNET_FRAMEWORK) dotnet format nxs.csproj

test-csharp:
	cd csharp && dotnet run -p:NxsTargetFramework=$(DOTNET_FRAMEWORK) -- ../$(FIXTURE_OUT)
