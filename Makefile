
VERSION = 0.1

PROG := init

OBJS := init.o

all: $(PROG)

$(PROG): init.c Makefile
	gcc -Os -g -static init.c -o init -lc

initrd: $(PROG)
	# construct the initrd
	mkdir -p initrd.in
	mkdir -p initrd.in/{dev,proc,sbin,sys,newroot}
	#mknod initrd.in/dev/console c 5 1
	#mknod initrd.in/dev/null c 1 3
	#ln -s /proc/self/fd initrd.in/dev/fd
	#ln -s fd/0 initrd.in/dev/stdin
	#ln -s fd/1 initrd.in/dev/stdout
	#ln -s fd/2 initrd.in/dev/stderr
	cp init initrd.in/sbin/init
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
