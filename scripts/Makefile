# Makefile for scripts
#
# This file is part of drbd by Philipp Reisner
#
# drbd is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# drbd is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with drbd; see the file COPYING.  If not, write to
# the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
#
# for Debian:
#   update-rc.d drbd defaults 70 08
#

SCRIPT=drbd
DIST            :=$(strip \
  $(if $(wildcard /etc/gentoo-release),gentoo,\
  $(if $(wildcard /etc/redhat-release),redhat,\
  $(if $(wildcard /etc/slackware-version),slackware,\
  $(if $(wildcard /sbin/rcsyslog),suselike,)))))

ifeq ($(DIST),slackware)
  INITD=$(PREFIX)/etc/rc.d/
endif

ifeq ($(DIST),gentoo)
  INITD=$(PREFIX)/etc/init.d/
  SCRIPT=drbd.gentoo
endif

ifeq ($(DIST),redhat)
  INITD=$(PREFIX)/etc/rc.d/init.d/
endif

ifeq ($(INITD),)
   INITD=$(PREFIX)/etc/init.d/
   $(warning No special distribution INITD)
   $(warning setting INITD=$(INITD) according to LSB)
endif

all:

install:
	mkdir -p $(PREFIX)/etc/ha.d/resource.d
	install -d $(INITD)
	install -m 755 $(SCRIPT) $(INITD)drbd
	@ if [ ! -e $(PREFIX)/etc/drbd.conf ]; then \
		install -d $(PREFIX)/etc/; \
		install -m 644 drbd.conf $(PREFIX)/etc/; \
	fi
	install -m 755 drbddisk $(PREFIX)/etc/ha.d/resource.d
ifeq ($(DIST),suselike)
	ln -sf ../etc/init.d/drbd $(PREFIX)/sbin/rcdrbd
endif
	@ echo
	@ echo "Don't forget to run update-rc.d or chkconfig"

clean:
	rm -f *~
	rm -f datadisk

distclean: clean

uninstall:
	rm $(INITD)drbd
	rm $(PREFIX)/etc/ha.d/resource.d/drbddisk
	! test -L $(PREFIX)/sbin/rcdrbd || rm $(PREFIX)/sbin/rcdrbd
