# Makefile for drbd
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

# TODO move some of the more cryptic bash scriptlets here into scripts/*
# and call those from here.	-- lge 

# for some reason some of the commands below only work correctly in bash,
# and not in e.g. dash. I'm too lazy to fix it to be compatible.
SHELL=/bin/bash

#PREFIX      = /usr/local

SUBDIRS     = user scripts documentation drbd #testing #benchmark
ALLSUBDIRS  = user scripts benchmark documentation drbd testing

REL_VERSION := $(shell sed -ne '/REL_VERSION/{s/^[^"]*"\([^ "]*\).*/\1/;p;q;}' drbd/linux/drbd_config.h)
ifdef FORCE
#
# NOTE to generate a tgz even if too lazy to update the changelogs,
# or to forcefully include the FIXME to be done: latest change date;
# for now, include the git hash of the latest commit
# in the tgz name:
#   make distclean doc tgz FORCE=1
#
REL_VERSION := $(REL_VERSION)-$(shell git-rev-parse HEAD)
endif

DIST_VERSION := $(subst -,_,$(REL_VERSION))
FDIST_VERSION := $(shell test -e .filelist && sed -ne 's,^drbd-\([^/]*\)/.*,\1,p;q' < .filelist)
ifeq ($(FDIST_VERSION),)
FDIST_VERSION := $(DIST_VERSION)
endif

LN_S = ln -s
RPMBUILD=rpmbuild

all:
	@ set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i ; done
	@ echo -e "\n\tBuild successful."

tools:
	@ set -e; for i in $(patsubst drbd,,$(SUBDIRS)); do $(MAKE) -C $$i ; done
	@ echo -e "\n\tBuild successful."

doc:
	$(MAKE) -C documentation doc

doc-clean:
	$(MAKE) -C documentation doc-clean

install:
	@ set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i install; done

install-tools:
	@ set -e; for i in $(patsubst drbd,,$(SUBDIRS)); do $(MAKE) -C $$i install; done

clean:
	@ set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i clean; done
	rm -f *~
	rm -rf dist

distclean:
	@ set -e; for i in $(ALLSUBDIRS); do $(MAKE) -C $$i distclean; done
	rm -f *~ .filelist
	rm -rf dist

uninstall:
	@ set -e; for i in $(SUBDIRS); do $(MAKE) -C $$i uninstall; done

check_changelogs_up2date:
	@ up2date=true; dver_re=$(DIST_VERSION); dver_re=$${dver_re//./\\.}; \
	echo "checking for presence of $$dver_re in various changelog files"; \
	in_changelog=$$(sed -n -e '0,/^%changelog/d' \
	                     -e '/^- *drbd ('"$$dver_re"'-/p' \
	                     -e '/^\*.* \['"$$dver_re"'-/p' < drbd.spec.in) ; \
	if test -z "$$in_changelog" ; \
	then \
	   echo "You need to update the %changelog in drbd.spec.in"; \
	   up2date=false; fi; \
	if ! grep "^$$dver_re\>" >/dev/null 2>&1 ChangeLog; \
	then \
	   echo "You need to update ChangeLog"; \
	   up2date=false; fi ; \
	if ! grep "^drbd ($$dver_re-" >/dev/null 2>&1 debian/changelog; \
	then \
	   echo -e "\n\n\tdebian/changelog needs some update\n"; \
	   : do not fail the build because of outdated debian/changelog ; fi ; \
	$$up2date

# XXX this is newly created whenever the toplevel makefile does something.
# however it is NOT updated when you just do a make in user/ or drbd/ ...
#
# update of drbd_buildtag.c is forced:
.PHONY: drbd/drbd_buildtag.c
drbd/drbd_buildtag.c:
	$(MAKE) -C drbd drbd_buildtag.c

# update of .filelist is forced:
.PHONY: .filelist
.filelist:
	@git-ls-files | sed '$(if $(PRESERVE_DEBIAN),,/^debian/d);s#^#drbd-$(DIST_VERSION)/#' > .filelist
	@[ -s .filelist ] # assert there is something in .filelist now
	@find documentation -name "[^.]*.[58]" -o -name "*.html" | \
	sed "s/^/drbd-$(DIST_VERSION)\//" >> .filelist           ;\
	echo drbd-$(DIST_VERSION)/drbd_config.h >> .filelist     ;\
	echo drbd-$(DIST_VERSION)/drbd/drbd_buildtag.c >> .filelist ;\
	echo drbd-$(DIST_VERSION)/.filelist >> .filelist         ;\
	echo "./.filelist updated."

# tgz will no longer automatically update .filelist,
# so the tgz and therefore rpm target will work within
# an extracted tarball, too.
# to generate a distribution tarball, use make tarball,
# which will regenerate .filelist 
tgz:
	test -e .filelist
	ln -sf drbd/linux/drbd_config.h drbd_config.h
	rm -f drbd-$(FDIST_VERSION)
	ln -s . drbd-$(FDIST_VERSION)
	for f in $$(<.filelist) ; do [ -e $$f ] && continue ; echo missing: $$f ; exit 1; done
	grep debian .filelist >/dev/null 2>&1 && _DEB=-debian || _DEB="" ; \
	tar --owner=0 --group=0 -czf - -T .filelist > drbd-$(FDIST_VERSION)$$_DEB.tar.gz
	rm drbd-$(FDIST_VERSION)

ifeq ($(FORCE),)
tgz: check_changelogs_up2date doc
endif

check_all_committed:
	@$(if $(FORCE),-,)modified=`git-ls-files -m -t`; 		\
	if test -n "$$modified" ; then	\
		echo "$$modified";	\
	       	false;			\
	fi

prepare_release:
	$(MAKE) tarball
	$(MAKE) tarball PRESERVE_DEBIAN=1

tarball: check_all_committed distclean doc .filelist
	$(MAKE) tgz

all tools doc .filelist: drbd/drbd_buildtag.c

export KDIR KVER O
KDIR := $(shell echo /lib/modules/`uname -r`/build)
KVER := $(shell KDIR=$(KDIR) O=$(O) scripts/get_uts_release.sh)

kernel-patch: drbd/drbd_buildtag.c
	set -o errexit; \
	kbase=$$(basename $(KDIR)); \
	d=patch-$$kbase-drbd-$(DIST_VERSION); \
	test -e $$d && cp -fav --backup=numbered $$d $$d; \
	bash scripts/patch-kernel $(KDIR) . > $$d

rpm: tgz
	@if [ -z "$(KVER)" ]; then \
		echo "Could not determine uts_release" ; \
		false ; \
	fi
	mkdir -p dist/BUILD \
	         dist/RPMS  \
	         dist/SPECS \
	         dist/SOURCES \
	         dist/TMP \
	         dist/install \
	         dist/SRPMS
	[ -h dist/SOURCES/drbd-$(FDIST_VERSION).tar.gz ] || \
	  $(LN_S) $(PWD)/drbd-$(FDIST_VERSION).tar.gz \
	          $(PWD)/dist/SOURCES/drbd-$(FDIST_VERSION).tar.gz
	if test drbd.spec.in -nt dist/SPECS/drbd.spec ; then \
	   sed -e "s/^\(Version:\).*/\1 $(FDIST_VERSION)/;" \
	       -e "s/^\(Packager:\).*/\1 $(USER)@$(HOSTNAME)/;" < drbd.spec.in \
	   > dist/SPECS/drbd.spec ; \
	fi
	$(RPMBUILD) -bb \
	    --define "_topdir $(PWD)/dist" \
	    --define "buildroot $(PWD)/dist/install" \
	    --define "kernelversion $(KVER)" \
	    --define "kdir $(KDIR)" \
	    $(RPMOPT) \
	    $(PWD)/dist/SPECS/drbd.spec
	@echo "You have now:" ; ls -l dist/*RPMS/*/*.rpm


# the INSTALL file is writen in lge-markup, which is basically
# wiki style plus some "conventions" :)
# why don't I write in or convert to HTML directly?
# editing INSTALL feels more natural this way ...

INSTALL.html: INSTALL.pod
	-pod2html --title "Howto Build and Install DRBD" \
		< INSTALL.pod > INSTALL.html ; rm -f pod2htm*
#	-w3m -T text/html -dump < INSTALL.html > INSTALL.txt

INSTALL.pod: INSTALL
	-@perl -pe 'BEGIN { print "=pod\n\n"; };                  \
	 	s/^= +(.*?) +=$$/=head1 $$1/;                     \
	 	s/^== +(.*?) +==$$/=head2 $$1/;                   \
		s/^(Last modi)/ $$1/ and next;                    \
	 	if(s/^ +([^#]*)$$/$$1/ or /^\S/) {                \
			s/(Note:)/B<$$1>/g;                       \
			s/"([^"]+)"/C<$$1>/g;                     \
	 		s,((^|[. ])/(`[^`]*`|\S)+),C<$$1>,g;      \
	 	}' \
	 	< INSTALL > INSTALL.pod
