NAME := abhpc-lbs
VERSION := 1.1.0
CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
SYSTEMD_UNITDIR ?= /usr/lib/systemd/system
RPMBUILD ?= rpmbuild
RPM_TOPDIR ?= $(CURDIR)/build/rpm
CONTAINER_ENGINE ?= docker
PYTHON ?= python3
SOURCES := src/main.c src/formula.c src/history.c
DISTFILES := Makefile README.md README.en.md LICENSE abhpc-lbs.conf src tests packaging

.PHONY: all test install clean dist rpm srpm rpm-el7 rpm-el8
all: $(NAME)

$(NAME): $(SOURCES) src/formula.h src/history.h Makefile
	$(CC) $(CPPFLAGS) -DVERSION='"$(VERSION)"' -DCONFIG_PATH='"$(SYSCONFDIR)/$(NAME).conf"' $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS) -lm

build/test-history: tests/test_history.c src/history.c src/history.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) -o $@ tests/test_history.c src/history.c $(LDLIBS) -lm

test: $(NAME) build/test-history
	./build/test-history
	$(PYTHON) -m unittest discover -s tests -v

install: $(NAME)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(SYSCONFDIR) $(DESTDIR)$(SYSTEMD_UNITDIR)
	install -d -m 0750 $(DESTDIR)/var/lib/$(NAME)
	install -m 0755 $(NAME) $(DESTDIR)$(BINDIR)/$(NAME)
	@test -e $(DESTDIR)$(SYSCONFDIR)/$(NAME).conf || install -m 0644 $(NAME).conf $(DESTDIR)$(SYSCONFDIR)/$(NAME).conf
	sed 's|@BINDIR@|$(BINDIR)|g; s|@SYSCONFDIR@|$(SYSCONFDIR)|g' packaging/$(NAME).service.in > $(DESTDIR)$(SYSTEMD_UNITDIR)/$(NAME).service
	chmod 0644 $(DESTDIR)$(SYSTEMD_UNITDIR)/$(NAME).service
	install -m 0644 packaging/$(NAME).timer $(DESTDIR)$(SYSTEMD_UNITDIR)/$(NAME).timer

dist:
	mkdir -p dist
	tar --exclude=__pycache__ --transform 's,^,$(NAME)-$(VERSION)/,' -czf dist/$(NAME)-$(VERSION).tar.gz $(DISTFILES)

rpm: dist
	mkdir -p $(RPM_TOPDIR)/SOURCES
	cp dist/$(NAME)-$(VERSION).tar.gz $(RPM_TOPDIR)/SOURCES/
	$(RPMBUILD) --define '_topdir $(RPM_TOPDIR)' -ba packaging/$(NAME).spec

srpm: dist
	mkdir -p $(RPM_TOPDIR)/SOURCES
	cp dist/$(NAME)-$(VERSION).tar.gz $(RPM_TOPDIR)/SOURCES/
	$(RPMBUILD) --define '_topdir $(RPM_TOPDIR)' -bs packaging/$(NAME).spec

rpm-el7 rpm-el8: dist
	$(CONTAINER_ENGINE) build -f packaging/Dockerfile.$(@:rpm-%=%) -t $(NAME)-build:$(@:rpm-%=%) packaging
	mkdir -p build/rpm-$(@:rpm-%=%)
	$(CONTAINER_ENGINE) run --rm --user "$$(id -u):$$(id -g)" \
		-v "$(CURDIR)/dist:/sources:ro" -v "$(CURDIR)/packaging:/packaging:ro" \
		-v "$(CURDIR)/build/rpm-$(@:rpm-%=%):/rpmbuild" $(NAME)-build:$(@:rpm-%=%) \
		bash -ec 'mkdir -p /rpmbuild/SOURCES; cp /sources/$(NAME)-$(VERSION).tar.gz /rpmbuild/SOURCES/; rpmbuild --define "_topdir /rpmbuild" -ba /packaging/$(NAME).spec'

clean:
	rm -f $(NAME)
	rm -rf build dist tests/__pycache__
