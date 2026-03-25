BINARY := fan_control
INSTALL_DIR := /usr/local/bin
CFLAGS := -O2 -Wall -Wextra -Wno-unused-parameter

$(BINARY): fan_control.c
	clang $(CFLAGS) -o $@ $< -framework IOKit -framework CoreFoundation

clean:
	rm -f $(BINARY)

install: $(BINARY)
	sudo install -o root -g wheel -m 0755 -d $(INSTALL_DIR)
	sudo install -o root -g wheel -m 0755 $(BINARY) $(INSTALL_DIR)/$(BINARY)

.PHONY: clean install
