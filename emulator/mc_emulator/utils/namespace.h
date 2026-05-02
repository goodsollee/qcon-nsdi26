#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sched.h>

static int openTunInterface(const std::string& ifName) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }
    
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI; // Layer 3 mode, no packet info
    strncpy(ifr.ifr_name, ifName.c_str(), IFNAMSIZ - 1);
    
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }
    
    return fd;
}

static int saveHostNamespace() {
    // Open our current netns handle
    int fd = open("/proc/self/ns/net", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "saveHostNamespace() failed: %s\n", strerror(errno));
    }
    return fd;
}

static int enterNamespace(const std::string& nsName) {
    std::string path = "/var/run/netns/" + nsName;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "enterNamespace(%s): open failed: %s\n",
                nsName.c_str(), strerror(errno));
        return -1;
    }
    if (setns(fd, CLONE_NEWNET) < 0) {
        fprintf(stderr, "enterNamespace(%s): setns failed: %s\n",
                nsName.c_str(), strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int restoreNamespace(int fd) {
    if (fd < 0) {
        return -1;
    }
    if (setns(fd, CLONE_NEWNET) < 0) {
        fprintf(stderr, "restoreNamespace(): setns failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}
