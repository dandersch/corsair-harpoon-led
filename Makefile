SYSTEMD_UNIT_DIR ?= /etc/systemd/system
UDEV_RULES_DIR ?= /etc/udev/rules.d

CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean install uninstall

all: corsair-harpoon-led-off

corsair-harpoon-led-off: corsair-harpoon-led-off.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f corsair-harpoon-led-off

install: corsair-harpoon-led-off
	install -Dm755 corsair-harpoon-led-off "$(DESTDIR)/usr/local/bin/corsair-harpoon-led-off"
	install -Dm644 corsair-harpoon-led-off@.service "$(DESTDIR)$(SYSTEMD_UNIT_DIR)/corsair-harpoon-led-off@.service"
	install -Dm644 99-corsair-harpoon-led-off.rules "$(DESTDIR)$(UDEV_RULES_DIR)/99-corsair-harpoon-led-off.rules"

uninstall:
	rm -f "$(DESTDIR)/usr/local/bin/corsair-harpoon-led-off"
	rm -f "$(DESTDIR)$(SYSTEMD_UNIT_DIR)/corsair-harpoon-led-off@.service"
	rm -f "$(DESTDIR)$(UDEV_RULES_DIR)/99-corsair-harpoon-led-off.rules"
