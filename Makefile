xclipd: src/xclipd.c
	$(CC) -Wall -Wextra -o $@ $< -lX11

install: xclipd
	install -Dm755 xclipd ~/.local/bin/xclipd

docker:
	docker build -t docker/xclipd .

.PHONY: docker install
