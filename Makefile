
VERSION = 0.1

PROG := init

OBJS := init.o

all: initrd

$(PROG): init.c Makefile
	gcc -Os -g -static init.c -o init -lc

initrd: $(PROG)
	mkdir -p initrd.in
	mkdir -p initrd.in/{dev,proc,sbin,sys,newroot}
	install -m0755 init initrd.in/sbin/init
	strip --strip-all initrd.in/sbin/init
	ln -s /sbin/init initrd.in/init
	(cd initrd.in ; find * | cpio -o --format='newc' ) > initrd.raw
	gzip -c initrd.raw > initrd && rm -rf initrd.raw initrd.in

install: initrd
	install -m0644 -u root -g root initrd /boot/initrd

clean:
	rm -rf initrd.in
	rm -f *~ *.o $(PROG) initrd initrd.raw

dist: clean
	git tag v$(VERSION) || :
	git archive --format=tar -v --prefix="initrd-$(VERSION)/" v$(VERSION) | \
		gzip > initrd-$(VERSION).tar.gz
