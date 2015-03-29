VERSION=0.01.12

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"'

BINDIR=/usr/sbin
MANDIR=/usr/share/man/man8

power-calibrate: power-calibrate.o
	$(CC) $(CFLAGS) $< -lm -o $@ $(LDFLAGS)

power-calibrate.8.gz: power-calibrate.8
	gzip -c $< > $@

dist:
	rm -rf power-calibrate-$(VERSION)
	mkdir power-calibrate-$(VERSION)
	cp -rp Makefile scripts power-calibrate.c power-calibrate.8 COPYING power-calibrate-$(VERSION)
	tar -zcf power-calibrate-$(VERSION).tar.gz power-calibrate-$(VERSION)
	rm -rf power-calibrate-$(VERSION)

clean:
	rm -f power-calibrate power-calibrate.o power-calibrate.8.gz
	rm -f power-calibrate-$(VERSION).tar.gz

install: power-calibrate power-calibrate.8.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp power-calibrate ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp power-calibrate.8.gz ${DESTDIR}${MANDIR}
