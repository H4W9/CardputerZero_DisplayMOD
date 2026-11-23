/*
 * displaymod-probe - check whether an ILI9341 is connected on SPI0 CS1.
 * Reads the panel's ID (command 0xD3, "Read ID4") over SDO/MISO.
 *
 *   displaymod-probe DC_GPIO RST_GPIO
 *   displaymod-probe vt TTY graphics|text   (set a console to graphics/text mode)
 *   exit 0 = panel found, 1 = no panel, 2 = could not probe (no spidev/gpio)
 *
 * Needs SDO (MISO) wired to header pin 11 (G9). Runs as root from
 * displaymod-ctl before the fbtft driver is loaded.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/kd.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int request_lines(int dc, int rst)
{
    int chip = open("/dev/gpiochip0", O_RDONLY);
    if (chip < 0) return -1;
    struct gpio_v2_line_request req;
    memset(&req, 0, sizeof req);
    req.offsets[0] = dc;
    req.offsets[1] = rst;
    req.num_lines = 2;
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    snprintf(req.consumer, sizeof req.consumer, "displaymod-probe");
    int r = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req);
    close(chip);
    return r < 0 ? -1 : req.fd;
}

static void set_lines(int fd, int dc, int rst)
{
    struct gpio_v2_line_values v = {0};
    v.mask = 3;
    v.bits = (dc ? 1 : 0) | (rst ? 2 : 0);
    ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

static int xfer(int spi, const uint8_t *tx, uint8_t *rx, int len, int keep_cs)
{
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof t);
    t.tx_buf = (uintptr_t)tx;
    t.rx_buf = (uintptr_t)rx;
    t.len = len;
    t.speed_hz = 1000000;
    t.bits_per_word = 8;
    t.cs_change = keep_cs;   /* on the last transfer: keep CS asserted */
    return ioctl(spi, SPI_IOC_MESSAGE(1), &t);
}

/* One uninterrupted transfer: command byte then n read bytes, DC held low.
 * The built-in screen shares this SPI bus, so a read split over two
 * transfers can be cut off by its traffic. The ILI9341 ignores DC while it
 * is sending read data, so keeping DC low is fine. */
static int read_atomic(int spi, int gpio, uint8_t cmd, uint8_t *out, int n)
{
    uint8_t tx[16] = {0}, rx[16] = {0};
    if (n > 15) n = 15;
    set_lines(gpio, 0, 1);
    tx[0] = cmd;
    if (xfer(spi, tx, rx, n + 1, 0) < 0) return -1;
    memcpy(out, rx + 1, n);
    return 0;
}

/* Undocumented ILI9341 index read (as Adafruit's readcommand8): select
 * parameter `index` with 0xD9, then read one byte of `cmd`. */
static int read_indexed(int spi, int gpio, uint8_t cmd, int index, uint8_t *out)
{
    uint8_t tx[2] = {0xD9, 0}, rx[2];
    set_lines(gpio, 0, 1);
    if (xfer(spi, tx, rx, 1, 0) < 0) return -1;
    set_lines(gpio, 1, 1);
    tx[0] = 0x10 + index;
    if (xfer(spi, tx, rx, 1, 0) < 0) return -1;
    uint8_t b[2];
    if (read_atomic(spi, gpio, cmd, b, 1) < 0) return -1;
    *out = b[0];
    return 0;
}

/* Some boards shift read data by one clock. Look for the 0x9341 ID at
 * every bit offset in the returned bytes. */
static int has_9341(const uint8_t *b, int n)
{
    for (int bit = 0; bit + 16 <= n * 8; bit++) {
        unsigned v = 0;
        for (int i = 0; i < 16; i++) {
            int k = bit + i;
            v = (v << 1) | ((b[k / 8] >> (7 - k % 8)) & 1);
        }
        if (v == 0x9341) return 1;
    }
    return 0;
}

/* Send one command byte, then read n bytes with CS held low throughout. */
static int read_cmd(int spi, int gpio, uint8_t cmd, uint8_t *out, int n)
{
    uint8_t tx[8] = {0}, rx[8] = {0};
    set_lines(gpio, 0, 1);                 /* DC low = command */
    tx[0] = cmd;
    if (xfer(spi, tx, rx, 1, 1) < 0) return -1;
    set_lines(gpio, 1, 1);                 /* DC high = data */
    memset(tx, 0, sizeof tx);
    if (xfer(spi, tx, out, n, 0) < 0) return -1;
    return 0;
}

/* Graphics mode stops the Linux console from drawing typed keys and kernel
 * messages over the launcher on the built-in screen. */
static int set_vt_mode(const char *tty, const char *mode)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/tty%d", atoi(tty));
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror(path); return 2; }
    int r = ioctl(fd, KDSETMODE, strcmp(mode, "graphics") == 0 ? KD_GRAPHICS : KD_TEXT);
    close(fd);
    return r == 0 ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "vt") == 0) return set_vt_mode(argv[2], argv[3]);
    if (argc != 3) { fprintf(stderr, "usage: %s DC RST\n", argv[0]); return 2; }
    int dc = atoi(argv[1]), rst = atoi(argv[2]);

    int spi = open("/dev/spidev0.1", O_RDWR);
    if (spi < 0) { fprintf(stderr, "probe: /dev/spidev0.1: %s\n", strerror(errno)); return 2; }
    uint8_t mode = SPI_MODE_0;
    ioctl(spi, SPI_IOC_WR_MODE, &mode);

    int gpio = request_lines(dc, rst);
    if (gpio < 0) { fprintf(stderr, "probe: gpio: %s\n", strerror(errno)); close(spi); return 2; }

    /* Hardware reset */
    set_lines(gpio, 1, 0); usleep(20000);
    set_lines(gpio, 1, 1); usleep(150000);

    uint8_t a[6] = {0}, id[4] = {0}, did[4] = {0}, x[4] = {0}, pm[2] = {0};
    int ok = 1;
    for (int i = 0; i < 4; i++) ok = read_indexed(spi, gpio, 0xD3, i, &x[i]) == 0 && ok;
    ok = read_atomic(spi, gpio, 0x0A, pm, 2) == 0 && ok;  /* power mode, 0x08 after reset */
    ok = read_atomic(spi, gpio, 0xD3, a, 6) == 0 && ok;   /* ID4: xx 00 93 41 */
    ok = read_cmd(spi, gpio, 0xD3, id, 4) == 0 && ok;    /* same, split read */
    ok = read_cmd(spi, gpio, 0x04, did, 4) == 0 && ok;   /* RDDID, fallback */

    close(gpio);
    close(spi);
    if (!ok) return 2;

    fprintf(stderr, "probe: D9=%02x %02x %02x %02x  0A=%02x %02x  D3a=%02x %02x %02x %02x %02x %02x  D3=%02x %02x %02x %02x  04=%02x %02x %02x %02x\n",
            x[0], x[1], x[2], x[3], pm[0], pm[1],
            a[0], a[1], a[2], a[3], a[4], a[5], id[0], id[1], id[2], id[3],
            did[0], did[1], did[2], did[3]);
    if (has_9341(x, 4) || has_9341(a, 6) || has_9341(id, 4)) return 0;
    /* Power mode answers with a few bits set; an open line reads 00 or FF */
    for (int i = 0; i < 2; i++) if (pm[i] != 0x00 && pm[i] != 0xFF) return 0;

    /* Some clones answer RDDID but not ID4. A floating/unwired MISO reads
     * all 0x00 (Pi pull-down) or all 0xFF. */
    int all0 = 1, allf = 1;
    for (int i = 1; i < 4; i++) { if (did[i]) all0 = 0; if (did[i] != 0xFF) allf = 0; }
    return (all0 || allf) ? 1 : 0;
}
