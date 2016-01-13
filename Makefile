#
# Copyright (C) 2014-2016 Canonical, Ltd.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#

VERSION=0.01.21

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"' -O2

BINDIR=/usr/sbin
MANDIR=/usr/share/man/man8

SRC = power-calibrate.c perf.c
OBJS = $(SRC:.c=.o)

power-calibrate: $(OBJS) Makefile perf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBJS) -lm -o $@ $(LDFLAGS)

power-calibrate.o: power-calibrate.c Makefile perf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c power-calibrate.c -o $@

perf.o: perf.c Makefile perf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c perf.c -o $@

power-calibrate.8.gz: power-calibrate.8
	gzip -c $< > $@

dist:
	rm -rf power-calibrate-$(VERSION)
	mkdir power-calibrate-$(VERSION)
	cp -rp Makefile scripts power-calibrate.c power-calibrate.8 perf.c perf.h COPYING power-calibrate-$(VERSION)
	tar -zcf power-calibrate-$(VERSION).tar.gz power-calibrate-$(VERSION)
	rm -rf power-calibrate-$(VERSION)

clean:
	rm -f power-calibrate $(OBJS) power-calibrate.8.gz
	rm -f power-calibrate-$(VERSION).tar.gz

install: power-calibrate power-calibrate.8.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp power-calibrate ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp power-calibrate.8.gz ${DESTDIR}${MANDIR}
