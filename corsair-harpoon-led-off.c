#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

enum {
    CORSAIR_VENDOR_ID = 0x1b1c,
    HARPOON_PRODUCT_ID = 0x1b5e,
    REPORT_SIZE = 65,
    RESPONSE_SIZE = 64,
};

static volatile sig_atomic_t stopping;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int wait_for_fd(int fd, short events, int timeout_ms)
{
    struct pollfd poll_fd = { .fd = fd, .events = events };
    int result;

    do {
        result = poll(&poll_fd, 1, timeout_ms);
    } while (result < 0 && errno == EINTR && !stopping);

    if (result <= 0) {
        if (result == 0)
            errno = ETIMEDOUT;
        return -1;
    }
    if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static void drain_responses(int fd)
{
    uint8_t response[RESPONSE_SIZE];

    while (read(fd, response, sizeof(response)) > 0)
        ;
}

static int command(int fd, const uint8_t *payload, size_t payload_size,
                   int accept_device_error)
{
    uint8_t report[REPORT_SIZE] = { 0 };
    uint8_t response[RESPONSE_SIZE];
    struct timespec deadline;
    ssize_t count;

    if (payload_size > REPORT_SIZE - 1) {
        errno = EINVAL;
        return -1;
    }

    memcpy(report + 1, payload, payload_size);
    drain_responses(fd);

    count = write(fd, report, sizeof(report));
    if (count != (ssize_t)sizeof(report)) {
        if (count >= 0)
            errno = EIO;
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
        return -1;
    deadline.tv_sec++;

    for (;;) {
        struct timespec now;
        int timeout_ms;

        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
            return -1;
        timeout_ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 +
                           (deadline.tv_nsec - now.tv_nsec + 999999) / 1000000);
        if (timeout_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (wait_for_fd(fd, POLLIN, timeout_ms) < 0)
            return -1;
        count = read(fd, response, sizeof(response));
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (count != RESPONSE_SIZE || response[0] != 0x00 ||
            response[1] != payload[1])
            continue;
        break;
    }

    if (response[2] != 0 && !accept_device_error) {
        fprintf(stderr, "Corsair command 0x%02x failed with device status 0x%02x\n",
                payload_size > 1 ? payload[1] : 0, response[2]);
        errno = EPROTO;
        return -1;
    }
    return response[2];
}

static int start_lighting_transaction(int fd, uint8_t *endpoint)
{
    static const uint8_t stop_transaction[] = { 0x08, 0x05, 0x01, 0x00 };
    uint8_t start_transaction[] = { 0x08, 0x0d, 0x00, *endpoint };
    int status;

    for (;;) {
        status = command(fd, start_transaction, sizeof(start_transaction), 1);
        if (status < 0)
            return -1;
        if (status == 0)
            return 0;

        if (status == 0x03) {
            /* A previous process may have left the lighting handle open. */
            (void)command(fd, stop_transaction, sizeof(stop_transaction), 1);
            status = command(fd, start_transaction, sizeof(start_transaction), 1);
            if (status < 0)
                return -1;
            if (status == 0)
                return 0;
        }

        if (*endpoint == 0x22) {
            *endpoint = 0x01;
            start_transaction[3] = *endpoint;
            continue;
        }

        fprintf(stderr, "Opening lighting endpoint 0x%02x failed with device status 0x%02x\n",
                *endpoint, status);
        errno = EPROTO;
        return -1;
    }
}

static int turn_leds_off(int fd, uint8_t *endpoint)
{
    static const uint8_t software_mode[] = { 0x08, 0x01, 0x03, 0x00, 0x02, 0x00 };
    static const uint8_t lighting_control[] = { 0x08, 0x02, 0x5f, 0x00, 0x00 };
    static const uint8_t stop_transaction[] = { 0x08, 0x05, 0x01, 0x00 };
    static const uint8_t black[] = {
        0x08, 0x06, 0x00, 0x06, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    int update_result;
    int saved_errno;

    if (command(fd, software_mode, sizeof(software_mode), 0) < 0 ||
        command(fd, lighting_control, sizeof(lighting_control), 0) < 0)
        return -1;

    if (start_lighting_transaction(fd, endpoint) < 0)
        return -1;

    update_result = command(fd, black, sizeof(black), 0);
    saved_errno = errno;
    if (command(fd, stop_transaction, sizeof(stop_transaction), 0) < 0 &&
        update_result >= 0)
        return -1;
    if (update_result < 0) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int validate_device(int fd)
{
    struct hidraw_devinfo info = { 0 };
    struct hidraw_report_descriptor descriptor = { 0 };
    int descriptor_size;

    if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0)
        return -1;
    if (info.vendor != CORSAIR_VENDOR_ID || info.product != HARPOON_PRODUCT_ID) {
        fprintf(stderr, "Refusing non-Harpoon device %04hx:%04hx\n",
                info.vendor, info.product);
        errno = ENODEV;
        return -1;
    }

    if (ioctl(fd, HIDIOCGRDESCSIZE, &descriptor_size) < 0)
        return -1;
    if (descriptor_size <= 0 || descriptor_size > HID_MAX_DESCRIPTOR_SIZE) {
        errno = EPROTO;
        return -1;
    }
    descriptor.size = (uint32_t)descriptor_size;
    if (ioctl(fd, HIDIOCGRDESC, &descriptor) < 0)
        return -1;

    /* Interface 1 starts with Usage Page 0xff42, Usage 1. */
    if (descriptor_size < 7 || descriptor.value[0] != 0x06 ||
        descriptor.value[1] != 0x42 || descriptor.value[2] != 0xff ||
        descriptor.value[3] != 0x09 || descriptor.value[4] != 0x01) {
        fprintf(stderr, "Refusing Harpoon hidraw interface without usage ff42:0001\n");
        errno = ENODEV;
        return -1;
    }
    return 0;
}

static void sleep_interruptibly(time_t seconds)
{
    struct timespec remaining = { .tv_sec = seconds };

    while (!stopping && nanosleep(&remaining, &remaining) < 0 && errno == EINTR)
        ;
}

int main(int argc, char **argv)
{
    struct sigaction action = { .sa_handler = handle_signal };
    const char *path;
    uint8_t lighting_endpoint = 0x22;
    int fd;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s /dev/hidrawN\n", argv[0]);
        return 2;
    }
    path = argv[1];

    fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    if (validate_device(fd) < 0) {
        perror("device validation");
        close(fd);
        return 1;
    }

    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    while (!stopping) {
        if (turn_leds_off(fd, &lighting_endpoint) == 0) {
            sleep_interruptibly(45);
            continue;
        }

        if (errno == ENODEV || errno == ENXIO || errno == EPIPE) {
            perror("mouse disconnected");
            close(fd);
            return 1;
        }
        perror("turning LEDs off");
        sleep_interruptibly(2);
    }

    close(fd);
    return 0;
}
