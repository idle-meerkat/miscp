#include <stdio.h>
#include <stddef.h>
#include <fcntl.h>

#include <asm/termios.h>

int ioctl(int fd, unsigned long request, ...);

int setbaud(int fd, unsigned long speed) {
        struct termios2 tio = {0};

        if (ioctl(fd, TCGETS2, &tio) < 0)
                return -1;
        tio.c_cflag &= ~CBAUD;
        tio.c_cflag |= BOTHER;
        tio.c_ispeed = (speed_t)speed;
        tio.c_ospeed = (speed_t)speed;
        return ioctl(fd, TCSETS2, &tio);
}

int getbaud(int fd, unsigned long *ispeed, unsigned long *ospeed) {
        struct termios2 tio = {0};

        if (ioctl(fd, TCGETS2, &tio) < 0)
                return -1;

        *ispeed = tio.c_ispeed;
        *ospeed = tio.c_ospeed;

        return 0;
}

int main(int argc, char **argv) {
        int fd;
        unsigned long speed;
        const char *dev_str = NULL;
        const char *speed_str = NULL;

        if (argc < 2)
                return -1;

        dev_str = argv[1];

        if (argc > 2)
                speed_str = argv[2];

        fd = open(dev_str, O_RDWR);
        if (fd < 0) {
                perror("open");
                return -1;
        }

        if (!speed_str) {
                unsigned long ispeed, ospeed;
                if (getbaud(fd, &ispeed, &ospeed) < 0) {
                        perror("getbaud");
                        return -1;
                }
                printf("%lu %lu", ispeed, ospeed);
                return 0;
        }

        if (sscanf(speed_str, "%lu", &speed) < 1) {
                fprintf(stderr, "bad speed: %s\n", speed_str);
                return -1;
        }

        if (setbaud(fd, speed)) {
                perror("setbaud");
                return -1;
        }

        return 0;
}
