EXTRA_DIST = \
    c-constants-gen.py \
    check-coding-style.mk \
    check-c-style.sh \
    check-misc.sh \
    check-whitespace.sh \
    doc-generator.xsl \
    glib-client-gen.py \
    glib-client-marshaller-gen.py \
    glib-errors-enum-body-gen.py \
    glib-errors-enum-header-gen.py \
    glib-ginterface-gen.py \
    glib-gtypes-generator.py \
    glib-interfaces-gen.py \
    glib-signals-marshal-gen.py \
    gobject-foo.py \
    identity.xsl \
    libtpcodegen.py \
    libglibcodegen.py \
    make-version-script.py \
    with-session-bus.sh

CLEANFILES = libtpcodegen.pyc libtpcodegen.pyo libglibcodegen.pyc libglibcodegen.pyo $(noinst_SCRIPTS)

all: $(EXTRA_DIST)

libglibcodegen.py: libtpcodegen.py
	touch $@
c-constants-gen.py: libglibcodegen.py
	touch $@
glib-client-marshaller-gen.py: libglibcodegen.py
	touch $@
glib-errors-enum-body-gen.py: libglibcodegen.py
	touch $@
glib-errors-enum-header-gen.py: libglibcodegen.py
	touch $@
glib-ginterface-gen.py: libglibcodegen.py
	touch $@
glib-gtypes-generator.py: libglibcodegen.py
	touch $@
glib-interfaces-gen.py: libglibcodegen.py
	touch $@
glib-signals-marshal-gen.py: libglibcodegen.py
	touch $@

TELEPATHY_GLIB_SRCDIR = $(top_srcdir)/../telepathy-glib
maintainer-update-from-telepathy-glib:
	set -e && cd $(srcdir) && \
	for x in $(EXTRA_DIST); do \
		if test -f $(TELEPATHY_GLIB_SRCDIR)/tools/$$x; then \
			cp $(TELEPATHY_GLIB_SRCDIR)/tools/$$x $$x; \
		fi; \
	done
